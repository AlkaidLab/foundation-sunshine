/**
 * @file src/platform/linux/wlr_randr.cpp
 * @brief Probe, parse and drive wlr-randr for wlroots compositors.
 */
#include "wlr_randr.h"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <unistd.h>

#include "src/display_device/vdd_utils.h"
#include "src/logging.h"

namespace platf::wlr_randr {
  namespace {
    std::atomic<std::chrono::steady_clock::rep> probe_after { 0 };
    std::atomic<bool> unavailability_logged { false };
    /// -1 unknown, 0 the compositor has no output manager, 1 it has one.
    std::atomic<int> probe_verdict { -1 };

    std::chrono::steady_clock::time_point
    now() {
      return std::chrono::steady_clock::now();
    }

    bool
    starts_with(std::string_view text, std::string_view prefix) {
      return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    std::string_view
    trim(std::string_view text) {
      const auto first = text.find_first_not_of(" \t\r\n");
      if (first == std::string_view::npos) {
        return {};
      }
      const auto last = text.find_last_not_of(" \t\r\n");
      return text.substr(first, last - first + 1);
    }

    /// wlr-randr takes plain arguments, but output names come from the
    /// compositor and Sunshine runs its helpers through /bin/sh.
    std::string
    quote(const std::string &value) {
      std::string quoted { "'" };
      for (const char ch : value) {
        if (ch == '\'') {
          quoted += "'\\''";
        }
        else {
          quoted += ch;
        }
      }
      quoted += '\'';
      return quoted;
    }

    bool
    parse_uint(std::string_view text, unsigned int &out, std::size_t &consumed) {
      if (text.empty()) {
        return false;
      }
      const auto *first = text.data();
      const auto *last = text.data() + text.size();
      const auto result = std::from_chars(first, last, out);
      if (result.ec != std::errc {} || result.ptr == first) {
        return false;
      }
      consumed = static_cast<std::size_t>(result.ptr - first);
      return true;
    }

    /**
     * @brief Parse one physical-size line, e.g. "Physical size: 600x340 mm".
     */
    bool
    parse_physical_size(std::string_view text, head_t &head) {
      unsigned int width = 0;
      unsigned int height = 0;
      std::size_t consumed = 0;
      if (!parse_uint(text, width, consumed) || text.size() <= consumed || text[consumed] != 'x') {
        return false;
      }
      text.remove_prefix(consumed + 1);
      if (!parse_uint(text, height, consumed)) {
        return false;
      }
      head.phys_width = width;
      head.phys_height = height;
      return true;
    }

    /**
     * @brief Parse one mode line, e.g. "1920x1080 px, 59.940000 Hz (preferred, current)".
     * @details The refresh rate is optional, the "px" unit is optional in the
     *          argument parser but always printed, and the flag list holds at
     *          most "preferred" and "current".
     */
    bool
    parse_mode_line(std::string_view text, mode_t &mode) {
      unsigned int width = 0;
      unsigned int height = 0;
      std::size_t consumed = 0;
      if (!parse_uint(text, width, consumed) || text.size() <= consumed || text[consumed] != 'x') {
        return false;
      }
      text.remove_prefix(consumed + 1);
      if (!parse_uint(text, height, consumed)) {
        return false;
      }
      text.remove_prefix(consumed);

      // " px" is always present in the listing; accept its absence as well so a
      // future format tweak degrades to a mode list rather than to nothing.
      if (starts_with(text, " px")) {
        text.remove_prefix(3);
      }

      double refresh = 0.0;
      if (!text.empty() && (text.front() == ',' || text.front() == '@')) {
        text.remove_prefix(1);
        while (!text.empty() && text.front() == ' ') {
          text.remove_prefix(1);
        }

        // strtod wants a terminated string; the rate is the last field but one.
        const std::string number { text };
        char *end = nullptr;
        refresh = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || refresh <= 0.0) {
          return false;
        }
        text.remove_prefix(static_cast<std::size_t>(end - number.c_str()));
        while (!text.empty() && text.front() == ' ') {
          text.remove_prefix(1);
        }
        if (!starts_with(text, "Hz")) {
          return false;
        }
        text.remove_prefix(2);
      }

      bool current = false;
      bool preferred = false;
      if (const auto open = text.find('('); open != std::string_view::npos) {
        const auto close = text.find(')', open);
        const auto flags = close == std::string_view::npos ? text.substr(open + 1) : text.substr(open + 1, close - open - 1);
        current = flags.find("current") != std::string_view::npos;
        preferred = flags.find("preferred") != std::string_view::npos;
      }

      mode.width = width;
      mode.height = height;
      mode.refresh = refresh;
      mode.current = current;
      mode.preferred = preferred;
      mode.spec = mode_spec(width, height, refresh);
      return true;
    }

