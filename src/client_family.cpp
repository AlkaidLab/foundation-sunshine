/**
 * @file src/client_family.cpp
 * @brief Ordered, evidence-only client family rules.
 */

#include "client_family.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>

namespace client_family {
  namespace {
    constexpr std::size_t max_fields = 128;
    constexpr std::size_t max_key_bytes = 128;
    constexpr std::size_t max_value_bytes = 512;
    constexpr std::size_t max_version_bytes = 64;

    enum class op_e : std::uint8_t {
      present,
      equals,
      not_equal,
      prefix,
      contains_ci,
      product_token,
      absent_prefix,
    };

    struct predicate_t {
      std::string_view key;
      op_e operation;
      std::string_view value;
    };

    struct rule_t {
      std::string_view id;
      family_e family;
      std::uint8_t rank;
      std::string_view default_os;
      std::span<const std::string_view> version_tokens;
      std::span<const predicate_t> predicates;
    };

    bool
    is_ascii_alpha(char value) noexcept {
      return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    }

    bool
    is_ascii_digit(char value) noexcept {
      return value >= '0' && value <= '9';
    }

    char
    ascii_lower(char value) noexcept {
      return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }

    std::string
    lowercase(std::string_view value) {
      std::string out(value);
      std::transform(out.begin(), out.end(), out.begin(), ascii_lower);
      return out;
    }

    bool
    is_token_char(char value) noexcept {
      return is_ascii_alpha(value) || is_ascii_digit(value) || value == '.' || value == '_' || value == '+' || value == '-';
    }

    bool
    contains_ci(std::string_view haystack, std::string_view needle) noexcept {
      if (needle.empty()) {
        return true;
      }
      if (needle.size() > haystack.size()) {
        return false;
      }
      for (std::size_t offset = 0; offset + needle.size() <= haystack.size(); ++offset) {
        bool equal = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
          if (ascii_lower(haystack[offset + index]) != ascii_lower(needle[index])) {
            equal = false;
            break;
          }
        }
        if (equal) {
          return true;
        }
      }
      return false;
    }

