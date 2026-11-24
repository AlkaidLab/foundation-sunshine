/**
 * @file src/config_session_binding.h
 * @brief Interactive-session binding policy shared by Sunshine and its service.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace config_session_binding {
  enum class mode_e : std::uint8_t {
    automatic = 0,
    console = 1,
  };

  enum class user_token_purpose_e : std::uint8_t {
    capture,
    gui,
  };

  // Keep capture on the console unless interactive-session capture is explicitly requested.
  inline constexpr mode_e default_mode = mode_e::console;
  inline constexpr std::uint32_t invalid_session_id = 0xFFFFFFFFu;
  inline constexpr std::string_view key { "session_binding" };
  inline constexpr std::string_view automatic_value { "0" };
  inline constexpr std::string_view console_value { "1" };

  inline constexpr std::optional<mode_e>
  parse(std::string_view value) {
    // The normal config parser accepts quoted, zero-padded and hexadecimal
    // integers. Use the same interpretation in the service and the host.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value.remove_prefix(1);
      value.remove_suffix(1);
    }
    bool negative = false;
    if (!value.empty() && value.front() == '-') {
      negative = true;
      value.remove_prefix(1);
    }
    if (value.size() >= 2 && value.substr(0, 2) == "0x") {
      value.remove_prefix(2);
    }
    if (value.empty()) {
      return std::nullopt;
    }
    unsigned int parsed = 0;
    for (const char digit : value) {
      if (digit != '0' && digit != '1') {
        return std::nullopt;
      }
      parsed = parsed * 10 + static_cast<unsigned int>(digit - '0');
      if (parsed > 1) {
        return std::nullopt;
      }
    }
    if (negative && parsed != 0) {
      return std::nullopt;
    }
    if (parsed == 0) {
      return mode_e::automatic;
    }
    return mode_e::console;
  }

  inline constexpr mode_e
  parse_or_default(std::string_view value) {
    return parse(value).value_or(default_mode);
  }

  // GUI ownership is selected independently; this policy chooses only the
  // session used by the streaming core and its capture-related user token.
  inline constexpr std::uint32_t
  capture_session(mode_e mode, std::uint32_t console_session, std::uint32_t interactive_session) {
    if (mode == mode_e::automatic && interactive_session != invalid_session_id) {
      return interactive_session;
    }
    return console_session;
  }

  struct session_targets_t {
    std::uint32_t capture;
    std::uint32_t gui;
  };

  inline constexpr session_targets_t
  select_sessions(mode_e mode, std::uint32_t console_session, std::uint32_t interactive_session) {
    return { capture_session(mode, console_session, interactive_session), interactive_session };
  }

  struct session_update_t {
    session_targets_t targets;
    bool update_gui;
    bool restart_capture;
    bool retry;
  };

  inline constexpr session_update_t
  update_sessions(mode_e mode, std::uint32_t current_capture, std::uint32_t current_gui,
    std::uint32_t console_session, std::uint32_t interactive_session, bool query_failed) {
    if (query_failed) {
      interactive_session = current_gui;
    }
    const auto targets = select_sessions(mode, console_session, interactive_session);
    const bool capture_known = targets.capture != invalid_session_id && !(query_failed && mode == mode_e::automatic);
    return {
      targets,
      !query_failed && targets.gui != current_gui,
      capture_known && targets.capture != current_capture,
      query_failed || targets.capture == invalid_session_id,
    };
  }

  inline constexpr std::uint32_t
  user_token_session(user_token_purpose_e purpose, mode_e mode, std::uint32_t console_session, std::uint32_t interactive_session) {
    if (purpose == user_token_purpose_e::gui) {
      return interactive_session;
    }
    return capture_session(mode, console_session, interactive_session);
  }
}  // namespace config_session_binding
