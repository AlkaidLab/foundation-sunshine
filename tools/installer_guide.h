#pragma once

#include <string>
#include <string_view>

// Whether 开启串流旅程 hands the Panel `--start-guide`. Pure text rules, no Windows headers, so the unit tests load it.
//
// The host keeps the guide in its configuration (config\sunshine.conf): guide_finished (true once the person reached the end) and
// guide_closed (the version the guide was closed for with the X; empty when it was never closed). Either one means the person has
// dealt with the guide, and the installer must not start it over.
namespace installer_guide {

  struct host_guide {
    bool finished = false;  // guide_finished is true or 1
    bool closed = false;  // guide_closed holds a version
  };

  namespace detail {
    inline std::string_view
    trim(std::string_view v) {
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
      return v;
    }
  }  // namespace detail

  // What the configuration text says about the guide, read the way the host reads its own file (config::parse_config): a line is
  // `key = value` with a trailing # comment cut off, a line without a value is skipped, and the first non-empty value of a key wins.
  // A missing or unreadable file reads as "nothing recorded".
  inline host_guide
  read_host_guide(std::string_view conf) {
    host_guide out;
    bool seen_finished = false;
    bool seen_closed = false;
    size_t pos = 0;
    while (pos < conf.size()) {
      size_t end = conf.find('\n', pos);
      if (end == std::string_view::npos) end = conf.size();
      std::string_view line = conf.substr(pos, end - pos);
      pos = end + 1;
      if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
      const size_t eq = line.find('=');
      if (eq == std::string_view::npos) continue;
      const std::string_view key = detail::trim(line.substr(0, eq));
      const std::string_view value = detail::trim(line.substr(eq + 1));
      if (value.empty()) continue;
      if (key == "guide_finished" && !seen_finished) {
        seen_finished = true;
        out.finished = value == "true" || value == "1";
      }
      else if (key == "guide_closed" && !seen_closed) {
        seen_closed = true;
        out.closed = true;
      }
    }
    return out;
  }

  // `--start-guide` opens the whole first-run flow from its first step and clears what was saved, so it is for a first run only:
  //  - an upgrade or a maintenance run never passes it: the Panel's own entry decides, and shows "what is new" once per version;
  //  - a fresh install passes it until the guide is finished or closed once; after that a click only opens the Panel.
  inline bool
  start_guide(bool upgrade, bool maintenance, std::string_view conf) {
    if (upgrade || maintenance) return false;
    const host_guide state = read_host_guide(conf);
    return !state.finished && !state.closed;
  }

}  // namespace installer_guide
