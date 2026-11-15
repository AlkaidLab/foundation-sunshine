/**
 * @file src/telemetry/device_facts.cpp
 */
#include "device_facts.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace telemetry::device_facts {

  namespace {
    constexpr std::array<int, 21> STANDARD_RATES { 24, 25, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 170, 180, 200, 240, 280, 360, 480, 500 };
  }

  std::string
  nic_speed_class(std::optional<int> link_speed_mbps) {
    if (!link_speed_mbps || *link_speed_mbps <= 0) return "unknown";
    const int mbps = *link_speed_mbps;
    if (mbps < 900) return "lt_1g";
    if (mbps <= 1500) return "1g";
    if (mbps <= 3500) return "2_5g";
    if (mbps <= 7500) return "5g";
    if (mbps <= 15000) return "10g";
    return "gt_10g";
  }

  std::string
  nic_link_class(std::string_view type) {
    if (type == "ethernet") return "wired";
    if (type == "wifi" || type == "mesh") return "wireless";
    if (type.empty()) return "unknown";
    return "other";
  }

  std::optional<nic_t>
  pick_nic(const std::vector<nic_t> &nics) {
    const nic_t *best = nullptr;
    for (const auto &n : nics) {
      if (!n.up || n.type == "virtual_interface") continue;
      if (n.is_default_route) return n;
      if (!best || n.link_speed_mbps.value_or(0) > best->link_speed_mbps.value_or(0)) best = &n;
    }
    if (best) return *best;
    return std::nullopt;
  }

  int
  standard_refresh_hz(double hz) {
    if (!std::isfinite(hz) || hz < 1.0 || hz > 1000.0) return 0;
    for (const int rate : STANDARD_RATES)
      if (std::abs(hz - rate) <= rate * 0.015) return rate;
    return static_cast<int>(std::lround(hz));
  }

  std::string
  os_build_token(std::string_view text) {
    // the last run of digits of "10.0.26100" or "26100.4349": a Windows build number has five digits, a version has fewer; take the longest run
    std::string best, current;
    for (const char c : text) {
      if (c >= '0' && c <= '9') current.push_back(c);
      else {
        if (current.size() > best.size()) best = current;
        current.clear();
      }
    }
    if (current.size() > best.size()) best = current;
    if (best.size() > 16) best.resize(16);
    return best;
  }

  nlohmann::json
  to_props(const facts_t &f) {
    nlohmann::json p = nlohmann::json::object();
    if (!f.gpu_vendor.empty()) p["gpu_vendor"] = f.gpu_vendor;
    if (!f.gpu_driver.empty()) p["gpu_driver"] = f.gpu_driver;
    if (!f.os_build.empty()) p["os_build"] = f.os_build;
    if (f.displays >= 0) p["displays"] = std::min(f.displays, 16);
    if (f.refresh_hz > 0) p["refresh_hz"] = std::min(f.refresh_hz, 1000);
    p["nic_link"] = f.nic_link;
    p["nic_speed"] = f.nic_speed;
    return p;
  }

  std::string
  fingerprint(const facts_t &f) {
    return f.gpu_vendor + "|" + f.gpu_driver + "|" + f.os_build + "|" + std::to_string(f.displays) + "|" + std::to_string(f.refresh_hz) + "|" + f.nic_link + "|" + f.nic_speed;
  }

  bool
  should_send(const std::optional<state_t> &last, const facts_t &facts, std::int64_t now_ms) {
    const bool anything = !facts.gpu_vendor.empty() || !facts.gpu_driver.empty() || !facts.os_build.empty() || facts.displays >= 0 || facts.refresh_hz > 0 || facts.nic_speed != "unknown" || facts.nic_link != "unknown";
    if (!anything) return false;
    if (!last) return true;
    if (last->fingerprint == fingerprint(facts)) return false;
    return now_ms - last->sent_ms >= MIN_RESEND_MS;
  }

  nlohmann::json
  state_to_json(const state_t &s) {
    return { { "v", 1 }, { "fp", s.fingerprint }, { "sent_ms", s.sent_ms } };
  }

  std::optional<state_t>
  state_from_json(const nlohmann::json &j) {
    if (!j.is_object() || j.value("v", 0) != 1 || !j.contains("fp") || !j["fp"].is_string() || !j.contains("sent_ms") || !j["sent_ms"].is_number_integer()) return std::nullopt;
    state_t s;
    s.fingerprint = j["fp"].get<std::string>();
    s.sent_ms = j["sent_ms"].get<std::int64_t>();
    if (s.fingerprint.size() > 256) return std::nullopt;
    return s;
  }

}  // namespace telemetry::device_facts
