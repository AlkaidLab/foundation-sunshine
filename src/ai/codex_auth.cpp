/**
 * @file src/ai/codex_auth.cpp
 * @brief OpenAI Codex device-code OAuth and protected credential lifecycle.
 */

#if defined(_WIN32) && !defined(NOMINMAX)
  #define NOMINMAX
#endif

#include "codex_auth.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace codex_auth {
  namespace {
    using json = nlohmann::json;
    using steady_clock = std::chrono::steady_clock;

    constexpr auto client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
    constexpr auto user_code_url = "https://auth.openai.com/api/accounts/deviceauth/usercode";
    constexpr auto device_token_url = "https://auth.openai.com/api/accounts/deviceauth/token";
    constexpr auto oauth_token_url = "https://auth.openai.com/oauth/token";
    constexpr auto verification_uri = "https://auth.openai.com/codex/device";
    constexpr auto device_redirect_uri = "https://auth.openai.com/deviceauth/callback";
    constexpr int device_lifetime_seconds = 15 * 60;
    constexpr std::size_t max_response_bytes = 64 * 1024;

    struct pending_t {
      std::string device_auth_id;
      std::string user_code;
      std::uint64_t generation = 0;
      int interval_seconds = 1;
      steady_clock::time_point next_poll;
      steady_clock::time_point expires;
      bool poll_in_flight = false;
    };

    struct credential_t {
      std::string access;
      std::string refresh;
      std::string account_id;
      std::int64_t expires_at = 0;
    };

    struct http_response_t {
      long status = 0;
      std::string body;
      std::string error;
      bool too_large = false;
    };

    std::mutex state_mutex;
    std::optional<pending_t> pending;
    std::uint64_t flow_generation = 0;
    std::mutex credential_mutex;
    std::mutex refresh_mutex;
    std::uint64_t credential_generation = 0;

    bool
    is_current_flow(std::uint64_t generation) {
      std::lock_guard lock(state_mutex);
      return pending && pending->generation == generation && flow_generation == generation;
    }

    size_t
    append_response(char *data, size_t size, size_t count, void *context) {
      auto &response = *static_cast<http_response_t *>(context);
      if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        response.too_large = true;
        return 0;
      }
      const size_t bytes = size * count;
      if (bytes > max_response_bytes - response.body.size()) {
        response.too_large = true;
        return 0;
      }
      response.body.append(data, bytes);
      return bytes;
    }

    http_response_t
    post(const char *url, std::string_view body, const char *content_type) {
      http_response_t response;
      if (body.size() > 32 * 1024) {
        response.error = "OpenAI authentication request is too large";
        return response;
      }
      CURL *curl = curl_easy_init();
      if (!curl) {
        response.error = "Could not initialize OpenAI authentication request";
        return response;
      }
      curl_slist *headers = curl_slist_append(nullptr, content_type);
      if (!headers) {
        curl_easy_cleanup(curl);
        response.error = "Could not initialize OpenAI authentication headers";
        return response;
      }
      curl_slist *complete_headers = curl_slist_append(headers, "Accept: application/json");
      if (!complete_headers) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        response.error = "Could not initialize OpenAI authentication headers";
        return response;
      }
      headers = complete_headers;

      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif

      const CURLcode result = curl_easy_perform(curl);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      if (response.too_large) {
        response.error = "OpenAI authentication response is too large";
      } else if (result != CURLE_OK) {
        // Do not expose transport details or server bodies: they may contain tokens.
        response.error = "OpenAI authentication network request failed";
      }
      return response;
    }

    json
    parse_json(std::string_view body) {
      return json::parse(body, nullptr, false);
    }

    std::string
    error_code(const json &body) {
      if (!body.is_object()) return {};
      const auto item = body.find("error");
      if (item == body.end()) return {};
      if (item->is_string()) return item->get<std::string>();
      if (!item->is_object()) return {};
      const auto code = item->find("code");
      return code != item->end() && code->is_string() ? code->get<std::string>() : std::string {};
    }

    std::string
    form_escape(std::string_view value) {
      static constexpr char hex[] = "0123456789ABCDEF";
      std::string encoded;
      encoded.reserve(value.size());
      for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
          encoded.push_back(static_cast<char>(c));
        } else {
          encoded.push_back('%');
          encoded.push_back(hex[c >> 4]);
          encoded.push_back(hex[c & 15]);
        }
      }
      return encoded;
    }

    std::int64_t
    unix_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool
    http_success(long status) {
      return status >= 200 && status < 300;
    }

    bool
    parse_token_response(const http_response_t &response, credential_t &credential) {
      if (!response.error.empty() || !http_success(response.status)) return false;
      const auto body = parse_json(response.body);
      if (!body.is_object() || !body.value("access_token", json {}).is_string() ||
          !body.value("refresh_token", json {}).is_string() ||
          !body.value("expires_in", json {}).is_number_integer()) return false;
      credential.access = body["access_token"].get<std::string>();
      credential.refresh = body["refresh_token"].get<std::string>();
      const auto seconds = body["expires_in"].get<std::int64_t>();
      if (credential.access.empty() || credential.refresh.empty() ||
          credential.access.size() > 16 * 1024 || credential.refresh.size() > 16 * 1024 ||
          seconds <= 0 || seconds > 365 * 24 * 60 * 60) return false;
      credential.account_id = account_id_from_jwt(credential.access);
      if (credential.account_id.empty()) return false;
      credential.expires_at = unix_seconds() + seconds;
      return true;
    }

    std::string
    encode_credential(const credential_t &credential) {
      return json {
        { "version", 1 },
        { "access_token", credential.access },
        { "refresh_token", credential.refresh },
        { "account_id", credential.account_id },
        { "expires_at", credential.expires_at }
      }.dump();
    }

    bool
    decode_credential(std::string_view encoded, credential_t &credential) {
      if (encoded.size() > 32 * 1024) return false;
      const auto body = parse_json(encoded);
      if (!body.is_object() || !body.contains("version") || !body["version"].is_number_integer() ||
          body["version"].get<std::int64_t>() != 1 ||
          !body.value("access_token", json {}).is_string() ||
          !body.value("refresh_token", json {}).is_string() ||
          !body.value("account_id", json {}).is_string() ||
          !body.value("expires_at", json {}).is_number_integer()) return false;
      credential.access = body["access_token"].get<std::string>();
      credential.refresh = body["refresh_token"].get<std::string>();
      credential.account_id = body["account_id"].get<std::string>();
      credential.expires_at = body["expires_at"].get<std::int64_t>();
      return !credential.access.empty() && !credential.refresh.empty() &&
             credential.access.size() <= 16 * 1024 && credential.refresh.size() <= 16 * 1024 &&
             !credential.account_id.empty() && credential.account_id.size() <= 256 &&
             credential.account_id == account_id_from_jwt(credential.access) &&
             credential.expires_at > 0;
    }

    std::optional<credential_t>
    read_credential(const std::filesystem::path &path, std::string &error) {
      auto stored = credential_store::read_codex_credential(path);
      if (stored.status == credential_store::read_status_e::not_found) return std::nullopt;
      if (stored.status == credential_store::read_status_e::error) {
        error = stored.error;
        return std::nullopt;
      }
      credential_t credential;
      if (!decode_credential(stored.secret, credential)) {
        error = "Stored OpenAI account credential is invalid";
        return std::nullopt;
      }
      return credential;
    }

    std::optional<credential_t>
    exchange_code(std::string_view code, std::string_view verifier, std::string &error) {
      const std::string body =
        "grant_type=authorization_code&client_id=" + std::string(client_id) +
        "&code=" + form_escape(code) +
        "&code_verifier=" + form_escape(verifier) +
        "&redirect_uri=" + form_escape(device_redirect_uri);
      const auto response = post(oauth_token_url, body, "Content-Type: application/x-www-form-urlencoded");
      if (!response.error.empty()) {
        error = response.error;
        return std::nullopt;
      }
      credential_t credential;
      if (!parse_token_response(response, credential)) {
        error = "OpenAI token exchange failed (HTTP " + std::to_string(response.status) + ")";
        return std::nullopt;
      }
      return credential;
    }
  }  // namespace

  detail::poll_response_e
  detail::classify_device_poll_response(long http_status, std::string_view response_body) {
    const auto code = error_code(parse_json(response_body));
    if (code == "deviceauth_authorization_pending") return poll_response_e::pending;
    if (code == "slow_down") return poll_response_e::slow_down;
    if (http_status == 403 || http_status == 404) return poll_response_e::pending;
    if (http_success(http_status)) return poll_response_e::authorized;
    return poll_response_e::error;
  }

  std::string
  account_id_from_jwt(std::string_view access_token) {
    if (access_token.size() > 16 * 1024) return {};
    const auto first = access_token.find('.');
    if (first == std::string_view::npos) return {};
    const auto second = access_token.find('.', first + 1);
    if (second == std::string_view::npos || access_token.find('.', second + 1) != std::string_view::npos) return {};
    const auto payload = access_token.substr(first + 1, second - first - 1);
    if (payload.empty() || payload.size() > 16 * 1024) return {};
    std::string decoded;
    decoded.reserve(payload.size() * 3 / 4);
    unsigned int bits = 0;
    int bit_count = 0;
    for (const char ch : payload) {
      unsigned int value;
      if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
      else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
      else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
      else if (ch == '-') value = 62;
      else if (ch == '_') value = 63;
      else return {};
      bits = (bits << 6) | value;
      bit_count += 6;
      if (bit_count >= 8) {
        bit_count -= 8;
        decoded.push_back(static_cast<char>((bits >> bit_count) & 0xff));
      }
    }
    if (bit_count >= 6 || (bits & ((1U << bit_count) - 1)) != 0) return {};
    const auto body = parse_json(decoded);
    if (!body.is_object()) return {};
    const auto auth = body.find("https://api.openai.com/auth");
    if (auth == body.end() || !auth->is_object()) return {};
    const auto account_id = auth->find("chatgpt_account_id");
    if (account_id == auth->end() || !account_id->is_string()) return {};
    auto value = account_id->get<std::string>();
    return value.size() <= 256 ? value : std::string {};
  }

  start_result_t
  start() {
#if !defined(_WIN32)
    return { false, {}, {}, 0, 0, "Secure OpenAI account persistence is only supported on Windows" };
#else
    std::uint64_t generation;
    {
      std::lock_guard lock(state_mutex);
      generation = ++flow_generation;
      pending.reset();
    }

    const auto response = post(user_code_url, json { { "client_id", client_id } }.dump(),
                               "Content-Type: application/json");
    if (!response.error.empty()) return { false, {}, {}, 0, 0, response.error };
    if (!http_success(response.status)) {
      return { false, {}, {}, 0, 0,
               "OpenAI device sign-in could not start (HTTP " + std::to_string(response.status) + ")" };
    }
    const auto body = parse_json(response.body);
    if (!body.is_object() || !body.value("device_auth_id", json {}).is_string() ||
        !body.value("user_code", json {}).is_string()) {
      return { false, {}, {}, 0, 0, "Invalid OpenAI device sign-in response" };
    }
    std::string device_auth_id = body["device_auth_id"].get<std::string>();
    std::string user_code = body["user_code"].get<std::string>();
    int interval = 5;
    if (body.contains("interval")) {
      if (body["interval"].is_number_integer()) {
        const auto parsed_interval = body["interval"].get<std::int64_t>();
        if (parsed_interval < 0 || parsed_interval > 60) {
          return { false, {}, {}, 0, 0, "Invalid OpenAI device sign-in interval" };
        }
        interval = static_cast<int>(parsed_interval);
      } else if (body["interval"].is_string()) {
        try {
          const auto value = body["interval"].get<std::string>();
          std::size_t consumed = 0;
          interval = std::stoi(value, &consumed);
          if (consumed != value.size()) throw std::invalid_argument("trailing characters");
        } catch (...) {
          return { false, {}, {}, 0, 0, "Invalid OpenAI device sign-in interval" };
        }
      } else {
        return { false, {}, {}, 0, 0, "Invalid OpenAI device sign-in interval" };
      }
    }
    if (device_auth_id.empty() || device_auth_id.size() > 2048 ||
        user_code.empty() || user_code.size() > 256 || interval < 0 || interval > 60) {
      return { false, {}, {}, 0, 0, "Invalid OpenAI device sign-in response" };
    }
    interval = std::max(1, interval);
    const auto now = steady_clock::now();
    {
      std::lock_guard lock(state_mutex);
      if (generation != flow_generation) {
        return { false, {}, {}, 0, 0, "OpenAI device sign-in was superseded" };
      }
      pending = pending_t { std::move(device_auth_id), user_code, generation, interval,
                            now + std::chrono::seconds(interval),
                            now + std::chrono::seconds(device_lifetime_seconds), false };
    }
    return { true, std::move(user_code), verification_uri, interval, device_lifetime_seconds, {},
             std::to_string(generation) };
#endif
  }

  poll_result_t
  poll(const std::filesystem::path &credential_path, std::string_view flow_id) {
    pending_t flow;
    {
      std::lock_guard lock(state_mutex);
      if (!pending) return { poll_state_e::error, 0, "No OpenAI device sign-in is pending" };
      if (flow_id.empty() || flow_id != std::to_string(pending->generation)) {
        return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
      }
      const auto now = steady_clock::now();
      if (now >= pending->expires) {
        pending.reset();
        ++flow_generation;
        return { poll_state_e::expired, 0, "OpenAI device sign-in expired" };
      }
      if (pending->poll_in_flight || now < pending->next_poll) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(pending->next_poll - now).count();
        return { poll_state_e::pending, std::max(1, static_cast<int>(remaining + 1)), {} };
      }
      pending->poll_in_flight = true;
      pending->next_poll = now + std::chrono::seconds(pending->interval_seconds);
      flow = *pending;
    }

    const auto response = post(device_token_url,
                               json { { "device_auth_id", flow.device_auth_id },
                                      { "user_code", flow.user_code } }.dump(),
                               "Content-Type: application/json");
    if (!response.error.empty()) {
      std::lock_guard lock(state_mutex);
      if (!pending || pending->generation != flow.generation) {
        return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
      }
      pending->poll_in_flight = false;
      return { poll_state_e::retryable_error, flow.interval_seconds, response.error };
    }
    if (!is_current_flow(flow.generation)) {
      return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
    }

    const auto outcome = detail::classify_device_poll_response(response.status, response.body);
    if (outcome == detail::poll_response_e::pending) {
      std::lock_guard lock(state_mutex);
      if (!pending || pending->generation != flow.generation) {
        return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
      }
      pending->poll_in_flight = false;
      return { poll_state_e::pending, pending->interval_seconds, {} };
    }
    if (outcome == detail::poll_response_e::slow_down) {
      std::lock_guard lock(state_mutex);
      if (!pending || pending->generation != flow.generation) {
        return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
      }
      pending->poll_in_flight = false;
      pending->interval_seconds = std::min(60, pending->interval_seconds + 5);
      pending->next_poll = steady_clock::now() + std::chrono::seconds(pending->interval_seconds);
      return { poll_state_e::pending, pending->interval_seconds, {} };
    }
    if (outcome == detail::poll_response_e::error) {
      std::lock_guard lock(state_mutex);
      if (pending && pending->generation == flow.generation) {
        pending.reset();
        ++flow_generation;
      }
      return { poll_state_e::error, 0,
               "OpenAI device sign-in failed (HTTP " + std::to_string(response.status) + ")" };
    }
    const auto body = parse_json(response.body);
    if (!body.is_object() || !body.value("authorization_code", json {}).is_string() ||
        !body.value("code_verifier", json {}).is_string()) {
      std::lock_guard lock(state_mutex);
      if (pending && pending->generation == flow.generation) {
        pending.reset();
        ++flow_generation;
      }
      return { poll_state_e::error, 0, "Invalid OpenAI device authorization response" };
    }
    const auto authorization_code = body["authorization_code"].get<std::string>();
    const auto code_verifier = body["code_verifier"].get<std::string>();
    if (authorization_code.empty() || code_verifier.empty() ||
        authorization_code.size() > 2048 || code_verifier.size() > 2048) {
      std::lock_guard lock(state_mutex);
      if (pending && pending->generation == flow.generation) {
        pending.reset();
        ++flow_generation;
      }
      return { poll_state_e::error, 0, "Invalid OpenAI device authorization response" };
    }
    std::string error;
    const auto credential = exchange_code(authorization_code, code_verifier, error);
    std::lock_guard state_lock(state_mutex);
    if (!pending || pending->generation != flow.generation || flow_generation != flow.generation) {
      return { poll_state_e::error, 0, "OpenAI device sign-in was superseded" };
    }
    pending.reset();
    ++flow_generation;
    if (!credential) return { poll_state_e::error, 0, error };
    std::lock_guard credential_lock(credential_mutex);
    ++credential_generation;
    const auto saved = credential_store::write_codex_credential(credential_path, encode_credential(*credential));
    if (!saved.success) return { poll_state_e::error, 0, saved.error };
    return { poll_state_e::complete, 0, {} };
  }

  status_result_t
  status(const std::filesystem::path &credential_path) {
    status_result_t result;
    {
      std::lock_guard lock(state_mutex);
      const auto now = steady_clock::now();
      if (pending && now >= pending->expires) {
        pending.reset();
        ++flow_generation;
      }
      if (pending) {
        result.pending = true;
        result.user_code = pending->user_code;
        result.verification_uri = verification_uri;
        result.flow_id = std::to_string(pending->generation);
        result.interval_seconds = pending->interval_seconds;
      }
    }
    std::string error;
    std::optional<credential_t> credential;
    {
      std::lock_guard lock(credential_mutex);
      credential = read_credential(credential_path, error);
    }
    if (!credential) {
      result.error = std::move(error);
      return result;
    }
    result.connected = true;
    result.account_id = credential->account_id;
    return result;
  }

  credential_store::mutation_result_t
  logout(const std::filesystem::path &credential_path) {
    {
      std::lock_guard lock(state_mutex);
      pending.reset();
      ++flow_generation;
    }
    std::lock_guard lock(credential_mutex);
    ++credential_generation;
    return credential_store::erase_codex_credential(credential_path);
  }

  token_result_t
  access_token(const std::filesystem::path &credential_path) {
    std::string error;
    std::optional<credential_t> credential;
    std::uint64_t generation = 0;
    {
      std::lock_guard lock(credential_mutex);
      credential = read_credential(credential_path, error);
      generation = credential_generation;
    }
    if (!credential) {
      return { false, {}, {}, error.empty() ? "OpenAI account is not connected" : error };
    }
    if (credential->expires_at > unix_seconds() + 60) {
      return { true, std::move(credential->access), std::move(credential->account_id), {} };
    }

    std::lock_guard refresh_lock(refresh_mutex);
    {
      std::lock_guard credential_lock(credential_mutex);
      credential = read_credential(credential_path, error);
      generation = credential_generation;
    }
    if (!credential) {
      return { false, {}, {}, error.empty() ? "OpenAI account is not connected" : error };
    }
    if (credential->expires_at > unix_seconds() + 60) {
      return { true, std::move(credential->access), std::move(credential->account_id), {} };
    }

    const std::string body = "grant_type=refresh_token&refresh_token=" + form_escape(credential->refresh) +
                             "&client_id=" + std::string(client_id);
    const auto response = post(oauth_token_url, body, "Content-Type: application/x-www-form-urlencoded");
    if (!response.error.empty()) return { false, {}, {}, response.error };
    credential_t refreshed;
    if (!parse_token_response(response, refreshed)) {
      if (response.status == 400 || response.status == 401 || response.status == 403) {
        std::lock_guard credential_lock(credential_mutex);
        if (generation == credential_generation) {
          const auto erased = credential_store::erase_codex_credential(credential_path);
          if (!erased.success) {
            return { false, {}, {}, "OpenAI account token refresh was rejected, but stored credential could not be cleared: " + erased.error };
          }
          ++credential_generation;
        }
      }
      return { false, {}, {}, "OpenAI account token refresh failed (HTTP " +
                              std::to_string(response.status) + ")" };
    }

    {
      std::lock_guard credential_lock(credential_mutex);
      if (generation != credential_generation) {
        return { false, {}, {}, "OpenAI account credential changed during token refresh" };
      }
      const auto saved = credential_store::write_codex_credential(credential_path, encode_credential(refreshed));
      if (!saved.success) return { false, {}, {}, saved.error };
      ++credential_generation;
    }
    credential = std::move(refreshed);
    return { true, std::move(credential->access), std::move(credential->account_id), {} };
  }

}  // namespace codex_auth