    /**
     * @brief Parse one head line, e.g. `DP-1 "Dell Inc. DELL U2720Q"`.
     */
    bool
    parse_head_line(std::string_view text, head_t &head) {
      const auto separator = text.find(' ');
      if (separator == std::string_view::npos || separator == 0) {
        return false;
      }
      head.name = std::string { text.substr(0, separator) };
      head.description.clear();

      const auto open = text.find('"', separator);
      if (open != std::string_view::npos) {
        const auto close = text.find_last_of('"');
        if (close > open) {
          head.description = std::string { text.substr(open + 1, close - open - 1) };
        }
      }
      return true;
    }
  }  // namespace

  bool
  available() {
    for (const char *dir : { "/usr/bin", "/usr/local/bin", "/bin" }) {
      if (::access((std::string { dir } + "/wlr-randr").c_str(), X_OK) == 0) {
        return true;
      }
    }
    return false;
  }

  bool
  probe_allowed() {
    return now().time_since_epoch().count() >= probe_after.load(std::memory_order_relaxed);
  }

  void
  note_unavailable(const std::string &reason) {
    probe_verdict.store(0, std::memory_order_relaxed);
    probe_after.store((now() + kRetryCooldown).time_since_epoch().count(), std::memory_order_relaxed);
    if (!unavailability_logged.exchange(true)) {
      BOOST_LOG(debug) << "wlr-randr output management unavailable (" << reason << ")";
    }
  }

  void
  note_available() {
    probe_verdict.store(1, std::memory_order_relaxed);
    probe_after.store(0, std::memory_order_relaxed);
    unavailability_logged.store(false, std::memory_order_relaxed);
  }

  bool
  session_supports_wlr_randr() {
    if (!available()) {
      return false;
    }

    if (probe_verdict.load(std::memory_order_relaxed) == 1) {
      return true;
    }

    if (!probe_allowed()) {
      return false;
    }

    // The probe *is* a query: there is no environment variable, bus name or
    // socket that announces a wlroots output manager, so the only honest answer
    // is what the compositor actually lists.
    const auto state = query();
    return !state.heads.empty();
  }

  std::string
  mode_spec(unsigned int width, unsigned int height, double refresh) {
    std::string spec = std::to_string(width) + "x" + std::to_string(height);
    if (refresh > 0.0) {
      // wlr-randr converts a rate to millihertz with round(hz * 1000) and
      // matches it against the advertised modes, so three decimals round-trip
      // every rate a compositor can report; trimming the trailing zeros keeps
      // the common ones readable ("60" rather than "60.000").
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.3f", refresh);
      std::string rate { buffer };
      while (!rate.empty() && rate.back() == '0') {
        rate.pop_back();
      }
      if (!rate.empty() && rate.back() == '.') {
        rate.pop_back();
      }
      spec += "@" + rate;
    }
    return spec;
  }

