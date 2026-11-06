#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace display_device::vdd_edid {
  /** @brief Treat missing, empty and null mode-list values as omitted; an explicit [] replaces the list. */
  inline std::string_view
  mode_list_or(const std::map<std::string, std::string> &values, const char *key, std::string_view fallback = "[]") {
    const auto found = values.find(key);
    return found == values.end() || found->second.empty() || found->second == "null" ? fallback : std::string_view { found->second };
  }

  /**
   * @brief Fill omitted saved mode lists from the currently advertised runtime lists.
   * @details Nonempty/non-null values, including explicit empty lists, retain their values.
   *          API validation and locked persistence use the same fallback rules.
   */
  inline std::map<std::string, std::string>
  with_mode_list_defaults(std::map<std::string, std::string> saved,
    const std::vector<std::string> &runtime_resolutions, const std::vector<std::string> &runtime_fps) {
    const auto fill = [&](const char *key, const auto &items) {
      if (!mode_list_or(saved, key, "").empty()) return;
      std::string list = "[";
      for (const auto &item : items) {
        if (list.size() > 1) list += ',';
        list += item;
      }
      saved.insert_or_assign(key, list + ']');
    };
    fill("resolutions", runtime_resolutions);
    fill("fps", runtime_fps);
    return saved;
  }

  struct resolution_t {
    unsigned int width;
    unsigned int height;
  };

  /** @brief Parse a configured size with dimensions bounded to the EDID DTD fields. */
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

  /** @brief Accept Default or a canonical lowercase-x size suitable for a command. */
  inline bool
  valid_value(std::string_view value) {
    return value == "default" || (value.find('X') == value.npos && parse_size(value).has_value());
  }

  /** @brief Visit nonempty tokens in the existing bracketed/comma-separated list format. */
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

  /** @brief Match the driver's preset/four-pixel timings and 16-bit 10-kHz clock bound. */
  inline bool
  timing_encodable(const resolution_t &size, unsigned int refresh) {
    // Match ZakoVDD: preserve the verified 1080p60/4K60 timings; other
    // virtual modes use dispinfo's four-pixel blanking and a 10 kHz clock.
    const std::uint64_t clock = refresh == 60 && size.width == 1920 && size.height == 1080 ? 14850 :
                                refresh == 60 && size.width == 3840 && size.height == 2160 ? 59400 :
                                                                                             (static_cast<std::uint64_t>(size.width + 4) * (size.height + 4) * refresh + 5000) / 10000;
    return refresh > 0 && clock > 0 && clock <= 65535;
  }

  /** @brief Require list membership and an encodable rate after SETMODES FPS rounding. */
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
    const auto value = [&](const char *key) {
      return mode_list_or(incoming, key, mode_list_or(existing, key));
    };
    bool changed = false;
    for (const auto key : { "resolutions", "fps" }) {
      if (value(key) != mode_list_or(existing, key)) changed = true;
    }
    if (!changed) return true;
    return supported_selection(preference->second,
      value("resolutions"), value("fps"));
  }
}  // namespace display_device::vdd_edid
