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

  inline constexpr std::string_view key { "session_binding" };
  inline constexpr std::string_view automatic_value { "0" };
  inline constexpr std::string_view console_value { "1" };

  inline std::optional<mode_e>
  parse(std::string_view value) {
    if (value == automatic_value) {
      return mode_e::automatic;
    }
    if (value == console_value) {
      return mode_e::console;
    }
    return std::nullopt;
  }
}