  state_t
  parse_state(const std::string &output) {
    state_t state;
    std::size_t position = 0;

    while (position <= output.size()) {
      const auto newline = output.find('\n', position);
      const auto end = newline == std::string::npos ? output.size() : newline;
      const std::string_view raw { output.data() + position, end - position };
      position = newline == std::string::npos ? output.size() + 1 : newline + 1;

      const std::string_view line = trim(raw);
      if (line.empty()) {
        continue;
      }

      // A head line is the only one that starts in column zero.
      if (!raw.empty() && raw.front() != ' ') {
        head_t head;
        if (parse_head_line(line, head)) {
          state.heads.push_back(std::move(head));
        }
        continue;
      }

      if (state.heads.empty()) {
        // Output that no head could own; the listing always starts with one.
        continue;
      }

      auto &head = state.heads.back();
      if (line == "Modes:") {
        continue;
      }
      if (starts_with(line, "Physical size:")) {
        parse_physical_size(trim(line.substr(std::strlen("Physical size:"))), head);
        continue;
      }
      if (starts_with(line, "Enabled:")) {
        head.enabled = trim(line.substr(std::strlen("Enabled:"))) == "yes";
        continue;
      }
      if (starts_with(line, "Position:")) {
        const auto value = trim(line.substr(std::strlen("Position:")));
        const auto comma = value.find(',');
        if (comma != std::string_view::npos) {
          head.x = std::atoi(std::string { value.substr(0, comma) }.c_str());
          head.y = std::atoi(std::string { value.substr(comma + 1) }.c_str());
        }
        continue;
      }
      if (starts_with(line, "Transform:")) {
        head.transform = std::string { trim(line.substr(std::strlen("Transform:"))) };
        continue;
      }
      if (starts_with(line, "Scale:")) {
        const std::string value { trim(line.substr(std::strlen("Scale:"))) };
        if (!value.empty()) {
          head.scale = std::strtod(value.c_str(), nullptr);
          if (head.scale <= 0.0) {
            head.scale = 1.0;
          }
        }
        continue;
      }

      // Everything else indented under a head is a mode.
      mode_t mode;
      if (parse_mode_line(line, mode)) {
        head.modes.push_back(std::move(mode));
      }
    }

    return state;
  }

  std::string
  build_args(const std::vector<request_t> &requests, bool dry_run) {
    std::string args;
    for (const auto &request : requests) {
      if (request.name.empty()) {
        continue;
      }
      if (!args.empty()) {
        args += ' ';
      }
      args += "--output " + quote(request.name);
      args += request.enabled ? " --on" : " --off";
      if (request.enabled && !request.mode.empty()) {
        args += " --mode " + quote(request.mode);
      }
    }

    if (dry_run && !args.empty()) {
      args += " --dryrun";
    }
    return args;
  }

  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout) {
    // vdd_utils::run_logged bounds the child and keeps inherited sockets out of
    // it: wlr-randr talks to a compositor that may be gone, in which case it
    // would otherwise wait on the Wayland connection forever.
    const auto result = display_device::vdd_utils::run_logged("wlr-randr" + (args.empty() ? std::string {} : " " + args), timeout);
    return { result.exit_code, result.output };
  }

  bool
  failure_means_unsupported(const std::string &output) {
    // Exactly what wlr-randr prints (to stderr) when it gives up on the
    // display before parsing any argument: the protocol is absent, or the
    // connection itself failed.
    return output.find("doesn't support") != std::string::npos ||
           output.find("failed to connect to display") != std::string::npos;
  }

  state_t
  query(std::chrono::milliseconds timeout) {
    if (!available()) {
      note_unavailable("wlr-randr is not installed");
      return {};
    }

    const auto result = run({}, timeout);
    if (result.exit_code != 0) {
      // The tool says exactly why, and the two cases matter: no compositor at
      // all is a session fact, a failing apply is a caller problem.
      note_unavailable(failure_means_unsupported(result.output) ? "this compositor has no wlr-output-management" :
                                     "wlr-randr failed: " + std::string { trim(result.output) });
      return {};
    }

    auto state = parse_state(result.output);
    if (state.heads.empty()) {
      note_unavailable("wlr-randr listed no outputs");
      return {};
    }

    note_available();
    return state;
  }
}  // namespace platf::wlr_randr
