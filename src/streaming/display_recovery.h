#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace streaming {
  // ms-query-display-config-20261121, flags: topology can change.
  // ms-dxgi-duplicate-output-20261121, Remarks: duplication belongs to an output.
  inline std::optional<std::string>
  select_recovery_display(const std::vector<std::string> &displays, const std::string &resolved,
    const std::string &fallback, bool require_target) {
    if (!resolved.empty() && std::find(displays.begin(), displays.end(), resolved) != displays.end()) {
      return resolved;
    }
    return require_target ? std::nullopt : std::optional<std::string> { fallback };
  }

  template <class Apply>
  bool
  commit_display_resolution(bool allowed, bool applied, Apply &&commit) {
    if (!allowed || !applied) {
      return false;
    }
    commit();
    return true;
  }
}  // namespace streaming
