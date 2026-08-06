/**
 * @file src/client_fingerprint.h
 * @brief Conservative, non-blocking identification of distinctive client request patterns.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace client_fingerprint {
  inline constexpr std::string_view suspicious_client_code = "suspected_unknown_infringing_client";
  inline constexpr std::string_view built_in_rule_id = "axixi-moonlight-android-2026-07";

  using arguments_t = std::map<std::string, std::string, std::less<>>;

  enum class operation_t {
    present,
    equals,
    not_equal,
    one_of,
    prefix,
  };

  struct predicate_t {
    std::string query;
    operation_t operation = operation_t::present;
    std::string value;
    std::vector<std::string> values;
  };

  struct rule_t {
    std::string id;
    bool enabled = true;
    std::string message_key;
    std::vector<predicate_t> predicates;
  };

  struct rule_set_t {
    std::uint64_t revision = 0;
    std::int64_t expires_at = 0;
    std::vector<rule_t> rules;
  };

  struct parse_result_t {
    std::shared_ptr<const rule_set_t> rules;
    std::string error;

    explicit operator bool() const noexcept {
      return static_cast<bool>(rules);
    }
  };

  struct match_result_t {
    bool suspicious = false;
    std::string rule_id;
    std::string message_key;
    std::uint64_t revision = 0;
    std::string source;
  };

  struct status_t {
    std::uint64_t revision = 0;
    std::string source = "built-in";
    std::string last_error;
    bool remote_rules_enabled = false;
  };

  struct options_t {
    bool remote_rules_enabled = true;
    std::filesystem::path signing_certificate;
    std::filesystem::path cache_file;
  };

  struct install_result_t {
    bool installed = false;
    bool unchanged = false;
    std::uint64_t revision = 0;
    std::string error;

    explicit operator bool() const noexcept {
      return error.empty();
    }
  };

  class deinit_t {
  public:
    deinit_t(const deinit_t &) = delete;
    deinit_t &operator=(const deinit_t &) = delete;
    ~deinit_t();

  private:
    friend std::unique_ptr<deinit_t> init(options_t options) noexcept;
    struct impl_t;

    explicit deinit_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl_;
  };

  /**
   * Verify and parse a signed rule envelope.
   *
   * The envelope contains base64-encoded `payload` and `signature` strings.
   * The signature is SHA-256 over the exact decoded payload bytes using the
   * private key corresponding to `certificate_pem`.
   */
  parse_result_t
  parse_signed_rules(
    std::string_view envelope,
    std::string_view certificate_pem,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
  );

  /** Match normalized launch arguments against the current active rules. */
  match_result_t
  match(const arguments_t &args);

  /** Return a thread-safe snapshot of the active rule updater state. */
  status_t
  status();

  /**
   * Verify, persist, and activate a candidate envelope supplied by the local
   * GUI transport. The GUI is deliberately not trusted to validate rules.
   */
  install_result_t
  install_signed_rules(std::string_view envelope);

  /**
   * Load the built-in fallback and the verified last-known-good cache.
   */
  [[nodiscard]] std::unique_ptr<deinit_t>
  init(options_t options) noexcept;

  template<class Args>
  match_result_t
  match_client(const Args &args) {
    arguments_t normalized;
    for (const auto &[key, value] : args) {
      normalized.try_emplace(std::string {key}, std::string {value});
    }
    return match(normalized);
  }

  /**
   * Compatibility helper used by callers that only need the warning bit.
   *
   * Every value is client-controlled. This is intentionally informational
   * and must not be used as an authentication or authorization check.
   */
  template<class Args>
  bool
  is_highly_suspected_unknown_client(const Args &args) {
    return match_client(args).suspicious;
  }
}  // namespace client_fingerprint
