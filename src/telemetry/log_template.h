#pragma once

#include <string>
#include <string_view>

namespace telemetry {
  // Lookup of the static operands of a host log statement, with placeholders for values.
  std::string_view
  log_template_at(std::string_view file, int line);

  std::string
  log_component_of(std::string_view message);

  // The wire site is a source basename and line, or the fixed fallback for records with no site.
  inline constexpr std::string_view UNKNOWN_LOG_SITE = "host-log";

  inline bool
  valid_log_site(std::string_view site) {
    if (site == UNKNOWN_LOG_SITE) return true;
    const auto colon = site.find(':');
    if (colon == std::string_view::npos || colon < 3 || colon > 85) return false;
    const auto file = site.substr(0, colon);
    const auto dot = file.rfind('.');
    if (dot == 0 || dot == std::string_view::npos) return false;
    const auto extension = file.substr(dot);
    if (extension != ".cpp" && extension != ".cc" && extension != ".c" && extension != ".h" && extension != ".hpp" && extension != ".mm" && extension != ".m") return false;
    if (file.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") != std::string_view::npos) return false;
    const auto line = site.substr(colon + 1);
    return !line.empty() && line.size() <= 6 && line.front() != '0' && line.find_first_not_of("0123456789") == std::string_view::npos;
  }

  inline std::string
  log_site_basename(std::string_view location) {
    const auto slash = location.find_last_of("/\\");
    auto site = location.substr(slash == std::string_view::npos ? 0 : slash + 1);
    const auto colon = site.find(':');
    if (colon != std::string_view::npos) site = site.substr(0, site.find(':', colon + 1));
    return std::string { valid_log_site(site) ? site : UNKNOWN_LOG_SITE };
  }
}  // namespace telemetry
