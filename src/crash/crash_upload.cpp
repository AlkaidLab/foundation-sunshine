/**
 * @file src/crash/crash_upload.cpp
 * @brief Uploads the pending crash reports to POST <endpoint>/v1/crash (multipart: "meta" JSON + optional "dump" + the anonymised session log of the run).
 *
 * Runs on its own thread and only ever waits on the network, never on anything the host needs to start. A report is deleted after a 2xx and
 * after an answer that says the body will never be accepted (upload_policy.h); everything else, a network failure, a 5xx, a limit, a proxy's refusal, leaves it for a later pass, spread by a random share.
 * At most 20 stay on disk (and a byte cap), none older than 30 days.
 */
#include "crash_log.h"
#include "crash_state.h"
#include "upload_policy.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <functional>
#include <optional>
#include <random>
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace crash {
  namespace fs = std::filesystem;
  using namespace detail;

  namespace {
    std::thread g_uploader;
    std::atomic<bool> g_stop_upload { false };
    std::mutex g_wait_mutex;
    std::condition_variable g_wait_cv;
    std::once_flag g_curl_once;

    std::string
    read_all(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    bool
    strncasecmp_ascii(const std::string &line, const char *prefix) {
      for (std::size_t i = 0; prefix[i]; ++i)
        if (i >= line.size() || std::tolower((unsigned char) line[i]) != prefix[i]) return false;
      return true;
    }

    size_t
    collect_body(char *data, size_t size, size_t count, void *user) {
      auto *body = static_cast<std::string *>(user);
      if (body->size() < 16384) body->append(data, std::min(size * count, 16384 - body->size()));
      return size * count;
    }

    /// Retry-After of the last answer in seconds (0 = none). The service sends it with a 429 (a limit) and with a 503.
    size_t
    header_line(char *data, size_t size, size_t count, void *user) {
      const std::string line(data, size * count);
      if (line.size() > 12 && (line[0] == 'R' || line[0] == 'r') && strncasecmp_ascii(line, "retry-after:")) *static_cast<long *>(user) = std::atol(line.c_str() + 12);
      return size * count;
    }

    int
    stop_requested(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      return g_stop_upload.load() ? 1 : 0;
    }

    struct answer_t {
      long status = 0;  ///< 0: network failure; otherwise the HTTP status
      std::string body;
      long retry_after = 0;  ///< seconds, from the Retry-After header of a 429 or 503
    };

    void
    add_log_parts(curl_mime *mime, const logfile::packed_t &log) {
      auto *part = curl_mime_addpart(mime);
      curl_mime_name(part, "log");
      curl_mime_data(part, log.gz.data(), log.gz.size());
      curl_mime_filename(part, "sunshine.log.gz");
      curl_mime_type(part, "application/gzip");
      // the first and the last 64 KiB of the same text, plain: the service checks them for personal data and against the start of the gzip file
      const std::pair<const char *, const std::string *> sidecars[] = { { "log_head", &log.head }, { "log_tail", &log.tail } };
      for (const auto &[name, text] : sidecars) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, name);
        curl_mime_data(part, text->data(), text->size());
        curl_mime_type(part, "text/plain; charset=utf-8");
      }
      const auto lines = std::to_string(log.lines);
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "log_lines");
      curl_mime_data(part, lines.data(), lines.size());
    }

    /// One multipart POST. `dump`, `log` and `image` (the screenshot of a feedback report) are optional.
    answer_t
    post(const std::string &url, const std::string &meta, const std::string &dump, const logfile::packed_t *log = nullptr, const std::string *image = nullptr) {
      answer_t answer;
      CURL *curl = curl_easy_init();
      if (!curl) return answer;
      curl_mime *mime = curl_mime_init(curl);
      auto *part = curl_mime_addpart(mime);
      curl_mime_name(part, "meta");
      curl_mime_data(part, meta.data(), meta.size());
      curl_mime_type(part, "application/json");
      if (!dump.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "dump");
        curl_mime_data(part, dump.data(), dump.size());
        curl_mime_filename(part, "crash.dmp");
        curl_mime_type(part, "application/octet-stream");
      }
      if (log) add_log_parts(mime, *log);
      if (image) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "image");
        curl_mime_data(part, image->data(), image->size());
        curl_mime_filename(part, "screenshot");
        curl_mime_type(part, "application/octet-stream");  // the server judges by the first bytes
      }
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer.body);
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_line);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, &answer.retry_after);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "FoundationSunshine-crash/1");
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stop_requested);
      const CURLcode rc = curl_easy_perform(curl);
      if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &answer.status);
      curl_mime_free(mime);
      curl_easy_cleanup(curl);
      return answer;
    }

    /// The configured log path (sunshine.conf log_path), else sunshine.log in the config directory.
    fs::path
    log_path_of(const fs::path &config_dir) {
      const auto configured = conf_value(current_conf_text(), "log_path");
      if (configured.empty()) return config_dir / "sunshine.log";
      // the host's own rule (file_handler::path_from_utf8 and path_f): UTF-8 text, and a relative path is relative to the config directory
      const std::u8string utf8(reinterpret_cast<const char8_t *>(configured.data()), configured.size());
      const fs::path path(utf8);
      return path.is_absolute() ? path : config_dir / path;
    }

    std::mutex g_env_mutex;
    std::function<std::string()> g_environment;
    /// After a 429/503 or a network failure the uploader waits longer (Retry-After, else 15 minutes doubling up to 4 hours: the numbers Crashpad uses for its retries), each spread by a random share.
    std::atomic<long> g_wait_hint_s { 0 };
    std::atomic<int> g_failures { 0 };
    std::mutex g_jitter_mutex;
    std::function<double()> g_jitter;

    /// A number in [0, 1): the random share of a wait or of the start delay.
    double
    jitter01() {
      std::lock_guard lock(g_jitter_mutex);
      if (g_jitter) return g_jitter();
      static std::mt19937_64 engine { std::random_device {}() };
      return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
    }

    /// What names this account and this computer, for the privacy pass over a dump (redact_private_names).
    std::vector<std::string>
    private_names() {
      std::vector<std::string> names;
      auto add = [&](const char *value) {
        if (!value || !*value) return;
        std::string v = value;
        std::string lower = v;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (lower == "system" || lower == "root") return;  // not a person, and words the runtime's own names contain
        names.push_back(std::move(v));
      };
      for (const char *key : { "USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME" }) add(std::getenv(key));
  #ifdef _WIN32
      char buffer[256];
      DWORD size = sizeof(buffer);
      if (GetComputerNameA(buffer, &size)) add(buffer);
  #else
      char host[256] = {};
      if (gethostname(host, sizeof(host) - 1) == 0) add(host);
  #endif
      return names;
    }

    void
    uploader_main() {
      using namespace std::chrono_literals;
      auto wait = [](std::chrono::seconds s) {
        std::unique_lock lock(g_wait_mutex);
        return !g_wait_cv.wait_for(lock, s, [] { return g_stop_upload.load(); });
      };
      // 20 s after the start and up to two minutes more, so the computers that were started together (a power cut, a morning) do not all knock at once
      std::chrono::seconds delay = 20s + std::chrono::seconds(static_cast<long>(jitter01() * 120.0));
      if (const char *env = std::getenv("SUNSHINE_CRASH_UPLOAD_DELAY_S"); env && *env) delay = std::chrono::seconds(std::max(0, std::atoi(env)));
      if (!wait(delay)) return;
      do {
        if (enabled()) {
          const auto dir = current_config_dir();
          upload_pending_once(dir, endpoint_from(current_conf_text()), read_or_create_install_id(dir, true));
        }
        // a pass that found nothing to wait for comes back in ten minutes (a report written by a hang is not left for half an hour); after a failure the wait the policy chose
      } while (wait(std::chrono::seconds([] {
        // after a failure the wait the policy chose (the service's Retry-After or the backoff, each spread by the random share); with nothing to wait for, ten minutes
        const auto hint = g_wait_hint_s.load();
        return hint > 0 ? hint : 10L * 60;
      }())));
    }
  }  // namespace

  namespace {
    /// "-p<pid hex>." as the stems the handlers write carry it
    std::string
    own_pid_tag() {
      char tag[24];
  #ifdef _WIN32
      const unsigned long pid = GetCurrentProcessId();
  #else
      const unsigned long pid = (unsigned long) getpid();
  #endif
      std::snprintf(tag, sizeof(tag), "-p%08lx.", pid);
      return tag;
    }
  }  // namespace

  int
  upload_pending_once(const fs::path &config_dir, const std::string &endpoint, const std::string &install_id) {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    const auto dir = pending_dir(config_dir);
    trim_pending(dir);
    int done = 0;
    const auto now = fs::file_time_type::clock::now();
    std::string base = endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const auto url = base + "/v1/crash";
    for (const auto &report : list_pending(dir)) {
      if (g_stop_upload.load()) break;
      // one this process is still writing (a hang report: the dump follows the meta only when the meta is last, which it is not, but be safe)
      if (now - report.time < std::chrono::seconds(10) && report.meta.filename().string().find(own_pid_tag()) != std::string::npos) continue;
      auto meta = nlohmann::json::parse(read_all(report.meta), nullptr, false);
      std::string body;
      if (meta.is_discarded() || !meta.is_object()) {
        std::error_code ec;
        fs::remove(report.meta, ec);
        if (!report.dump.empty()) fs::remove(report.dump, ec);
        continue;
      }
      if (!meta.contains("install_id") && !install_id.empty()) meta["install_id"] = install_id;
      if (!meta.contains("v")) meta["v"] = 1;
      // the machine (CPU, GPU, memory, displays, language, how it was installed) is read now, on the computer that crashed: the service keeps fixed words and numbers only
      if (!meta.contains("env")) {
        std::function<std::string()> provider;
        {
          std::lock_guard lock(g_env_mutex);
          provider = g_environment;
        }
        if (provider) {
          try {
            auto env = nlohmann::json::parse(provider(), nullptr, false);
            if (env.is_object() && !env.empty()) meta["env"] = std::move(env);
          }
          catch (...) {
          }
        }
      }
      body = meta.dump();
      std::string dump;
      if (!report.dump.empty()) {
        std::error_code ec;
        if (fs::file_size(report.dump, ec) <= max_dump_bytes) {
          dump = read_all(report.dump);
          // the dump type keeps heap and data out; this blanks the account and computer names that a path or a stack word still carries
          redact_private_names(dump, private_names());
        }
      }
      // the anonymised log of the run that wrote the report (host reports only): found by time, anonymised, compressed. A log that cannot be made never holds a report back.
      std::optional<logfile::packed_t> log;
      if (meta.value("product", std::string("host")) == "host") {
        try {
          const auto text = logfile::collect(log_path_of(config_dir), report.time);
          if (!text.empty()) log = logfile::pack(text);
        }
        catch (...) {
        }
      }
      auto answer = post(url, body, dump, log ? &*log : nullptr);
      auto verdict = upload_policy::judge(answer.status, !dump.empty());
      // 413: the dump is too large for the service, the report is not: it goes again without the dump (the log stays with it)
      if (verdict == upload_policy::verdict_e::send_without_dump) {
        answer = post(url, body, {}, log ? &*log : nullptr);
        verdict = upload_policy::judge(answer.status, false);
      }
      // a limit (429, wait Retry-After), a service that could not store it (503), no answer, any other answer that is not about this report: keep everything and stop this pass
      if (verdict == upload_policy::verdict_e::wait) {
        const long failures = g_failures.fetch_add(1) + 1;
        g_wait_hint_s.store(upload_policy::wait_seconds(failures, answer.retry_after, jitter01()));
        break;
      }
      g_failures.store(0);
      g_wait_hint_s.store(0);
      if (verdict == upload_policy::verdict_e::keep_going) continue;
      std::error_code ec;
      fs::remove(report.meta, ec);
      if (!report.dump.empty()) fs::remove(report.dump, ec);
      ++done;
    }
    return done;
  }

  void
  set_upload_jitter_source(std::function<double()> source) {
    std::lock_guard lock(g_jitter_mutex);
    g_jitter = std::move(source);
  }

  // lives here, not in crash_common.cpp: the service wrapper links the capture without the uploader
  void
  run_test_switch() {
    const char *what = std::getenv("SUNSHINE_TEST_CRASH");
    if (!what || !*what) return;
    if (std::string_view(what) == "upload-only") {
      // one synchronous upload pass, then the process ends: what a test needs from a real start, without the servers
      if (enabled()) {
        const auto dir = current_config_dir();
        upload_pending_once(dir, endpoint_from(current_conf_text()), read_or_create_install_id(dir, true));
      }
      std::_Exit(0);
    }
    platform_test_crash(what);
  }

  void
  start_uploader() {
    if (g_uploader.joinable()) return;
    g_stop_upload.store(false);
    g_uploader = std::thread(uploader_main);
  }

  void
  stop() {
    g_stop_upload.store(true);
    g_wait_cv.notify_all();
    if (g_uploader.joinable()) g_uploader.join();
    stop_watchdog();
  }

  long
  upload_wait_hint_s() {
    return g_wait_hint_s.load();
  }

  void
  set_environment_provider(std::function<std::string()> provider) {
    std::lock_guard lock(g_env_mutex);
    g_environment = std::move(provider);
  }

  namespace {
    std::atomic<const char *> g_session_log_state { "no_log" };
  }

  const char *
  session_log_state() {
    return g_session_log_state.load();
  }

  std::optional<std::string>
  upload_session_log(const std::string &endpoint, const std::string &install_id, const std::string &version) {
    // no check of the error-report switch: the run log is part of a feedback report, and pressing 发送 is the consent for it
    return upload_session_log_from(current_config_dir(), endpoint, install_id, version);
  }

  namespace {
    std::atomic<const char *> g_session_shot_state { "none" };
  }

  const char *
  session_shot_state() {
    return g_session_shot_state.load();
  }

  std::optional<std::string>
  upload_session_shot(const std::string &endpoint, const std::string &install_id, const std::string &version, const std::string &image) {
    g_session_shot_state.store("none");
    if (image.empty()) return std::nullopt;
    try {
      std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
      std::string base = endpoint;
      while (!base.empty() && base.back() == '/') base.pop_back();
      const nlohmann::json meta { { "v", 1 }, { "install_id", install_id }, { "version", version } };
      const auto answer = post(base + "/v1/user-screenshot", meta.dump(), {}, nullptr, &image);
      if (answer.status == 0) {
        g_session_shot_state.store("unreachable");
        return std::nullopt;
      }
      if (answer.status < 200 || answer.status >= 300) {
        g_session_shot_state.store("refused");  // a service without the endpoint (404), a limit (429): the report goes without the picture
        return std::nullopt;
      }
      g_session_shot_state.store("bad_answer");
      const auto doc = nlohmann::json::parse(answer.body, nullptr, false);
      if (!doc.is_object()) return std::nullopt;
      const auto state = doc.value("shot", std::string());
      if (state == "stored" && doc.contains("ref") && doc["ref"].is_string()) {
        g_session_shot_state.store("uploaded");
        return doc["ref"].get<std::string>();
      }
      // the server's own words for why it left the picture out: only the ones the contract has
      for (const char *known : { "too_large", "rejected", "unavailable", "none" })
        if (state == known) g_session_shot_state.store(known);
      if (state == "skipped_daily" || state == "skipped_quota") g_session_shot_state.store("unavailable");
    }
    catch (...) {
    }
    return std::nullopt;
  }

  std::optional<std::string>
  upload_session_log_from(const fs::path &config_dir, const std::string &endpoint, const std::string &install_id, const std::string &version) {
    g_session_log_state.store("no_log");
    try {
      std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
      const auto text = logfile::collect_current(log_path_of(config_dir));
      if (text.empty()) return std::nullopt;
      g_session_log_state.store("pack_failed");
      const auto packed = logfile::pack(text);
      if (!packed) return std::nullopt;
      std::string base = endpoint;
      while (!base.empty() && base.back() == '/') base.pop_back();
      const nlohmann::json meta { { "v", 1 }, { "install_id", install_id }, { "version", version } };
      // a service without the endpoint (404), a limit (429), a refusal: the report goes without the log
      const auto answer = post(base + "/v1/user-log", meta.dump(), {}, &*packed);
      if (answer.status == 0) {
        g_session_log_state.store("unreachable");
        return std::nullopt;
      }
      if (answer.status < 200 || answer.status >= 300) {
        g_session_log_state.store("refused");
        return std::nullopt;
      }
      g_session_log_state.store("bad_answer");
      const auto doc = nlohmann::json::parse(answer.body, nullptr, false);
      if (doc.is_object() && doc.value("log", std::string()) == "stored" && doc.contains("ref") && doc["ref"].is_string()) {
        g_session_log_state.store("uploaded");
        return doc["ref"].get<std::string>();
      }
    }
    catch (...) {
    }
    return std::nullopt;
  }
}  // namespace crash
