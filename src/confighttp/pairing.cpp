/**
 * @file src/confighttp/pairing.cpp
 * @brief Pending-PIN pairing routes for the Web UI.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "pairing.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"

namespace confighttp::pairing {
  std::string
  validate_pin_request(const pin_request_t &request) {
    if (request.pin.size() < 4 || request.pin.size() > 8 ||
        !std::all_of(request.pin.begin(), request.pin.end(), [](unsigned char character) {
          return std::isdigit(character);
        })) {
      return "PIN must be 4-8 digits";
    }
    if (request.name.empty() || request.name.size() > 256) {
      return "Client name must be 1-256 characters";
    }
    if (!request.id.empty()) {
      if (request.id.size() > 64 ||
          !std::all_of(request.id.begin(), request.id.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_';
          })) {
        return "id must be 1-64 characters of [A-Za-z0-9_-]";
      }
    }
    return {};
  }

  namespace {
    constexpr int max_wait_seconds = 25;
    constexpr int max_waiters_total = 8;
    constexpr int max_waiters_per_address = 2;

    // Long-poll waiters run outside the HTTPS worker pool so a sleeping request
    // never consumes one of the listener's two worker threads. The counters
    // bound the detached threads; when the bound is reached the handler
    // answers immediately with the current snapshot.
    // Source keys: pair-libstdcxx-concurrency-20261103,
    // pair-msvc-condition-variable-20261103, ds5-rfc9110-http-20261031.
    std::mutex waiters_mutex;
    std::unordered_map<std::string, int> waiters_per_address;
    int waiters_total = 0;

    bool
    try_acquire_waiter(const std::string &address) {
      std::lock_guard lock { waiters_mutex };
      if (waiters_total >= max_waiters_total) {
        return false;
      }
      auto &count = waiters_per_address[address];
      if (count >= max_waiters_per_address) {
        return false;
      }
      ++count;
      ++waiters_total;
      return true;
    }

    void
    release_waiter(const std::string &address) {
      std::lock_guard lock { waiters_mutex };
      const auto entry = waiters_per_address.find(address);
      if (entry == waiters_per_address.end()) {
        return;
      }
      if (--entry->second <= 0) {
        waiters_per_address.erase(entry);
      }
      if (waiters_total > 0) {
        --waiters_total;
      }
    }

    std::optional<std::uint64_t>
    parse_unsigned(std::string_view text) {
      if (text.empty()) {
        return std::nullopt;
      }
      std::uint64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc {} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    }

    void
    write_pending_response(const resp_https_t &response, const nvhttp::pending_pair_snapshot_t &snapshot) {
      // Source key: ds5-rfc8259-json-20261031 sections 1, 2 and 4.
      nlohmann::json body;
      body["pending"] = !snapshot.requests.empty();
      body["generation"] = snapshot.generation;
      body["requests"] = nlohmann::json::array();
      for (const auto &request : snapshot.requests) {
        body["requests"].push_back(nlohmann::json {
          { "id", request.id },
          { "device_name", request.device_name },
          { "started_at", request.started_at },
          { "expires_at", request.expires_at },
        });
      }

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("Cache-Control", "no-store");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(body.dump(), headers);
    }

    void
    getPending(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) {
        return;
      }
      print_req(request);

      int wait_seconds = 0;
      std::optional<std::uint64_t> since;
      const auto args = request->parse_query_string();
      if (const auto entry = args.find("wait"); entry != args.end()) {
        const auto parsed = parse_unsigned(entry->second);
        if (!parsed) {
          write_json_error(response, web::StatusCode::bad_request, "wait must be a non-negative integer");
          return;
        }
        wait_seconds = static_cast<int>(std::min<std::uint64_t>(*parsed, max_wait_seconds));
      }
      if (const auto entry = args.find("since"); entry != args.end()) {
        const auto parsed = parse_unsigned(entry->second);
        if (!parsed) {
          write_json_error(response, web::StatusCode::bad_request, "since must be an unsigned integer");
          return;
        }
        since = *parsed;
      }

      auto snapshot = nvhttp::get_pending_pair_requests();
      bool should_wait = wait_seconds > 0;
      if (should_wait) {
        // A cursor makes the poll edge-triggered. Without one, an already
        // pending request is returned immediately so a first paint never
        // waits for the next change.
        should_wait = since ? (*since == snapshot.generation) : snapshot.requests.empty();
      }
      if (!should_wait) {
        write_pending_response(response, snapshot);
        return;
      }

      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      if (!try_acquire_waiter(address)) {
        write_pending_response(response, snapshot);
        return;
      }

      const auto generation = snapshot.generation;
      const auto wait = std::chrono::seconds(wait_seconds);
      try {
        std::thread([response, generation, wait, address] {
          nvhttp::wait_for_pending_pair_change(generation, wait);
          const auto latest = nvhttp::get_pending_pair_requests();
          try {
            if (!response->closed()) {
              write_pending_response(response, latest);
            }
          }
          catch (...) {
            // The connection may have gone away while the waiter slept.
          }
          release_waiter(address);
        }).detach();
      }
      catch (...) {
        release_waiter(address);
        write_pending_response(response, snapshot);
      }
    }

    void
    rejectPending(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) {
        return;
      }
      print_req(request);

      if (request->path_match.size() < 2) {
        write_json_error(response, web::StatusCode::bad_request, "missing pending pairing request id");
        return;
      }

      const std::string request_id = request->path_match[1].str();
      if (!nvhttp::reject_pending_pair_request(request_id)) {
        write_json_error(response, web::StatusCode::not_found, "pending pairing request not found");
        return;
      }

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("Cache-Control", "no-store");
      response->write(nlohmann::json { { "status", true } }.dump(), headers);
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/pairing/pending$"]["GET"] = getPending;
    server.resource["^/api/pairing/pending/([A-Za-z0-9_-]{1,64})$"]["DELETE"] = rejectPending;
  }
}  // namespace confighttp::pairing
