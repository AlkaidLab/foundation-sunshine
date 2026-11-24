/**
 * @file src/telemetry/device_facts.h
 * @brief Coarse, anonymous facts about the computer, reported when they change: GPU vendor and driver, OS build, number of screens,
 *        the highest refresh rate, and the link type and speed class of the network adapter in use.
 *
 * Pure code (std + nlohmann::json), so it is unit tested on its own; the probes that read the system live in telemetry.cpp.
 * Nothing here names a device: every value is a vendor word, a version number, a count or a class (docs/privacy_telemetry.md).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace telemetry::device_facts {

  /** A fact that changes is reported again only after this long since the last report: a refresh rate that flips with a display mode must not become a stream of events. */
  inline constexpr std::int64_t MIN_RESEND_MS = 24LL * 3600 * 1000;

  struct facts_t {
    std::string gpu_vendor;  ///< nvidia | amd | intel | apple | other, empty when unknown
    std::string gpu_driver;  ///< digits and dots of the driver version
    std::string os_build;  ///< digits of the OS build number
    int displays = -1;  ///< -1: unknown
    int refresh_hz = -1;  ///< the highest refresh rate of the screens, snapped to a standard rate; -1: unknown
    std::string nic_link = "unknown";  ///< wired | wireless | other | unknown
    std::string nic_speed = "unknown";  ///< lt_1g | 1g | 2_5g | 5g | 10g | gt_10g | unknown
  };

  /** What the inventory knows about an adapter (the host fills it from system_info). */
  struct nic_t {
    std::string type;  ///< ethernet | wifi | mesh | virtual_interface | other
    bool up = false;
    std::optional<int> link_speed_mbps;
    bool is_default_route = false;
  };

  /** lt_1g below 900 Mbit/s, 1g up to 1.5, 2_5g up to 3.5, 5g up to 7.5, 10g up to 15, gt_10g above; unknown without a speed. */
  std::string nic_speed_class(std::optional<int> link_speed_mbps);

  /** The adapter the host reaches the network with: the up adapter on the default route, else the fastest up one that is not virtual. nullopt when there is none. */
  std::optional<nic_t> pick_nic(const std::vector<nic_t> &nics);

  /** wired for ethernet, wireless for wifi and mesh, other for the rest of the real adapters. */
  std::string nic_link_class(std::string_view type);

  /** A refresh rate snapped to the nearest standard rate within 1.5 % (59.94 -> 60, 143.98 -> 144), else rounded; 0 for a value that is no rate. */
  int standard_refresh_hz(double hz);

  /** Only the digits of an OS build number ("26100" from "10.0.26100"), at most 16. */
  std::string os_build_token(std::string_view text);

  /** The props of the `device_facts` event: only the facts that are known. */
  nlohmann::json to_props(const facts_t &facts);

  /** A stable string of the facts that decides whether they changed. */
  std::string fingerprint(const facts_t &facts);

  struct state_t {
    std::string fingerprint;
    std::int64_t sent_ms = 0;
  };
  /** Never reported, or changed and the last report is older than MIN_RESEND_MS. Facts that are all unknown are never reported. */
  bool should_send(const std::optional<state_t> &last, const facts_t &facts, std::int64_t now_ms);

  nlohmann::json state_to_json(const state_t &state);
  std::optional<state_t> state_from_json(const nlohmann::json &json);

}  // namespace telemetry::device_facts