    bool
    contains_product_token(std::string_view haystack, std::string_view needle) noexcept {
      if (needle.empty() || needle.size() > haystack.size()) {
        return false;
      }
      for (std::size_t offset = 0; offset + needle.size() <= haystack.size(); ++offset) {
        if (offset > 0 && is_token_char(haystack[offset - 1])) {
          continue;
        }
        bool equal = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
          if (ascii_lower(haystack[offset + index]) != ascii_lower(needle[index])) {
            equal = false;
            break;
          }
        }
        if (equal) {
          const std::size_t after = offset + needle.size();
          if (after == haystack.size() || !is_token_char(haystack[after])) {
            return true;
          }
        }
      }
      return false;
    }

    bool
    predicate_matches(const predicate_t &predicate, const observation_t &observation) {
      if (predicate.operation == op_e::absent_prefix) {
        return std::none_of(
          observation.fields.begin(),
          observation.fields.end(),
          [&](const auto &entry) { return entry.first.starts_with(predicate.key); });
      }

      const auto entry = observation.fields.find(predicate.key);
      const bool present = entry != observation.fields.end() && !entry->second.empty();
      switch (predicate.operation) {
        case op_e::present:
          return present;
        case op_e::equals:
          return present && entry->second == predicate.value;
        case op_e::not_equal:
          return present && entry->second != predicate.value;
        case op_e::prefix:
          return present && std::string_view { entry->second }.starts_with(predicate.value);
        case op_e::contains_ci:
          return present && contains_ci(entry->second, predicate.value);
        case op_e::product_token:
          return present && contains_product_token(entry->second, predicate.value);
        case op_e::absent_prefix:
          break;
      }
      return false;
    }

    std::optional<std::string>
    version_from_token(std::string_view user_agent, std::string_view token) {
      const auto haystack = lowercase(user_agent);
      const auto needle = lowercase(token);
      if (needle.empty()) {
        return std::nullopt;
      }
      std::size_t position = 0;
      while (position < haystack.size()) {
        position = haystack.find(needle, position);
        if (position == std::string::npos) {
          return std::nullopt;
        }

        const bool left_boundary = position == 0 || !is_token_char(haystack[position - 1]);
        const std::size_t after = position + needle.size();
        if (left_boundary && after < haystack.size() && haystack[after] == '/') {
          const std::size_t begin = after + 1;
          std::size_t end = begin;
          while (end < user_agent.size() && is_token_char(user_agent[end])) {
            ++end;
          }
          if (end > begin && end - begin <= max_version_bytes) {
            std::string version(user_agent.substr(begin, end - begin));
            if (std::any_of(version.begin(), version.end(), is_ascii_digit)) {
              return version;
            }
          }
        }
        position = after;
      }
      return std::nullopt;
    }

    std::string
    os_from_user_agent(std::string_view user_agent) {
      const auto value = lowercase(user_agent);
      if (contains_ci(value, "android")) {
        return "android";
      }
      if (contains_ci(value, "iphone") || contains_ci(value, "ipad") || contains_ci(value, "ipados")) {
        return "ios";
      }
      if (contains_ci(value, "tvos") || contains_ci(value, "appletv")) {
        return "tvos";
      }
      if (contains_ci(value, "macintosh") || contains_ci(value, "mac os x") || contains_ci(value, "darwin") || contains_ci(value, "macos")) {
        return "macos";
      }
      if (contains_ci(value, "windows")) {
        return "windows";
      }
      if (contains_ci(value, "linux")) {
        return "linux";
      }
      return {};
    }

    std::string
    rule_source(const rule_t &rule) {
      std::string source(rule.id);
      source += ':';
      bool first = true;
      for (const auto &predicate : rule.predicates) {
        if (!first) {
          source += ',';
        }
        if (predicate.operation == op_e::absent_prefix) {
          source += "no ";
        }
        source += predicate.key;
        first = false;
      }
      if (!rule.version_tokens.empty()) {
        if (!first) {
          source += ',';
        }
        source += "header.user-agent(version)";
      }
      return source;
    }

    // Product tokens follow the HTTP product grammar (RFC 9110 section 10.1.5).
    // They are documented examples, not a guarantee that every build sends one.
    constexpr std::string_view voidlink_tokens[] = { "VoidLink" };
    constexpr std::string_view enhanced_tokens[] = { "MoonlightEnhanced" };
    constexpr std::string_view vplus_tokens[] = { "MoonlightVPlus", "VPlus" };

    constexpr predicate_t voidlink_pen_predicates[] = {
      { "input.penBarrelRoll", op_e::equals, "1" },
    };
    constexpr predicate_t voidlink_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "VoidLink" },
    };
    constexpr predicate_t enhanced_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "MoonlightEnhanced" },
    };
    constexpr predicate_t vplus_android_long_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "MoonlightVPlus" },
      { "header.user-agent", op_e::contains_ci, "android" },
    };
    constexpr predicate_t vplus_android_short_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "VPlus" },
      { "header.user-agent", op_e::contains_ci, "android" },
    };
    constexpr predicate_t vplus_desktop_long_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "MoonlightVPlus" },
    };
    constexpr predicate_t vplus_desktop_short_ua_predicates[] = {
      { "header.user-agent", op_e::product_token, "VPlus" },
    };
    constexpr predicate_t vplus_android_pyrowave_predicates[] = {
      { "sdp.x-ml-pyrowave.protocolVersion", op_e::present, {} },
    };
    constexpr predicate_t upstream_standard_predicates[] = {
      { "sdp.x-nv-general.featureFlags", op_e::present, {} },
      { "sdp.x-ml-", op_e::absent_prefix, {} },
    };

    // Ordered from the most specific evidence to the weakest signature.
    // Sources (docs/authority/source-index.tsv):
    //   client-family-rfc9110-user-agent-20261103 section 10.1.5
    //   client-family-rfc8866-sdp-20261103 section 8.2.4
    //   client-family-rfc2326-rtsp-20261103 section 10.3
    //   client-family-rfc7826-rtsp2-20261103 Appendix I.1
    // PyroWave is documented as the Moonlight V+ Android experimental contract.
    // The pen barrel-roll packet is the voidlink-c extension, not a generic
    // GameStream message. The standard signature is a vanilla x-nv only SDP.
    constexpr rule_t rules[] = {
      { "voidlink-pen-barrel-roll", family_e::voidlink, 100, "ios", {}, voidlink_pen_predicates },
      { "voidlink-user-agent", family_e::voidlink, 95, "", voidlink_tokens, voidlink_ua_predicates },
      { "moonlight-macos-enhanced-user-agent", family_e::moonlight_macos_enhanced, 95, "macos", enhanced_tokens, enhanced_ua_predicates },
      { "moonlight-vplus-android-user-agent", family_e::moonlight_vplus_android, 95, "android", vplus_tokens, vplus_android_long_ua_predicates },
      { "moonlight-vplus-android-user-agent-short", family_e::moonlight_vplus_android, 95, "android", vplus_tokens, vplus_android_short_ua_predicates },
      { "moonlight-vplus-desktop-user-agent", family_e::moonlight_vplus_desktop, 95, "", vplus_tokens, vplus_desktop_long_ua_predicates },
      { "moonlight-vplus-desktop-user-agent-short", family_e::moonlight_vplus_desktop, 95, "", vplus_tokens, vplus_desktop_short_ua_predicates },
      { "moonlight-vplus-android-pyrowave", family_e::moonlight_vplus_android, 90, "android", {}, vplus_android_pyrowave_predicates },
      { "moonlight-upstream-standard", family_e::moonlight_upstream, 10, "", {}, upstream_standard_predicates },
    };
  }  // namespace

  std::string_view
  to_string(family_e family) noexcept {
    switch (family) {
      case family_e::unknown:
        return unknown_id;
      case family_e::moonlight_vplus_android:
        return vplus_android_id;
      case family_e::moonlight_vplus_desktop:
        return vplus_desktop_id;
      case family_e::moonlight_macos_enhanced:
        return macos_enhanced_id;
      case family_e::voidlink:
        return voidlink_id;
      case family_e::moonlight_upstream:
        return upstream_id;
    }
    return unknown_id;
  }

  family_e
  family_from_string(std::string_view id) noexcept {
    if (id == vplus_android_id) {
      return family_e::moonlight_vplus_android;
    }
    if (id == vplus_desktop_id) {
      return family_e::moonlight_vplus_desktop;
    }
    if (id == macos_enhanced_id) {
      return family_e::moonlight_macos_enhanced;
    }
    if (id == voidlink_id) {
      return family_e::voidlink;
    }
    if (id == upstream_id) {
      return family_e::moonlight_upstream;
    }
    return family_e::unknown;
  }

  std::optional<bool>
  official(family_e family) noexcept {
    switch (family) {
      case family_e::moonlight_vplus_android:
      case family_e::moonlight_vplus_desktop:
      case family_e::moonlight_macos_enhanced:
        return true;
      case family_e::voidlink:
      case family_e::moonlight_upstream:
        return false;
      case family_e::unknown:
        return std::nullopt;
    }
    return std::nullopt;
  }

  void
  observation_t::add(std::string_view key, std::string_view value) {
    if (fields.size() >= max_fields || key.empty() || key.size() > max_key_bytes || value.size() > max_value_bytes) {
      return;
    }

    std::string safe_key(key);
    for (const char character : safe_key) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 0x20 || byte > 0x7e) {
        return;
      }
    }

    std::string safe_value(value);
    for (char &character : safe_value) {
      if (character == '\r' || character == '\n' || character == '\0') {
        character = ' ';
      }
    }

    fields.try_emplace(std::move(safe_key), std::move(safe_value));
  }

  match_t
  identify(const observation_t &observation) {
    for (const auto &rule : rules) {
      const bool matched = std::all_of(
        rule.predicates.begin(),
        rule.predicates.end(),
        [&](const predicate_t &predicate) { return predicate_matches(predicate, observation); });
      if (!matched) {
        continue;
      }

      match_t result;
      result.family = rule.family;
      result.rank = rule.rank;
      result.source = rule_source(rule);

      const auto user_agent = observation.fields.find("header.user-agent");
      if (!rule.version_tokens.empty() && user_agent != observation.fields.end()) {
        for (const auto token : rule.version_tokens) {
          if (const auto version = version_from_token(user_agent->second, token)) {
            result.version = *version;
            break;
          }
        }
      }
      if (user_agent != observation.fields.end()) {
        result.os = os_from_user_agent(user_agent->second);
      }
      if (result.os.empty()) {
        result.os = std::string(rule.default_os);
      }
      return result;
    }
    return {};
  }

  record_t
  merge(const record_t &previous, const observation_t &observation) {
    record_t result = previous;
    const auto match = identify(observation);

    const bool accept_family =
      match.matched() && (result.family == family_e::unknown || match.rank >= result.rank);
    if (accept_family) {
      result.family = match.family;
      result.rank = match.rank;
      result.source = match.source;
    }
    if (!match.version.empty()) {
      result.version = match.version;
    }
    if (!match.os.empty() && (result.family == family_e::unknown || accept_family)) {
      result.os = match.os;
    }
    if (observation.now > 0) {
      result.last_seen = observation.now;
    }
    return result;
  }

  nlohmann::json
  record_json(const record_t &record) {
    nlohmann::json result {
      { "family", std::string(to_string(record.family)) },
      { "official", nullptr },
      { "version", nullptr },
      { "os", nullptr },
      { "last_seen", nullptr },
      { "family_source", nullptr },
    };

    if (const auto value = official(record.family)) {
      result["official"] = *value;
    }
    if (!record.version.empty()) {
      result["version"] = record.version;
    }
    if (!record.os.empty()) {
      result["os"] = record.os;
    }
    if (record.last_seen > 0) {
      result["last_seen"] = record.last_seen;
    }
    if (!record.source.empty()) {
      result["family_source"] = record.source;
    }
    return result;
  }

  std::int64_t
  unix_now() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count();
  }

  namespace {
    bool
    is_blank(std::string_view value) {
      return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
      });
    }
  }  // namespace

  std::string
  pairing_device_name(std::string_view clientname, std::string_view devicename) {
    if (!is_blank(clientname)) {
      return std::string(clientname);
    }
    if (!is_blank(devicename) && lowercase(devicename) != "roth") {
      return std::string(devicename);
    }
    return {};
  }

  std::string
  session_client_name(std::string_view requested, std::string_view paired_name) {
    if (!is_blank(requested) && lowercase(requested) != "unknown") {
      return std::string(requested);
    }
    if (!is_blank(paired_name)) {
      return std::string(paired_name);
    }
    return "unknown";
  }

  void
  add_query(observation_t &observation, std::string_view name, std::string_view value) {
    observation.add("query." + std::string(name), value);
  }

  void
  add_header(observation_t &observation, std::string_view name, std::string_view value) {
    observation.add("header." + lowercase(name), value);
  }

  void
  add_sdp(observation_t &observation, std::string_view name, std::string_view value) {
    observation.add("sdp." + std::string(name), value);
  }
}  // namespace client_family
