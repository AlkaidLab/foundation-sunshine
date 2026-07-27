/**
 * @file src/webhook/webhook.cpp
 * @brief Isolated asynchronous Webhook delivery runtime.
 */

#include "webhook.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <charconv>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <Simple-Web-Server/client_http.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/uuid.h"
#include "webhook_format.h"
#include "webhook_httpclient.h"
#include "webhook_httpsclient.h"

using namespace std::literals;

namespace webhook {
  namespace {
    constexpr std::size_t MAX_PENDING_DELIVERIES = 64;
    constexpr std::size_t MAX_IN_FLIGHT_DELIVERIES = 2;
    constexpr std::size_t MAX_RESPONSE_BYTES = 16 * 1024;
    constexpr std::size_t MAX_HEADER_VALUE_BYTES = 512;
    constexpr std::size_t MAX_EVENTS_PER_MINUTE = 20;
    constexpr int MAX_PRODUCTION_ATTEMPTS = 3;
    constexpr auto RATE_LIMIT_WINDOW = 1min;
    constexpr auto MAX_RETRY_AFTER = 60s;
    std::atomic<std::shared_ptr<const configuration_t>> active_configuration {
      std::make_shared<const configuration_t>()
    };

    bool use_chinese_content() {
      return config::sunshine.locale == "zh"sv || config::sunshine.locale == "zh_TW"sv;
    }

    struct parsed_url_t {
      bool https = false;
      std::string server;
      std::string target;
    };

    struct delivery_t {
      settings_t settings;
      parsed_url_t destination;
      std::string payload;
      std::string id;
      int event_id = -1;
      std::string event_type;
      bool production = true;
      completion_handler_t completion;
      int attempt = 0;
      int max_attempts = 1;
      bool finalized = false;
      std::chrono::steady_clock::time_point accepted_at;
    };

    bool get_url_part(CURLU *url, CURLUPart part, std::string &value, unsigned int flags = 0) {
      char *raw = nullptr;
      if (curl_url_get(url, part, &raw, flags) != CURLUE_OK) {
        return false;
      }

      value.assign(raw);
      curl_free(raw);
      return true;
    }

    bool has_url_part(CURLU *url, CURLUPart part) {
      char *raw = nullptr;
      const auto result = curl_url_get(url, part, &raw, 0);
      if (raw) {
        curl_free(raw);
      }
      return result == CURLUE_OK;
    }

    bool parse_webhook_url(const std::string &value, parsed_url_t &parsed) {
      if (value.empty() ||
          value.size() > MAX_URL_SIZE ||
          std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c <= 0x20U || c == 0x7fU;
          })) {
        return false;
      }

      const auto authority_marker = value.find("://");
      if (authority_marker == std::string::npos) {
        return false;
      }
      const auto authority_begin = authority_marker + 3;
      if (authority_begin >= value.size() ||
          value[authority_begin] == '/' ||
          value[authority_begin] == '?' ||
          value[authority_begin] == '#') {
        return false;
      }

