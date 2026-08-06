/**
 * @file src/client_fingerprint.cpp
 * @brief Signed dynamic rule loading and conservative client fingerprint evaluation.
 */

#include "client_fingerprint.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "crypto.h"
#include "logging.h"

namespace client_fingerprint {
  namespace {
    using json = nlohmann::json;
    using namespace std::chrono_literals;

    constexpr std::size_t max_envelope_bytes = 512 * 1024;
    constexpr std::size_t max_payload_bytes = 256 * 1024;
    constexpr std::size_t max_signature_bytes = 16 * 1024;
    constexpr std::size_t max_rules = 64;
    constexpr std::size_t max_predicates = 16;
    constexpr std::size_t max_one_of_values = 16;
    constexpr std::size_t max_identifier_bytes = 96;
    constexpr std::size_t max_query_bytes = 64;
    constexpr std::size_t max_value_bytes = 256;
    constexpr auto max_feed_lifetime = 180 * 24h;
    constexpr auto allowed_clock_skew = 5min;
    constexpr std::string_view built_in_signing_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIEdTCCAt2gAwIBAgIUI3ZvACPZVxrOB1GZRlVKxb54DvwwDQYJKoZIhvcNAQEL
BQAwSjE0MDIGA1UEAwwrQWxrYWlkTGFiIFN1bnNoaW5lIENsaWVudCBGaW5nZXJw
cmludCBSdWxlczESMBAGA1UECgwJQWxrYWlkTGFiMB4XDTI2MDczMDE3MzM1OFoX
DTM2MDcyNzE3MzM1OFowSjE0MDIGA1UEAwwrQWxrYWlkTGFiIFN1bnNoaW5lIENs
aWVudCBGaW5nZXJwcmludCBSdWxlczESMBAGA1UECgwJQWxrYWlkTGFiMIIBojAN
BgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAmlpsbhHUc6XgbChaAjvBfyiqxULB
RTTsmKiXFh2Wgmk4UynIhcxRTQ6aFzQRxAojpwbYg79H4l8zWpKhSyWPqsEmxHOc
iZyTkTTEUtw0lbs8dv+p/TUiGhnX/SCZ2wqIAnrNNw0KFpbK6wmrnB7+NtipyAKs
lzCX0DzSfhz/Z5aK7qtGSabXYUmGIlVrwk5DqIyAnW+yuqGJuBBs1rjPXzB7nco7
F2b8NS9h7vr0tj/CTnulaRNy+U5LKtsR95gUV/S+q4RWHAyVTl049OvWzO7XNwgY
wEzSQur8yVVlgeQcBa2gM7YDHOnofMxlWiOHdbabUecBY5NztuNtfOhslh53JRqF
IIbnLMVf1/mn5f+zgfcBtJwEt6ucexbitixR17JZDdHZCVPpms6zRMOITf3zH4x0
GxLyQEUvQWICOtQ1txID5ZPha5aajj2gSdqss219t6dzgdlVpje7hkx6KcV9dRNI
hkV9I+9wGea8oiPKYtY62ra/5WFHCVFs1ZRFAgMBAAGjUzBRMB0GA1UdDgQWBBSF
KfXILcBwrZpp+9xBFdeiuI0J+jAfBgNVHSMEGDAWgBSFKfXILcBwrZpp+9xBFdei
uI0J+jAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBgQCFRe96FKSf
ciDDjBxqTJi8mdB9ZTevE0W0jreCj8spL8nS+0VXiU6V4/3J9QPZBeBZzEIBbS4O
+scQ3h10ok2kn4meNYuQ9KZRVEMgI7kBJfQ9nBaCMJPSaf+wK41+LNhVnIUAguiG
Qd5QGvh+EG5y4+AFP3MGMUd5zw+pXmwYV3IuCP9KumaoDLNL3SthMPHOJgv0zYzJ
2mkTL6EVRcRMPFCz03PTFJuUkeAXBY/OOBGBo4mSk0EWielpJ2wKzxXYf8OR2rrc
+LlM4v5Ibd+7qEuQx0U8CL/rxn+SX8fNZ+AhSNDecz5N+1Xgd24BdItgH5e8MkLU
/i1qltD97zDNAQgi7q/GpYVBYGA/lJsYTFj5VkoFRuAR6Z10CydhpCo81V4i1ZLC
K4B6JulL3BbTAzNZp2wVqJO33GUKKLlyirGOW4riahjogcKN5n6eZKRLRf3OMlOK
BuXdcXkyIk9BYg0tTStl015bfxdjWm/U2g8ULOVKtseXH4iGyaPdnJ0=
-----END CERTIFICATE-----
)PEM";

