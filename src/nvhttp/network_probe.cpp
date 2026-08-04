#include "network_probe.h"

#include <algorithm>
#include <array>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include "src/logging.h"
#include "src/stream.h"

using json = nlohmann::json;

namespace nvhttp::network_probe {
  namespace {
    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      return {
        { "Content-Type", "application/json" },
        { "Cache-Control", "no-store" },
      };
    }

    void
    write_error(resp_https_t response, SimpleWeb::StatusCode status, std::string_view error, std::uint32_t retry_after_ms = 0) {
      json body { { "error", error } };
      auto headers = json_headers();
      if (retry_after_ms > 0) {
        body["retryAfterMs"] = retry_after_ms;
        headers.emplace("Retry-After", std::to_string((retry_after_ms + 999) / 1000));
      }
      response->write(status, body.dump(), headers);
    }

    std::string
    client_log_id(std::string_view client) {
      return std::string(client.substr(0, std::min<std::size_t>(client.size(), 12)));
    }
  }  // namespace

  struct service_t::impl_t: std::enable_shared_from_this<impl_t> {
    limiter_t limiter;
    std::array<char, CHUNK_BYTES> payload {};

    impl_t() {
      if (RAND_bytes(reinterpret_cast<unsigned char *>(payload.data()), static_cast<int>(payload.size())) == 1) {
        return;
      }

      // HTTP compression is disabled for probe responses, so this deterministic
      // non-text fallback remains safe if OpenSSL's RNG is unavailable.
      std::uint32_t state = 0x9e3779b9U;
      for (auto &byte : payload) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<char>(state & 0xffU);
      }
    }

    struct send_state_t: std::enable_shared_from_this<send_state_t> {
      std::shared_ptr<impl_t> owner;
      resp_https_t response;
      std::string client;
      std::size_t requested = 0;
      std::size_t sent = 0;
      std::uint64_t admission_id = 0;
      clock_t::time_point started = clock_t::now();
      bool finished = false;

      void
      release_admission() noexcept {
        if (admission_id == 0) {
          return;
        }
        owner->limiter.complete(client, admission_id);
        admission_id = 0;
      }

      void
      finish(std::string_view result) {
        if (finished) {
          return;
        }
        finished = true;
        release_admission();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now() - started).count();
        BOOST_LOG(info) << "event=bandwidth_probe client=" << client_log_id(client)
                        << " requested_bytes=" << requested
                        << " sent_bytes=" << sent
                        << " duration_ms=" << duration
                        << " result=" << result;
      }

      void
      send_next() {
        const auto remaining = requested - sent;
        if (remaining == 0) {
          finish("completed");
          return;
        }

        const auto count = std::min(remaining, owner->payload.size());
        response->write(owner->payload.data(), static_cast<std::streamsize>(count));
        auto self = shared_from_this();
        response->send([self, count](const SimpleWeb::error_code &ec) {
          if (ec) {
            self->finish("cancelled");
            return;
          }
          self->sent += count;
          self->send_next();
        });
      }

      ~send_state_t() {
        // This is only the cancellation safety net. Normal completion and
        // transport errors use finish() above so logging remains explicit.
        release_admission();
      }
    };

    void
    capabilities(resp_https_t response, req_https_t request) {
      if (get_client_cert_uuid_from_request(request).empty()) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_forbidden, "not_paired");
        return;
      }
      json body {
        { "version", 1 },
        { "features", json::array({ "bandwidth-probe-v1" }) },
        { "bandwidthProbe", {
            { "version", 1 },
            { "endpoint", "/api/network/probe" },
            { "minBytes", MIN_BYTES },
            { "maxBytes", MAX_BYTES },
            { "cooldownMs", COOLDOWN_MS },
          } },
      };
      response->write(SimpleWeb::StatusCode::success_ok, body.dump(), json_headers());
    }

    void
    probe(resp_https_t response, req_https_t request) {
      const auto client = get_client_cert_uuid_from_request(request);
      if (client.empty()) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_forbidden, "not_paired");
        return;
      }
      if (request->header.contains("Range")) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "range_not_supported");
        return;
      }

      const auto args = request->parse_query_string();
      const auto bytes_it = args.find("bytes");
      const auto nonce_it = args.find("nonce");
      std::size_t bytes = 0;
      if (bytes_it == args.end() || nonce_it == args.end() ||
          !parse_payload_bytes(bytes_it->second, bytes) || !valid_nonce(nonce_it->second)) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "invalid_request");
        return;
      }
      if (stream::session::has_active_video_sessions()) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_conflict, "stream_active");
        BOOST_LOG(info) << "event=bandwidth_probe client=" << client_log_id(client)
                        << " requested_bytes=" << bytes << " sent_bytes=0 duration_ms=0 result=stream_active";
        return;
      }

      const auto admission = limiter.admit(client, nonce_it->second, bytes);
      if (!admission) {
        write_error(std::move(response), SimpleWeb::StatusCode::client_error_too_many_requests, "rate_limited", admission.retry_after_ms);
        BOOST_LOG(info) << "event=bandwidth_probe client=" << client_log_id(client)
                        << " requested_bytes=" << bytes << " sent_bytes=0 duration_ms=0 result=rate_limited";
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers {
        { "Content-Type", "application/octet-stream" },
        { "Content-Length", std::to_string(bytes) },
        { "Cache-Control", "no-store, no-transform" },
        { "X-Bandwidth-Probe-Version", "1" },
        { "X-Bandwidth-Probe-Nonce", nonce_it->second },
      };
      response->write(SimpleWeb::StatusCode::success_ok, headers);

      auto state = std::make_shared<send_state_t>();
      state->owner = shared_from_this();
      state->response = std::move(response);
      state->client = client;
      state->requested = bytes;
      state->admission_id = admission.id;
      state->send_next();
    }
  };

  service_t::service_t():
      impl_(std::make_shared<impl_t>()) {
  }

  void
  service_t::capabilities(resp_https_t response, req_https_t request) {
    impl_->capabilities(std::move(response), std::move(request));
  }

  void
  service_t::probe(resp_https_t response, req_https_t request) {
    impl_->probe(std::move(response), std::move(request));
  }

}  // namespace nvhttp::network_probe