      const auto cleanup = [](CURLU *url) {
        if (url) {
          curl_url_cleanup(url);
        }
      };
      std::unique_ptr<CURLU, decltype(cleanup)> url(curl_url(), cleanup);
      if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK) {
        return false;
      }

      std::string scheme;
      std::string host;
      if (!get_url_part(url.get(), CURLUPART_SCHEME, scheme) ||
          !get_url_part(url.get(), CURLUPART_HOST, host) || host.empty()) {
        return false;
      }

      std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (scheme != "http" && scheme != "https") {
        return false;
      }

      // Credentials in a Webhook URL can leak through error paths and are not
      // needed because the existing configuration has no userinfo contract.
      if (has_url_part(url.get(), CURLUPART_USER) || has_url_part(url.get(), CURLUPART_PASSWORD)) {
        return false;
      }

      std::string port;
      std::string path;
      std::string query;
      get_url_part(url.get(), CURLUPART_PORT, port);
      get_url_part(url.get(), CURLUPART_PATH, path);
      const bool has_query = get_url_part(url.get(), CURLUPART_QUERY, query);

      if (path.empty()) {
        path = "/";
      }
      if (path.front() != '/') {
        path.insert(path.begin(), '/');
      }
      if (has_query) {
        path += '?' + query;
      }

      if (host.find(':') != std::string::npos && !(host.front() == '[' && host.back() == ']')) {
        host = '[' + host + ']';
      }

      parsed.https = scheme == "https";
      parsed.server = port.empty() ? host : host + ':' + port;
      parsed.target = std::move(path);
      return true;
    }

    std::string normalize_header_value(const std::string &value) {
      std::string sanitized;
      sanitized.reserve(std::min(value.size(), MAX_HEADER_VALUE_BYTES));
      for (const unsigned char c : value) {
        if (sanitized.size() >= MAX_HEADER_VALUE_BYTES) {
          break;
        }
        sanitized.push_back(c < 0x20U || c == 0x7fU ? '_' : static_cast<char>(c));
      }
      return sanitized;
    }

    long timeout_seconds(std::chrono::milliseconds timeout) noexcept {
      const auto milliseconds = std::max<std::int64_t>(1, timeout.count());
      const auto seconds = milliseconds / 1000 + (milliseconds % 1000 != 0 ? 1 : 0);
      return static_cast<long>(std::min<std::int64_t>(seconds, (std::numeric_limits<long>::max)()));
    }

    int parse_status_code(const std::string &status_line) noexcept {
      const auto separator = status_line.find(' ');
      const char *begin = status_line.data();
      const char *end = begin + (separator == std::string::npos ? status_line.size() : separator);
      int status = 0;
      const auto result = std::from_chars(begin, end, status);
      return result.ec == std::errc {} && result.ptr == end ? status : 0;
    }

    std::optional<std::time_t> parse_http_date(const std::string &value) {
      static constexpr const char *HTTP_DATE_FORMATS[] {
        "%a, %d %b %Y %H:%M:%S GMT",  // IMF-fixdate
        "%A, %d-%b-%y %H:%M:%S GMT",  // obsolete RFC 850 date
        "%a %b %d %H:%M:%S %Y",       // obsolete ANSI C asctime date
      };

      for (const char *format : HTTP_DATE_FORMATS) {
        std::string normalized = value;
        if (format == HTTP_DATE_FORMATS[2] && normalized.size() == 24 && normalized[8] == ' ') {
          normalized[8] = '0';
        }
        std::tm parsed {};
        std::istringstream stream(normalized);
        stream.imbue(std::locale::classic());
        stream >> std::get_time(&parsed, format);
        if (stream.fail()) {
          continue;
        }
        stream >> std::ws;
        if (!stream.eof()) {
          continue;
        }

#ifdef _WIN32
        const auto timestamp = _mkgmtime(&parsed);
#else
        const auto timestamp = timegm(&parsed);
#endif
        if (timestamp != static_cast<std::time_t>(-1)) {
          return timestamp;
        }
      }
      return std::nullopt;
    }

    std::optional<std::chrono::seconds> parse_retry_after(const SimpleWeb::CaseInsensitiveMultimap &headers) noexcept {
      const auto it = headers.find("Retry-After");
      if (it == headers.end() || it->second.empty()) {
        return std::nullopt;
      }

      std::uint64_t seconds = 0;
      const auto result = std::from_chars(it->second.data(), it->second.data() + it->second.size(), seconds);
      if (result.ec == std::errc {} && result.ptr == it->second.data() + it->second.size()) {
        const auto capped = std::min<std::uint64_t>(seconds, MAX_RETRY_AFTER.count());
        return std::chrono::seconds(capped);
      }

      try {
        const auto timestamp = parse_http_date(it->second);
        if (!timestamp) {
          return std::nullopt;
        }

        const auto target = std::chrono::system_clock::from_time_t(*timestamp);
        const auto remaining = target - std::chrono::system_clock::now();
        if (remaining <= std::chrono::system_clock::duration::zero()) {
          return 0s;
        }

        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        const auto rounded_seconds = milliseconds / 1000 + (milliseconds % 1000 != 0 ? 1 : 0);
        return std::chrono::seconds(std::min<std::int64_t>(rounded_seconds, MAX_RETRY_AFTER.count()));
      }
      catch (...) {
        return std::nullopt;
      }
    }

    bool is_retryable_status(int status) noexcept {
      switch (status) {
        case 408:
        case 429:
        case 500:
        case 502:
        case 503:
        case 504:
          return true;
        default:
          return false;
      }
    }

    bool is_success_status(int status) noexcept {
      return status >= 200 && status < 300;
    }

    std::string generate_signature(long long timestamp, const std::string &hostname) {
      const std::string data = hostname + std::to_string(timestamp) + "Sunshine_Foundation";
      return std::to_string(std::hash<std::string> {}(data));
    }

    SimpleWeb::CaseInsensitiveMultimap generate_webhook_headers(
      const std::string &delivery_id,
      int event_id,
      const std::string &event_type
    ) {
      const auto now = std::chrono::system_clock::now();
      const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      const std::string hostname = normalize_header_value(platf::get_host_name());

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json; charset=utf-8");
      headers.emplace("Connection", "close");
      headers.emplace("User-Agent", "Sunshine_Foundation/1.0 (System Notification Service)");
      headers.emplace("X-Webhook-Delivery", delivery_id);
      headers.emplace("X-Trace-ID", delivery_id);
      if (event_id >= 0) {
        headers.emplace("X-Webhook-Event-ID", std::to_string(event_id));
      }
      if (!event_type.empty()) {
        headers.emplace("X-Webhook-Event", event_type);
      }

      // Retained for compatibility with existing receivers. This is not an
      // authentication scheme; a future HMAC feature requires a user secret.
      headers.emplace("X-Timestamp", std::to_string(timestamp));
      headers.emplace("X-Hostname", hostname);
      headers.emplace("X-Signature", generate_signature(timestamp, hostname));
      headers.emplace("X-Client-ID", "Sunshine_Foundation");
      headers.emplace("X-Auth-Token", "Sunshine_Foundation_" + std::to_string(timestamp % 10000));
      headers.emplace("X-API-Version", "v1.0");
      headers.emplace("X-Client-Info", "Foundation Sunshine");
      headers.emplace("X-Service-Name", "Sunshine_Foundation_Service");
      headers.emplace("X-Component", "Sunshine_Foundation_Component");
      return headers;
    }

    void invoke_completion(const std::shared_ptr<delivery_t> &delivery, delivery_result_t result) noexcept {
      if (!delivery->completion) {
        return;
      }

      try {
        delivery->completion(result);
      }
      catch (const std::exception &) {
        BOOST_LOG(warning) << "Webhook completion callback failed"sv;
      }
      catch (...) {
        BOOST_LOG(warning) << "Webhook completion callback failed with an unknown exception"sv;
      }
    }

    class dispatcher_t {
      using work_guard_t = boost::asio::executor_work_guard<SimpleWeb::io_context::executor_type>;

      struct active_attempt_t {
        std::shared_ptr<delivery_t> delivery;
        int attempt = 0;
        std::function<void()> stop;
        std::shared_ptr<boost::asio::steady_timer> deadline;
      };

      struct retry_wait_t {
        std::shared_ptr<delivery_t> delivery;
        std::shared_ptr<boost::asio::steady_timer> timer;
      };

    public:
      ~dispatcher_t() {
        // Final safety net for tests or future callers that start the runtime
        // without retaining the application lifecycle guard.
        stop();
      }

      bool start() noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (accepting_.load(std::memory_order_acquire)) {
          return true;
        }
        if (shutdown_requested_) {
          return false;
        }

        try {
          io_ = std::make_shared<SimpleWeb::io_context>();
          work_guard_ = std::make_unique<work_guard_t>(boost::asio::make_work_guard(*io_));
          stopping_ = false;
          accepting_.store(true, std::memory_order_release);
          thread_ = std::thread([this, io = io_]() noexcept {
            run(io);
          });
          return true;
        }
        catch (const std::exception &) {
          accepting_.store(false, std::memory_order_release);
          work_guard_.reset();
          io_.reset();
          BOOST_LOG(error) << "Webhook runtime failed to start"sv;
        }
        catch (...) {
          accepting_.store(false, std::memory_order_release);
          work_guard_.reset();
          io_.reset();
          BOOST_LOG(error) << "Webhook runtime failed to start with an unknown exception"sv;
        }
        return false;
      }

      void stop() noexcept {
        std::shared_ptr<SimpleWeb::io_context> io;
        {
          std::lock_guard<std::mutex> lock(lifecycle_mutex_);
          shutdown_requested_ = true;
          accepting_.store(false, std::memory_order_release);
          if (!io_) {
            return;
          }

          io = io_;
          try {
            boost::asio::post(*io, [this]() noexcept {
              shutdown_on_io();
            });
          }
          catch (...) {
            io->stop();
          }
        }

        try {
          if (thread_.joinable()) {
            thread_.join();
          }
        }
        catch (const std::exception &) {
          BOOST_LOG(error) << "Webhook runtime join failed"sv;
        }
        catch (...) {
          BOOST_LOG(error) << "Webhook runtime join failed with an unknown exception"sv;
        }

        // If the I/O loop had already stopped, the posted shutdown handler
        // could not run. Joining synchronizes access so cleanup is safe here.
        if (!stopping_) {
          shutdown_on_io();
        }

        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        work_guard_.reset();
        io_.reset();
        pending_count_.store(0, std::memory_order_release);
      }

      bool active() const noexcept {
        return accepting_.load(std::memory_order_acquire);
      }

      delivery_error_t enqueue(std::shared_ptr<delivery_t> delivery) noexcept {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!accepting_.load(std::memory_order_acquire) || !io_) {
          return delivery_error_t::NOT_RUNNING;
        }

        auto pending = pending_count_.load(std::memory_order_relaxed);
        while (pending < MAX_PENDING_DELIVERIES) {
          if (pending_count_.compare_exchange_weak(pending, pending + 1, std::memory_order_acq_rel)) {
            break;
          }
        }
        if (pending >= MAX_PENDING_DELIVERIES) {
          report_queue_full();
          return delivery_error_t::QUEUE_FULL;
        }

        std::string id;
        try {
          id = delivery->id;
          if (!pending_posts_.emplace(id, delivery).second) {
            pending_count_.fetch_sub(1, std::memory_order_acq_rel);
            return delivery_error_t::INTERNAL;
          }
          boost::asio::post(*io_, [this, id]() noexcept {
            std::shared_ptr<delivery_t> delivery;
            try {
              {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                const auto it = pending_posts_.find(id);
                if (it == pending_posts_.end()) {
                  return;
                }
                delivery = std::move(it->second);
                pending_posts_.erase(it);
              }
              accept_on_io(delivery);
            }
            catch (const std::exception &) {
              BOOST_LOG(error) << "Webhook queue handler failed"sv;
              if (delivery) {
                finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
              }
            }
            catch (...) {
              BOOST_LOG(error) << "Webhook queue handler failed with an unknown exception"sv;
              if (delivery) {
                finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
              }
            }
          });
          return delivery_error_t::NONE;
        }
        catch (...) {
          pending_posts_.erase(id);
          pending_count_.fetch_sub(1, std::memory_order_acq_rel);
          return delivery_error_t::INTERNAL;
        }
      }

    private:
      void run(const std::shared_ptr<SimpleWeb::io_context> &io) noexcept {
        for (;;) {
          try {
            io->run();
            return;
          }
          catch (const std::exception &) {
            BOOST_LOG(error) << "Webhook I/O handler failed"sv;
          }
          catch (...) {
            BOOST_LOG(error) << "Webhook I/O handler failed with an unknown exception"sv;
          }
        }
      }

      void accept_on_io(const std::shared_ptr<delivery_t> &delivery) {
        if (stopping_) {
          finalize(delivery, {false, 0, 0, delivery_error_t::CANCELLED});
          return;
        }

        if (delivery->production && is_rate_limited_on_io()) {
          finalize(delivery, {false, 0, 0, delivery_error_t::RATE_LIMITED});
          return;
        }

        ready_.push_back(delivery);
        pump();
      }

      bool is_rate_limited_on_io() {
        const auto now = std::chrono::steady_clock::now();
        const auto window_start = now - RATE_LIMIT_WINDOW;
        while (!accepted_events_.empty() && accepted_events_.front() < window_start) {
          accepted_events_.pop_front();
        }

        if (accepted_events_.size() >= MAX_EVENTS_PER_MINUTE) {
          if (last_rate_limit_log_ < window_start) {
            BOOST_LOG(warning) << "Webhook event rate limit reached; new events will be dropped temporarily"sv;
            last_rate_limit_log_ = now;
          }
          return true;
        }

        accepted_events_.push_back(now);
        return false;
      }

      void pump() noexcept {
        while (!stopping_ && active_.size() < MAX_IN_FLIGHT_DELIVERIES && !ready_.empty()) {
          auto delivery = std::move(ready_.front());
          ready_.pop_front();
          try {
            start_attempt(delivery);
          }
          catch (const std::exception &) {
            BOOST_LOG(warning) << "Webhook delivery "sv << delivery->id << " could not start"sv;
            handle_attempt_result(delivery, true, 0, std::nullopt);
          }
          catch (...) {
            BOOST_LOG(warning) << "Webhook delivery "sv << delivery->id << " could not start"sv;
            handle_attempt_result(delivery, true, 0, std::nullopt);
          }
        }
      }

      void start_attempt(const std::shared_ptr<delivery_t> &delivery) {
        ++delivery->attempt;
        if (delivery->destination.https) {
          if (delivery->settings.skip_ssl_verify && !skip_tls_warning_logged_) {
            BOOST_LOG(warning) << "Webhook TLS certificate verification is disabled by configuration"sv;
            skip_tls_warning_logged_ = true;
          }
          auto client = std::make_shared<WebhookHttpsClient>(
            delivery->destination.server,
            !delivery->settings.skip_ssl_verify
          );
          start_client(delivery, std::move(client));
        }
        else {
          auto client = std::make_shared<WebhookHttpClient>(delivery->destination.server);
          start_client(delivery, std::move(client));
        }
      }

      template <class Client>
      void start_client(const std::shared_ptr<delivery_t> &delivery, std::shared_ptr<Client> client) {
        const long seconds = timeout_seconds(delivery->settings.timeout);
        client->io_service = io_;
        client->config.timeout = seconds;
        client->config.timeout_connect = seconds;
        client->config.max_response_streambuf_size = MAX_RESPONSE_BYTES;

        const int attempt = delivery->attempt;
        const std::string id = delivery->id;
        auto deadline = std::make_shared<boost::asio::steady_timer>(*io_);
        deadline->expires_after(std::chrono::seconds {seconds});
        const bool inserted = active_.emplace(id, active_attempt_t {
          delivery,
          attempt,
          [client]() noexcept {
            client->stop();
          },
          deadline
        }).second;
        if (!inserted) {
          throw std::runtime_error("duplicate Webhook delivery ID");
        }

        using response_t = typename Client::Response;
        std::function<void(std::shared_ptr<response_t>, const SimpleWeb::error_code &)> callback =
          [this, id, attempt, client](std::shared_ptr<response_t> response, const SimpleWeb::error_code &) noexcept {
            try {
              const int status = response ? parse_status_code(response->status_code) : 0;
              const auto retry_after = response ? parse_retry_after(response->header) : std::optional<std::chrono::seconds> {};
              // Only status and headers are part of the Webhook contract. Stop
              // after the first callback so oversized or streaming bodies
              // cannot continue consuming resources after this attempt ends.
              client->stop();
              // Once a valid HTTP status has arrived, the receiver has
              // accepted and processed the request far enough for HTTP policy
              // to decide the result. A later body/read error must not turn a
              // 2xx into a retry and create avoidable duplicate deliveries.
              complete_attempt_on_io(id, attempt, status == 0, status, retry_after);
            }
            catch (const std::exception &) {
              BOOST_LOG(error) << "Webhook response handler failed"sv;
              complete_attempt_on_io(id, attempt, true, 0, std::nullopt);
            }
            catch (...) {
              BOOST_LOG(error) << "Webhook response handler failed with an unknown exception"sv;
              complete_attempt_on_io(id, attempt, true, 0, std::nullopt);
            }
        };

        try {
          deadline->async_wait([this, id, attempt](const boost::system::error_code &ec) noexcept {
            if (ec) {
              return;
            }
            const auto active = active_.find(id);
            if (active == active_.end() || active->second.attempt != attempt) {
              return;
            }
            active->second.stop();
            complete_attempt_on_io(id, attempt, true, 0, std::nullopt);
          });
          client->request(
            "POST",
            delivery->destination.target,
            delivery->payload,
            generate_webhook_headers(delivery->id, delivery->event_id, delivery->event_type),
            std::move(callback)
          );
        }
        catch (...) {
          try {
            deadline->cancel();
          }
          catch (...) {
          }
          active_.erase(id);
          throw;
        }
      }

      void complete_attempt_on_io(const std::string &id, int attempt, bool transport_error, int status, std::optional<std::chrono::seconds> retry_after) noexcept {
        const auto it = active_.find(id);
        if (it == active_.end() || it->second.attempt != attempt) {
          return;
        }

        try {
          it->second.deadline->cancel();
        }
        catch (...) {
        }
        auto delivery = std::move(it->second.delivery);
        active_.erase(it);
        handle_attempt_result(delivery, transport_error, status, retry_after);
      }

      void handle_attempt_result(const std::shared_ptr<delivery_t> &delivery, bool transport_error, int status, std::optional<std::chrono::seconds> retry_after) noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - delivery->accepted_at
        );

        if (!transport_error && is_success_status(status)) {
          BOOST_LOG(info) << "Webhook delivery "sv << delivery->id << " succeeded with HTTP "sv << status
                          << " after "sv << delivery->attempt << " attempt(s), "sv << elapsed.count() << " ms"sv;
          finalize(delivery, {true, status, delivery->attempt, delivery_error_t::NONE});
          pump();
          return;
        }

        const bool retryable = transport_error || is_retryable_status(status);
        if (retryable && delivery->attempt < delivery->max_attempts && !stopping_) {
          if (status != 429 && status != 503) {
            retry_after.reset();
          }
          schedule_retry(delivery, retry_after);
          pump();
          return;
        }

        const auto error = transport_error ? delivery_error_t::TRANSPORT : delivery_error_t::HTTP_STATUS;
        if (status > 0) {
          BOOST_LOG(warning) << "Webhook delivery "sv << delivery->id << " failed with HTTP "sv << status
                             << " after "sv << delivery->attempt << " attempt(s), "sv << elapsed.count() << " ms"sv;
        }
        else {
          BOOST_LOG(warning) << "Webhook delivery "sv << delivery->id << " failed with a transport error after "sv
                             << delivery->attempt << " attempt(s), "sv << elapsed.count() << " ms"sv;
        }
        finalize(delivery, {false, status, delivery->attempt, error});
        pump();
      }

      void schedule_retry(const std::shared_ptr<delivery_t> &delivery, std::optional<std::chrono::seconds> retry_after) noexcept {
        try {
          std::chrono::milliseconds delay;
          if (retry_after) {
            delay = std::chrono::duration_cast<std::chrono::milliseconds>(*retry_after);
          }
          else {
            const auto backoff = delivery->attempt == 1 ? 1000ms : 2000ms;
            delay = backoff + std::chrono::milliseconds(next_jitter());
          }

          auto timer = std::make_shared<boost::asio::steady_timer>(*io_);
          timer->expires_after(delay);
          retry_waits_[delivery->id] = retry_wait_t {delivery, timer};
          const std::string id = delivery->id;
          timer->async_wait([this, id](const boost::system::error_code &ec) noexcept {
            std::shared_ptr<delivery_t> delivery;
            try {
              const auto it = retry_waits_.find(id);
              if (it == retry_waits_.end()) {
                return;
              }

              delivery = std::move(it->second.delivery);
              retry_waits_.erase(it);
              if (ec || stopping_) {
                finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::CANCELLED});
                return;
              }

              ready_.push_back(delivery);
              pump();
            }
            catch (const std::exception &) {
              BOOST_LOG(error) << "Webhook retry handler failed"sv;
              if (delivery) {
                finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
              }
            }
            catch (...) {
              BOOST_LOG(error) << "Webhook retry handler failed with an unknown exception"sv;
              if (delivery) {
                finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
              }
            }
          });
        }
        catch (const std::exception &) {
          retry_waits_.erase(delivery->id);
          BOOST_LOG(warning) << "Webhook retry scheduling failed"sv;
          finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
        }
        catch (...) {
          retry_waits_.erase(delivery->id);
          BOOST_LOG(warning) << "Webhook retry scheduling failed with an unknown exception"sv;
          finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::INTERNAL});
        }
      }

      unsigned next_jitter() noexcept {
        jitter_state_ ^= jitter_state_ << 13;
        jitter_state_ ^= jitter_state_ >> 17;
        jitter_state_ ^= jitter_state_ << 5;
        return jitter_state_ % 251;
      }

      void finalize(const std::shared_ptr<delivery_t> &delivery, delivery_result_t result) noexcept {
        if (delivery->finalized) {
          return;
        }
        delivery->finalized = true;
        pending_count_.fetch_sub(1, std::memory_order_acq_rel);
        invoke_completion(delivery, result);
      }

      void shutdown_on_io() noexcept {
        if (stopping_) {
          return;
        }
        stopping_ = true;

        std::map<std::string, std::shared_ptr<delivery_t>> pending_posts;
        {
          std::lock_guard<std::mutex> lock(lifecycle_mutex_);
          pending_posts.swap(pending_posts_);
        }
        for (auto &entry : pending_posts) {
          auto &delivery = entry.second;
          finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::CANCELLED});
        }

        while (!ready_.empty()) {
          auto delivery = std::move(ready_.front());
          ready_.pop_front();
          finalize(delivery, {false, 0, delivery->attempt, delivery_error_t::CANCELLED});
        }

        for (auto &entry : retry_waits_) {
          auto &retry = entry.second;
          try {
            // Recent Boost.Asio versions expose only the throwing cancel()
            // overload. Shutdown must remain noexcept even if cancellation
            // reports an internal timer service error.
            retry.timer->cancel();
          }
          catch (...) {
            // Continue finalizing all pending deliveries during shutdown.
          }
          finalize(retry.delivery, {false, 0, retry.delivery->attempt, delivery_error_t::CANCELLED});
        }
        retry_waits_.clear();

        for (auto &entry : active_) {
          auto &active = entry.second;
          try {
            active.deadline->cancel();
          }
          catch (...) {
          }
          active.stop();
          finalize(active.delivery, {false, 0, active.delivery->attempt, delivery_error_t::CANCELLED});
        }
        active_.clear();

        accepted_events_.clear();
        work_guard_.reset();
        if (io_) {
          io_->stop();
        }
      }

      void report_queue_full() noexcept {
        const auto now = std::chrono::steady_clock::now();
        auto previous = last_queue_full_log_.load(std::memory_order_relaxed);
        const auto current = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        if (current - previous >= 60 && last_queue_full_log_.compare_exchange_strong(previous, current)) {
          BOOST_LOG(warning) << "Webhook queue is full; new deliveries will be dropped temporarily"sv;
        }
      }

      std::mutex lifecycle_mutex_;
      std::shared_ptr<SimpleWeb::io_context> io_;
      std::unique_ptr<work_guard_t> work_guard_;
      std::thread thread_;
      std::atomic<bool> accepting_ {false};
      bool shutdown_requested_ = false;
      std::atomic<std::size_t> pending_count_ {0};
      std::atomic<long long> last_queue_full_log_ {0};
      std::map<std::string, std::shared_ptr<delivery_t>> pending_posts_;

      // The following state is owned exclusively by the I/O thread.
      bool stopping_ = false;
      std::deque<std::shared_ptr<delivery_t>> ready_;
      std::map<std::string, active_attempt_t> active_;
      std::map<std::string, retry_wait_t> retry_waits_;
      std::deque<std::chrono::steady_clock::time_point> accepted_events_;
      std::chrono::steady_clock::time_point last_rate_limit_log_ {};
      std::uint32_t jitter_state_ = 0x8f31a2d7U;
      bool skip_tls_warning_logged_ = false;
    };

    dispatcher_t &dispatcher() {
      static dispatcher_t instance;
      return instance;
    }

    std::shared_ptr<delivery_t> make_delivery(
      const settings_t &settings,
      std::string payload,
      int event_id,
      std::string event_type,
      bool production,
      int max_attempts,
      completion_handler_t completion
    ) {
      parsed_url_t destination;
      if (!parse_webhook_url(settings.url, destination)) {
        return nullptr;
      }

      auto delivery = std::make_shared<delivery_t>();
      delivery->settings = settings;
      delivery->destination = std::move(destination);
      delivery->payload = std::move(payload);
      delivery->id = uuid_util::uuid_t::generate().string();
      delivery->event_id = event_id;
      delivery->event_type = std::move(event_type);
      delivery->production = production;
      delivery->max_attempts = std::max(1, max_attempts);
      delivery->completion = std::move(completion);
      delivery->accepted_at = std::chrono::steady_clock::now();
      return delivery;
    }

    void complete_rejected(completion_handler_t &completion, delivery_error_t error) noexcept {
      if (!completion) {
        return;
      }
      try {
        completion({false, 0, 0, error});
      }
      catch (...) {
      }
    }
  }  // namespace

  deinit_t::~deinit_t() {
    dispatcher().stop();
  }

  std::unique_ptr<deinit_t> init() noexcept {
    try {
      auto guard = std::unique_ptr<deinit_t>(new deinit_t());
      init_webhook_format();
      (void) dispatcher().start();
      return guard;
    }
    catch (const std::exception &) {
      BOOST_LOG(error) << "Webhook initialization failed"sv;
    }
    catch (...) {
      BOOST_LOG(error) << "Webhook initialization failed with an unknown exception"sv;
    }
    return nullptr;
  }

  bool ensure_running() noexcept {
    try {
      return dispatcher().start();
    }
    catch (...) {
      return false;
    }
  }

  bool runtime_active() noexcept {
    return dispatcher().active();
  }

  bool validate_configuration(const configuration_t &configuration) noexcept {
    try {
      if (configuration.url.size() > MAX_URL_SIZE ||
          configuration.timeout < MIN_TIMEOUT ||
          configuration.timeout > MAX_TIMEOUT ||
          (configuration.enabled && configuration.url.empty())) {
        return false;
      }

      parsed_url_t destination;
      if (!configuration.url.empty() &&
          !parse_webhook_url(configuration.url, destination)) {
        return false;
      }

      std::bitset<EVENT_TYPE_COUNT> seen;
      for (const auto event_id : configuration.events) {
        if (event_id < 0 ||
            event_id >= EVENT_TYPE_COUNT ||
            seen.test(static_cast<std::size_t>(event_id))) {
          return false;
        }
        seen.set(static_cast<std::size_t>(event_id));
      }
      return true;
    }
    catch (...) {
      return false;
    }
  }

  prepared_configuration_t
  prepare_configuration(configuration_t configuration) noexcept {
    if (!validate_configuration(configuration)) {
      return {};
    }

    try {
      std::sort(configuration.events.begin(), configuration.events.end());
      return prepared_configuration_t {
        std::make_shared<const configuration_t>(std::move(configuration))
      };
    }
    catch (...) {
      return {};
    }
  }

  bool
  commit_configuration(prepared_configuration_t &&configuration) noexcept {
    if (!configuration) {
      return false;
    }

    active_configuration.store(
      std::move(configuration.configuration_),
      std::memory_order_release
    );
    return true;
  }

  bool configure(configuration_t configuration) noexcept {
    auto prepared = prepare_configuration(std::move(configuration));
    return commit_configuration(std::move(prepared));
  }

  configuration_t current_configuration() {
    const auto configuration = active_configuration.load(std::memory_order_acquire);
    return *configuration;
  }

  void send_event_async(const event_t &event) noexcept {
    try {
      const auto configuration = current_configuration();
      const auto event_id = event_type_id(event.type);
      if (!configuration.enabled ||
          configuration.url.empty() ||
          std::find(configuration.events.begin(), configuration.events.end(), event_id) == configuration.events.end()) {
        return;
      }

      const settings_t settings {
        configuration.url,
        configuration.skip_ssl_verify,
        configuration.timeout
      };
      auto delivery = make_delivery(
        settings,
        generate_webhook_json(event, use_chinese_content()),
        event_id,
        event_type_name(event.type),
        true,
        MAX_PRODUCTION_ATTEMPTS,
        {}
      );
      if (!delivery) {
        BOOST_LOG(warning) << "Webhook event was rejected because the configured URL is invalid"sv;
        return;
      }

      // Runtime initialization already reports one stable warning. Silently
      // drop later production events when it is unavailable or shutting down
      // so an optional module cannot create an event-driven log storm.
      (void) dispatcher().enqueue(std::move(delivery));
    }
    catch (const std::exception &) {
      BOOST_LOG(error) << "Webhook event preparation failed"sv;
    }
    catch (...) {
      BOOST_LOG(error) << "Webhook event preparation failed with an unknown exception"sv;
    }
  }

  bool send_test_async(
    const settings_t &settings,
    int retry_count,
    completion_handler_t completion
  ) noexcept {
    try {
      if (retry_count < 0 ||
          retry_count > MAX_TEST_RETRIES ||
          settings.timeout < MIN_TIMEOUT ||
          settings.timeout > MAX_TIMEOUT) {
        complete_rejected(completion, delivery_error_t::INTERNAL);
        return false;
      }
      if (!ensure_running()) {
        complete_rejected(completion, delivery_error_t::NOT_RUNNING);
        return false;
      }
      auto delivery = make_delivery(
        settings,
        g_webhook_format.generate_test_json_payload(use_chinese_content()),
        -1,
        "webhook_test",
        false,
        retry_count + 1,
        completion
      );
      if (!delivery) {
        complete_rejected(completion, delivery_error_t::INVALID_URL);
        return false;
      }

      const auto error = dispatcher().enqueue(std::move(delivery));
      if (error != delivery_error_t::NONE) {
        complete_rejected(completion, error);
        return false;
      }
      return true;
    }
    catch (...) {
      complete_rejected(completion, delivery_error_t::INTERNAL);
      return false;
    }
  }

  bool is_enabled() {
    try {
      const auto configuration = current_configuration();
      return configuration.enabled && !configuration.url.empty();
    }
    catch (...) {
      return false;
    }
  }

  bool is_event_enabled(event_type_t type) noexcept {
    try {
      const auto configuration = current_configuration();
      const auto id = event_type_id(type);
      return std::find(configuration.events.begin(), configuration.events.end(), id) != configuration.events.end();
    }
    catch (...) {
      return false;
    }
  }

  int event_type_id(event_type_t type) noexcept {
    return static_cast<int>(type);
  }

  const char *event_type_name(event_type_t type) noexcept {
    switch (type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
        return "config_pair_success";
      case event_type_t::CONFIG_PIN_FAILED:
        return "config_pair_failed";
      case event_type_t::NV_APP_LAUNCH:
        return "nv_app_launch";
      case event_type_t::NV_APP_RESUME:
        return "nv_app_resume";
      case event_type_t::NV_APP_TERMINATE:
        return "nv_app_terminate";
      case event_type_t::NV_SESSION_START:
        return "nv_session_start";
      case event_type_t::NV_SESSION_END:
        return "nv_session_end";
      default:
        return "unknown";
    }
  }

  std::string get_alert_message(event_type_t type, bool is_chinese) {
    switch (type) {
      case event_type_t::CONFIG_PIN_SUCCESS:
        return is_chinese ? "🔗 配置配对成功" : "🔗 Config pairing successful";
      case event_type_t::CONFIG_PIN_FAILED:
        return is_chinese ? "❌ 配置配对失败" : "❌ Config pairing failed";
      case event_type_t::NV_APP_LAUNCH:
        return is_chinese ? "🚀 应用启动" : "🚀 application launched";
      case event_type_t::NV_APP_RESUME:
        return is_chinese ? "▶️ 应用恢复" : "▶️ application resumed";
      case event_type_t::NV_APP_TERMINATE:
        return is_chinese ? "⏹️ 应用终止" : "⏹️ application terminated";
      case event_type_t::NV_SESSION_START:
        return is_chinese ? "📱 会话开始" : "📱 session started";
      case event_type_t::NV_SESSION_END:
        return is_chinese ? "📱 会话结束" : "📱 session ended";
      default:
        return is_chinese ? "🔔 系统通知" : "🔔 System notification";
    }
  }

  std::string sanitize_json_string(const std::string &str) {
    std::string result;
    result.reserve(str.size());
    for (const unsigned char c : str) {
      switch (c) {
        case '\\':
          result += "\\\\";
          break;
        case '"':
          result += "\\\"";
          break;
        case '\n':
          result += "\\n";
          break;
        case '\r':
          result += "\\r";
          break;
        case '\t':
          result += "\\t";
          break;
        default:
          if (c >= 0x20) {
            result.push_back(static_cast<char>(c));
          }
          break;
      }
    }
    return result;
  }

  std::string get_current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
    return stream.str();
  }

  std::string generate_webhook_json(const event_t &event, bool is_chinese) {
    return g_webhook_format.generate_json_payload(event, is_chinese);
  }

  const char *delivery_error_name(delivery_error_t error) noexcept {
    switch (error) {
      case delivery_error_t::NONE:
        return "none";
      case delivery_error_t::NOT_RUNNING:
        return "not_running";
      case delivery_error_t::QUEUE_FULL:
        return "queue_full";
      case delivery_error_t::RATE_LIMITED:
        return "rate_limited";
      case delivery_error_t::INVALID_URL:
        return "invalid_url";
      case delivery_error_t::TRANSPORT:
        return "transport";
      case delivery_error_t::HTTP_STATUS:
        return "http_status";
      case delivery_error_t::CANCELLED:
        return "cancelled";
      case delivery_error_t::INTERNAL:
      default:
        return "internal";
    }
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    bool parse_url(const std::string &url, parsed_url_t &parsed) {
      webhook::parsed_url_t internal;
      if (!parse_webhook_url(url, internal)) {
        return false;
      }
      parsed.https = internal.https;
      parsed.server = std::move(internal.server);
      parsed.target = std::move(internal.target);
      return true;
    }

    std::string sanitize_header_value(const std::string &value) {
      return normalize_header_value(value);
    }

    long timeout_seconds(std::chrono::milliseconds timeout) noexcept {
      return webhook::timeout_seconds(timeout);
    }

    bool is_success_status(int status) noexcept {
      return webhook::is_success_status(status);
    }

    bool is_retryable_status(int status) noexcept {
      return webhook::is_retryable_status(status);
    }

    std::optional<long> retry_after_seconds(const std::string &value) {
      const SimpleWeb::CaseInsensitiveMultimap headers {{"Retry-After", value}};
      const auto parsed = parse_retry_after(headers);
      return parsed ? std::optional<long>(parsed->count()) : std::nullopt;
    }
  }  // namespace test_support
#endif

}  // namespace webhook