    struct active_state_t {
      std::shared_ptr<const rule_set_t> rules;
      status_t status;
      std::uint64_t remote_revision = 0;
      options_t options;
      bool initialized = false;
    };

    std::mutex active_mutex;
    std::mutex install_mutex;
    active_state_t active_state;

    bool
    safe_identifier(std::string_view value, std::size_t max_size) {
      if (value.empty() || value.size() > max_size) {
        return false;
      }
      return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') ||
               (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') ||
               ch == '-' || ch == '_' || ch == '.';
      });
    }

    std::shared_ptr<const rule_set_t>
    built_in_rules() {
      auto rules = std::make_shared<rule_set_t>();
      rules->rules.push_back(rule_t {
        .id = std::string {built_in_rule_id},
        .enabled = true,
        .message_key = std::string {suspicious_client_code},
        .predicates = {
          {"virtualDisplay", operation_t::present, {}, {}},
          {"virtualDisplay", operation_t::not_equal, "0", {}},
          {"virtualDisplayMode", operation_t::present, {}, {}},
          {"devicenickname", operation_t::present, {}, {}},
          {"ppi", operation_t::present, {}, {}},
          {"screen_resolution", operation_t::present, {}, {}},
          {"timeToTerminateApp", operation_t::equals, "-1", {}},
          {"UIScale", operation_t::equals, "200", {}},
        },
      });
      return rules;
    }

    void
    ensure_built_in_state() {
      std::lock_guard lock {active_mutex};
      if (!active_state.rules) {
        active_state.rules = built_in_rules();
        active_state.status = {};
      }
    }

    std::optional<operation_t>
    parse_operation(std::string_view value) {
      if (value == "present") {
        return operation_t::present;
      }
      if (value == "equals") {
        return operation_t::equals;
      }
      if (value == "not_equal") {
        return operation_t::not_equal;
      }
      if (value == "one_of") {
        return operation_t::one_of;
      }
      if (value == "prefix") {
        return operation_t::prefix;
      }
      return std::nullopt;
    }

    std::optional<std::string>
    decode_base64(std::string_view encoded, std::size_t max_decoded_bytes) {
      if (encoded.empty() || encoded.size() % 4 != 0 ||
          encoded.size() > ((max_decoded_bytes + 2) / 3) * 4) {
        return std::nullopt;
      }

      std::size_t padding = 0;
      if (encoded.ends_with("==")) {
        padding = 2;
      }
      else if (encoded.ends_with("=")) {
        padding = 1;
      }

      for (std::size_t i = 0; i < encoded.size(); ++i) {
        const auto ch = static_cast<unsigned char>(encoded[i]);
        const bool alphabet =
          (ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') ||
          ch == '+' || ch == '/';
        if (!alphabet && !(ch == '=' && i >= encoded.size() - padding)) {
          return std::nullopt;
        }
      }

      std::string decoded((encoded.size() / 4) * 3, '\0');
      const int written = EVP_DecodeBlock(
        reinterpret_cast<unsigned char *>(decoded.data()),
        reinterpret_cast<const unsigned char *>(encoded.data()),
        static_cast<int>(encoded.size())
      );
      if (written < 0 || static_cast<std::size_t>(written) < padding) {
        return std::nullopt;
      }
      decoded.resize(static_cast<std::size_t>(written) - padding);
      if (decoded.size() > max_decoded_bytes) {
        return std::nullopt;
      }
      return decoded;
    }

    bool
    read_required_string(
      const json &object,
      const char *key,
      std::string &out,
      std::size_t max_size,
      std::string &error
    ) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_string()) {
        error = std::string {"missing or invalid string field: "} + key;
        return false;
      }
      out = it->get<std::string>();
      if (out.empty() || out.size() > max_size) {
        error = std::string {"field has invalid length: "} + key;
        return false;
      }
      return true;
    }

    parse_result_t
    parse_payload(
      std::string_view payload,
      std::chrono::system_clock::time_point now
    ) {
      json root;
      try {
        root = json::parse(payload);
      }
      catch (const json::exception &e) {
        return {nullptr, std::string {"invalid rule payload JSON: "} + e.what()};
      }

      if (!root.is_object() || root.value("schema_version", 0) != 1) {
        return {nullptr, "unsupported client fingerprint rule schema"};
      }
      if (!root.contains("revision") || !root["revision"].is_number_unsigned()) {
        return {nullptr, "missing or invalid rule revision"};
      }
      const auto revision = root["revision"].get<std::uint64_t>();
      if (revision == 0) {
        return {nullptr, "rule revision must be greater than zero"};
      }
      if (!root.contains("issued_at") || !root["issued_at"].is_number_integer() ||
          !root.contains("expires_at") || !root["expires_at"].is_number_integer()) {
        return {nullptr, "issued_at and expires_at must be Unix timestamps"};
      }

      const auto issued_at = root["issued_at"].get<std::int64_t>();
      const auto expires_at = root["expires_at"].get<std::int64_t>();
      const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
      ).count();
      if (issued_at > now_seconds + std::chrono::duration_cast<std::chrono::seconds>(allowed_clock_skew).count()) {
        return {nullptr, "rule feed was issued in the future"};
      }
      if (expires_at <= now_seconds) {
        return {nullptr, "rule feed has expired"};
      }
      const auto max_lifetime_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(max_feed_lifetime).count();
      if (expires_at <= issued_at ||
          issued_at < expires_at - max_lifetime_seconds) {
        return {nullptr, "rule feed lifetime is invalid"};
      }

      const auto rules_it = root.find("rules");
      if (rules_it == root.end() || !rules_it->is_array() || rules_it->size() > max_rules) {
        return {nullptr, "rules must be an array within the configured size limit"};
      }

      auto parsed = std::make_shared<rule_set_t>();
      parsed->revision = revision;
      parsed->expires_at = expires_at;
      std::set<std::string, std::less<>> ids;

      for (const auto &item : *rules_it) {
        if (!item.is_object()) {
          return {nullptr, "each rule must be an object"};
        }

        rule_t rule;
        std::string error;
        if (!read_required_string(item, "id", rule.id, max_identifier_bytes, error) ||
            !safe_identifier(rule.id, max_identifier_bytes)) {
          return {nullptr, error.empty() ? "rule id contains unsupported characters" : error};
        }
        if (!ids.emplace(rule.id).second) {
          return {nullptr, "duplicate rule id: " + rule.id};
        }
        if (item.contains("enabled") && !item["enabled"].is_boolean()) {
          return {nullptr, "enabled must be a boolean for rule: " + rule.id};
        }
        rule.enabled = item.value("enabled", true);

        if (!rule.enabled) {
          parsed->rules.push_back(std::move(rule));
          continue;
        }

        if (item.value("action", std::string {}) != "warn") {
          return {nullptr, "remote rules may only use the warn action"};
        }
        if (item.value("confidence", std::string {}) != "high") {
          return {nullptr, "remote rules must declare high confidence"};
        }
        if (!read_required_string(item, "message_key", rule.message_key, max_identifier_bytes, error) ||
            !safe_identifier(rule.message_key, max_identifier_bytes)) {
          return {nullptr, error.empty() ? "message_key contains unsupported characters" : error};
        }
        if (rule.message_key != suspicious_client_code) {
          return {nullptr, "remote rule uses an unsupported message_key"};
        }

        const auto predicates_it = item.find("all");
        if (predicates_it == item.end() || !predicates_it->is_array() ||
            predicates_it->empty() || predicates_it->size() > max_predicates) {
          return {nullptr, "all must contain a bounded non-empty predicate array"};
        }

        for (const auto &predicate_json : *predicates_it) {
          if (!predicate_json.is_object()) {
            return {nullptr, "each predicate must be an object"};
          }

          predicate_t predicate;
          std::string op;
          if (!read_required_string(predicate_json, "query", predicate.query, max_query_bytes, error) ||
              !safe_identifier(predicate.query, max_query_bytes) ||
              !read_required_string(predicate_json, "op", op, 32, error)) {
            return {nullptr, error.empty() ? "predicate query contains unsupported characters" : error};
          }
          const auto parsed_operation = parse_operation(op);
          if (!parsed_operation) {
            return {nullptr, "unsupported predicate operation: " + op};
          }
          predicate.operation = *parsed_operation;

          if (predicate.operation == operation_t::one_of) {
            const auto values_it = predicate_json.find("values");
            if (values_it == predicate_json.end() || !values_it->is_array() ||
                values_it->empty() || values_it->size() > max_one_of_values) {
              return {nullptr, "one_of requires a bounded non-empty values array"};
            }
            for (const auto &value : *values_it) {
              if (!value.is_string()) {
                return {nullptr, "one_of values must be strings"};
              }
              auto decoded = value.get<std::string>();
              if (decoded.size() > max_value_bytes) {
                return {nullptr, "predicate value exceeds the size limit"};
              }
              predicate.values.push_back(std::move(decoded));
            }
          }
          else if (predicate.operation != operation_t::present) {
            if (!read_required_string(predicate_json, "value", predicate.value, max_value_bytes, error)) {
              return {nullptr, error};
            }
          }

          rule.predicates.push_back(std::move(predicate));
        }
        parsed->rules.push_back(std::move(rule));
      }

      return {std::move(parsed), {}};
    }

    bool
    predicate_matches(const predicate_t &predicate, const arguments_t &args) {
      const auto it = args.find(predicate.query);
      const bool present = it != args.end() && !it->second.empty();
      const std::string_view value = present ? std::string_view {it->second} : std::string_view {};

      switch (predicate.operation) {
        case operation_t::present:
          return present;
        case operation_t::equals:
          return present && value == predicate.value;
        case operation_t::not_equal:
          return present && value != predicate.value;
        case operation_t::one_of:
          return present && std::find(predicate.values.begin(), predicate.values.end(), value) != predicate.values.end();
        case operation_t::prefix:
          return present && value.starts_with(predicate.value);
      }
      return false;
    }

    std::shared_ptr<const rule_set_t>
    merge_with_built_in(const rule_set_t &remote) {
      std::map<std::string, rule_t, std::less<>> merged;
      for (const auto &rule : built_in_rules()->rules) {
        merged.emplace(rule.id, rule);
      }
      for (const auto &rule : remote.rules) {
        if (rule.enabled) {
          merged.insert_or_assign(rule.id, rule);
        }
        else {
          merged.erase(rule.id);
        }
      }

      auto result = std::make_shared<rule_set_t>();
      result->revision = remote.revision;
      result->expires_at = remote.expires_at;
      for (auto &[_, rule] : merged) {
        result->rules.push_back(std::move(rule));
      }
      return result;
    }

    bool
    write_cache(const std::filesystem::path &path, std::string_view envelope) {
      if (path.empty()) {
        return false;
      }

      std::error_code ec;
      if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
      }
      if (ec) {
        return false;
      }

      auto temporary = path;
      temporary += ".tmp";
      auto backup = path;
      backup += ".bak";
      {
        std::ofstream output {temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
          return false;
        }
        output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
        output.flush();
        if (!output) {
          return false;
        }
      }

      const bool had_previous = std::filesystem::exists(path, ec) && !ec;
      if (had_previous) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) {
          std::filesystem::remove(temporary, ec);
          return false;
        }
      }

      ec.clear();
      std::filesystem::rename(temporary, path, ec);
      if (ec) {
        if (had_previous) {
          std::error_code restore_ec;
          std::filesystem::rename(backup, path, restore_ec);
        }
        std::filesystem::remove(temporary, ec);
        return false;
      }
      if (had_previous) {
        std::filesystem::remove(backup, ec);
      }
      return true;
    }

    std::optional<std::string>
    read_bounded_file(const std::filesystem::path &path, std::size_t max_bytes) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(path, ec);
      if (ec || size == 0 || size > max_bytes) {
        return std::nullopt;
      }
      std::ifstream input {path, std::ios::binary};
      if (!input) {
        return std::nullopt;
      }
      std::string content {
        std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {},
      };
      if (content.empty() || content.size() != size || content.size() > max_bytes) {
        return std::nullopt;
      }
      return content;
    }

    std::optional<std::string>
    signing_certificate(const options_t &options) {
      if (options.signing_certificate.empty()) {
        return std::string {built_in_signing_certificate};
      }
      return read_bounded_file(options.signing_certificate, 64 * 1024);
    }

    void
    expire_active_rules_if_needed() {
      std::lock_guard lock {active_mutex};
      if (!active_state.rules || active_state.rules->expires_at == 0) {
        return;
      }
      const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      if (now < active_state.rules->expires_at) {
        return;
      }

      active_state.rules = built_in_rules();
      active_state.status.revision = 0;
      active_state.status.source = "built-in";
      active_state.status.last_error = "active rule feed expired; using built-in rules";
    }

    void
    record_error(std::string message) {
      {
        std::lock_guard lock {active_mutex};
        active_state.status.last_error = message;
      }
      BOOST_LOG(warning) << "Client fingerprint rules: " << message;
    }

    bool
    activate(
      const std::shared_ptr<const rule_set_t> &remote,
      std::string source,
      bool reject_rollback
    ) {
      std::lock_guard lock {active_mutex};
      if (reject_rollback && remote->revision <= active_state.remote_revision) {
        return false;
      }
      active_state.rules = merge_with_built_in(*remote);
      active_state.remote_revision = remote->revision;
      active_state.status.revision = remote->revision;
      active_state.status.source = std::move(source);
      active_state.status.last_error.clear();
      return true;
    }
  }  // namespace

  parse_result_t
  parse_signed_rules(
    std::string_view envelope,
    std::string_view certificate_pem,
    std::chrono::system_clock::time_point now
  ) {
    if (envelope.empty() || envelope.size() > max_envelope_bytes) {
      return {nullptr, "signed rule envelope exceeds the size limit"};
    }

    json outer;
    try {
      outer = json::parse(envelope);
    }
    catch (const json::exception &e) {
      return {nullptr, std::string {"invalid signed rule envelope: "} + e.what()};
    }
    if (!outer.is_object() ||
        !outer.contains("payload") || !outer["payload"].is_string() ||
        !outer.contains("signature") || !outer["signature"].is_string()) {
      return {nullptr, "signed rule envelope requires payload and signature strings"};
    }

    const auto payload = decode_base64(outer["payload"].get_ref<const std::string &>(), max_payload_bytes);
    const auto signature = decode_base64(outer["signature"].get_ref<const std::string &>(), max_signature_bytes);
    if (!payload || !signature || signature->empty()) {
      return {nullptr, "signed rule envelope contains invalid base64"};
    }

    auto certificate = crypto::x509(certificate_pem);
    if (!certificate) {
      return {nullptr, "rule signing certificate is invalid"};
    }
    if (!crypto::verify256(certificate, *payload, *signature)) {
      return {nullptr, "rule signature verification failed"};
    }
    return parse_payload(*payload, now);
  }

  match_result_t
  match(const arguments_t &args) {
    ensure_built_in_state();
    expire_active_rules_if_needed();

    std::shared_ptr<const rule_set_t> rules;
    status_t current_status;
    {
      std::lock_guard lock {active_mutex};
      rules = active_state.rules;
      current_status = active_state.status;
    }

    for (const auto &rule : rules->rules) {
      if (!rule.enabled) {
        continue;
      }
      const bool matches = std::all_of(
        rule.predicates.begin(),
        rule.predicates.end(),
        [&args](const predicate_t &predicate) {
          return predicate_matches(predicate, args);
        }
      );
      if (matches) {
        return {
          .suspicious = true,
          .rule_id = rule.id,
          .message_key = rule.message_key,
          .revision = current_status.revision,
          .source = current_status.source,
        };
      }
    }
    return {};
  }

  status_t
  status() {
    ensure_built_in_state();
    expire_active_rules_if_needed();
    std::lock_guard lock {active_mutex};
    return active_state.status;
  }

  struct deinit_t::impl_t {
    explicit impl_t(options_t options):
        options_(std::move(options)) {
      std::lock_guard install_lock {install_mutex};
      {
        std::lock_guard lock {active_mutex};
        active_state = {
          .rules = built_in_rules(),
          .status = {
            .revision = 0,
            .source = "built-in",
            .last_error = {},
            .remote_rules_enabled = options_.remote_rules_enabled,
          },
          .remote_revision = 0,
          .options = options_,
          .initialized = true,
        };
      }

      if (!options_.remote_rules_enabled) {
        BOOST_LOG(info) << "Client fingerprint remote rules are disabled; using built-in rules";
        return;
      }

      load_cache();
    }

    ~impl_t() {
      std::lock_guard install_lock {install_mutex};
      std::lock_guard lock {active_mutex};
      active_state = {
        .rules = built_in_rules(),
      };
    }

  private:
    std::optional<std::string>
    certificate() const {
      return signing_certificate(options_);
    }

    void
    load_cache() {
      if (options_.cache_file.empty()) {
        return;
      }
      const auto envelope = read_bounded_file(options_.cache_file, max_envelope_bytes);
      if (!envelope) {
        return;
      }
      const auto certificate_pem = certificate();
      if (!certificate_pem) {
        record_error("cached rules exist but the signing certificate could not be read");
        return;
      }
      const auto parsed = parse_signed_rules(*envelope, *certificate_pem);
      if (!parsed) {
        record_error("cached rules rejected: " + parsed.error);
        return;
      }
      activate(parsed.rules, "cache", false);
      BOOST_LOG(info) << "Loaded client fingerprint rule cache revision " << parsed.rules->revision;
    }

    options_t options_;
  };

  deinit_t::deinit_t(std::unique_ptr<impl_t> impl):
      impl_(std::move(impl)) {
  }

  deinit_t::~deinit_t() = default;

  install_result_t
  install_signed_rules(std::string_view envelope) {
    std::lock_guard install_lock {install_mutex};

    options_t options;
    std::uint64_t current_revision = 0;
    {
      std::lock_guard lock {active_mutex};
      if (!active_state.initialized) {
        return {.error = "client fingerprint rules are not initialized"};
      }
      if (!active_state.options.remote_rules_enabled) {
        return {.error = "remote client fingerprint rules are disabled"};
      }
      options = active_state.options;
      current_revision = active_state.remote_revision;
    }

    const auto certificate_pem = signing_certificate(options);
    if (!certificate_pem) {
      const std::string error = "could not read the rule signing certificate";
      record_error(error);
      return {.error = error};
    }
    const auto parsed = parse_signed_rules(envelope, *certificate_pem);
    if (!parsed) {
      const auto error = "candidate rules rejected: " + parsed.error;
      record_error(error);
      return {.error = error};
    }
    if (parsed.rules->revision < current_revision) {
      const std::string error = "candidate rule revision would roll back the active rules";
      record_error(error);
      return {.error = error};
    }
    if (parsed.rules->revision == current_revision) {
      return {
        .installed = false,
        .unchanged = true,
        .revision = current_revision,
      };
    }
    if (!write_cache(options.cache_file, envelope)) {
      const std::string error = "verified candidate rules could not be persisted";
      record_error(error);
      return {.error = error};
    }
    if (!activate(parsed.rules, "remote", true)) {
      return {
        .installed = false,
        .unchanged = true,
        .revision = status().revision,
      };
    }

    BOOST_LOG(info) << "Activated client fingerprint rule revision " << parsed.rules->revision;
    return {
      .installed = true,
      .unchanged = false,
      .revision = parsed.rules->revision,
    };
  }

  std::unique_ptr<deinit_t>
  init(options_t options) noexcept {
    try {
      return std::unique_ptr<deinit_t> {
        new deinit_t {std::make_unique<deinit_t::impl_t>(std::move(options))}
      };
    }
    catch (const std::exception &e) {
      record_error(std::string {"rule initialization failed: "} + e.what());
    }
    catch (...) {
      record_error("rule initialization failed");
    }
    return nullptr;
  }
}  // namespace client_fingerprint
