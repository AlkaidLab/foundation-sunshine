#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace display_device::vdd_edid {
  struct resolution_t {
    unsigned int width;
    unsigned int height;
  };

  inline std::optional<resolution_t>
  parse_size(std::string_view value) {
    const auto separator = value.find_first_of("xX");
    if (separator == value.npos || separator == 0 || separator + 1 == value.size()) return {};
    auto dimension = [](std::string_view digits, unsigned int &result) {
      result = 0;
      for (char digit : digits) {
        if (digit < '0' || digit > '9' || result > 409) return false;
        result = result * 10 + (digit - '0');
        if (result > 4095) return false;
      }
      return result > 0;
    };
    resolution_t size {};
    if (!dimension(value.substr(0, separator), size.width) ||
        !dimension(value.substr(separator + 1), size.height)) return {};
    return size;
  }

  inline bool
  valid_value(std::string_view value) {
    return value == "default" || (value.find('X') == value.npos && parse_size(value).has_value());
  }

  template <class Visitor>
  void
  visit_list(std::string_view list, Visitor visitor) {
    while (!list.empty()) {
      const auto comma = list.find(',');
      auto item = list.substr(0, comma);
      const auto first = item.find_first_not_of(" \t\r\n[");
      const auto last = item.find_last_not_of(" \t\r\n]");
      if (first != item.npos && last != item.npos && first <= last) visitor(item.substr(first, last - first + 1));
      if (comma == list.npos) break;
      list.remove_prefix(comma + 1);
    }
  }

  inline bool
  timing_encodable(const resolution_t &size, unsigned int refresh) {
    // Match ZakoVDD: preserve the verified 1080p60/4K60 timings; other
    // virtual modes use dispinfo's four-pixel blanking and a 10 kHz clock.
    const std::uint64_t clock = refresh == 60 && size.width == 1920 && size.height == 1080 ? 14850 :
                                refresh == 60 && size.width == 3840 && size.height == 2160 ? 59400 :
                                                                                             (static_cast<std::uint64_t>(size.width + 4) * (size.height + 4) * refresh + 5000) / 10000;
    return refresh > 0 && clock > 0 && clock <= 65535;
  }

  inline bool
  supported_selection(std::string_view value, std::string_view resolutions, std::string_view fps) {
    if (value == "default") return true;
    if (!valid_value(value)) return false;
    const auto selected = *parse_size(value);
    bool configured = false, encodable = false;
    visit_list(resolutions, [&](auto item) {
      if (const auto size = parse_size(item); size && size->width == selected.width && size->height == selected.height) configured = true;
    });
    if (!configured) return false;
    visit_list(fps, [&](auto item) {
      try {
        std::size_t consumed = 0;
        const auto rate = std::stod(std::string(item), &consumed);
        if (consumed == item.size() && std::isfinite(rate) && rate >= 1 && rate <= 480 &&
            timing_encodable(selected, static_cast<unsigned int>(std::lround(rate)))) encodable = true;
      }
      catch (...) { /* Invalid FPS entries do not make a mode available. */
      }
    });
    return encodable;
  }
  /** Check a retained preference before a legacy client changes either mode list. */
  inline bool
  retained_selection_supported(const std::map<std::string, std::string> &incoming,
    const std::map<std::string, std::string> &existing) {
    if (incoming.count("vdd_edid_native_resolution")) return true;
    const auto preference = existing.find("vdd_edid_native_resolution");
    if (preference == existing.end() || preference->second == "default") return true;
    auto value = [](const auto &values, const char *key) -> std::string_view {
      const auto found = values.find(key);
      return found == values.end() ? "[]" : std::string_view { found->second };
    };
    bool changed = false;
    for (const auto key : { "resolutions", "fps" }) {
      if (incoming.count(key) && value(incoming, key) != value(existing, key)) changed = true;
    }
    if (!changed) return true;
    return supported_selection(preference->second,
      value(incoming.count("resolutions") ? incoming : existing, "resolutions"),
      value(incoming.count("fps") ? incoming : existing, "fps"));
  }
}  // namespace display_device::vdd_edid
