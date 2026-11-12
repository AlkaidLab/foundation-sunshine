#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace display_device::vdd_modes {
  /** @brief Treat missing, empty and null mode-list values as omitted; an explicit [] replaces the list. */
  inline std::string_view
  mode_list_or(const std::map<std::string, std::string> &values, const char *key, std::string_view fallback = "[]") {
    const auto found = values.find(key);
    return found == values.end() || found->second.empty() || found->second == "null" ? fallback : std::string_view { found->second };
  }

  /**
   * @brief Fill omitted saved mode lists from the currently advertised runtime lists.
   * @details Nonempty/non-null values, including explicit empty lists, retain their values.
   *          Locked persistence uses the same fallback for host and legacy XML lists.
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

}  // namespace display_device::vdd_modes
