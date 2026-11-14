/**
 * @file src/client_family.h
 * @brief Conservative identification of a client product family from request evidence.
 *
 * This module only reads fields the GameStream/Moonlight exchange already
 * carries: request query arguments, HTTP header fields and RTSP/SDP
 * attributes. It adds no probe, cookie, timing measurement or network call,
 * and it never participates in authentication or authorization. A result is
 * a display label for the host user; paired certificates remain the only
 * client identity.
 *
 * Rules are ordered. The first rule whose predicates all match wins, and an
 * observation that does not match a family-specific rule stays "unknown".
 * Absence of evidence is never treated as a third-party verdict.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace client_family {
  inline constexpr std::string_view unknown_id = "unknown";
  inline constexpr std::string_view vplus_android_id = "moonlight-vplus-android";
  inline constexpr std::string_view vplus_desktop_id = "moonlight-vplus-desktop";
  inline constexpr std::string_view macos_enhanced_id = "moonlight-macos-enhanced";
  inline constexpr std::string_view voidlink_id = "voidlink";
  inline constexpr std::string_view upstream_id = "moonlight-upstream";

  enum class family_e : std::uint8_t {
    unknown,
    moonlight_vplus_android,
    moonlight_vplus_desktop,
    moonlight_macos_enhanced,
    voidlink,
    moonlight_upstream,
  };

  std::string_view
  to_string(family_e family) noexcept;
  family_e
  family_from_string(std::string_view id) noexcept;

  /**
   * @brief Official tri-state: true, false, or no answer when the family is unknown.
   */
  std::optional<bool>
  official(family_e family) noexcept;

  /**
   * @brief The stored observation for one paired client.
   *
   * source names the rule and the evidence keys that produced the family.
   * last_seen is Unix seconds and zero means never observed.
   */
  struct record_t {
    family_e family { family_e::unknown };
    std::string version;
    std::string os;
    std::string source;
    std::int64_t last_seen {};
    std::uint8_t rank {};
  };

  /**
   * @brief One request's evidence, keyed by source-qualified field name.
   *
   * Keys are query.<argument>, header.<field>, sdp.<attribute> or
   * input.<signal>. Values are capped so a hostile request cannot grow the
   * record without bound.
   */
  struct observation_t {
    std::map<std::string, std::string, std::less<>> fields;
    std::int64_t now {};

    void
    add(std::string_view key, std::string_view value);
  };

  struct match_t {
    family_e family { family_e::unknown };
    std::string version;
    std::string os;
    std::string source;
    std::uint8_t rank {};

    [[nodiscard]] bool
    matched() const noexcept {
      return family != family_e::unknown;
    }
  };

  /** Evaluate one observation against the ordered built-in rule table. */
  match_t
  identify(const observation_t &observation);

  /**
   * @brief Merge an observation into a previously stored record.
   *
   * A weaker observation never replaces a known family. A newer version or
   * OS value does replace the previous value, so a client upgrade is visible.
   */
  record_t
  merge(const record_t &previous, const observation_t &observation);

  /** JSON shape shared by the clients API and its tests. */
  nlohmann::json
  record_json(const record_t &record);

  std::int64_t
  unix_now() noexcept;

  /**
   * @brief The device name a pairing request carries, or an empty string when it names no device.
   *
   * The Foundation extension sends `clientname`; the standard Moonlight exchange sends `devicename`.
   * Moonlight's shared client code hard-codes `devicename=roth`, which names no device, so that value
   * is ignored. The caller supplies its own default for an empty result.
   */
  std::string
  pairing_device_name(std::string_view clientname, std::string_view devicename);

  /**
   * @brief The client name a streaming session carries.
   *
   * `/launch` and `/resume` carry no name in the standard exchange (the Foundation extension sends
   * `clientname`), so the host falls back to the name stored with the paired certificate that
   * authenticated the request. "unknown" remains the answer when neither is available.
   */
  std::string
  session_client_name(std::string_view requested, std::string_view paired_name);

  /** Source-qualified evidence helpers. */
  void
  add_query(observation_t &observation, std::string_view name, std::string_view value);
  void
  add_header(observation_t &observation, std::string_view name, std::string_view value);
  void
  add_sdp(observation_t &observation, std::string_view name, std::string_view value);
}  // namespace client_family
