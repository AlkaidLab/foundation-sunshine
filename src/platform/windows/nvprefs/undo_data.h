/**
 * @file src/platform/windows/nvprefs/undo_data.h
 * @brief Declarations for undoing changes to nvidia preferences.
 */
#pragma once

// standard library headers
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nvprefs {

  class undo_data_t {
  public:
    struct data_t {
      struct opengl_swapchain_t {
        uint32_t our_value;
        std::optional<uint32_t> undo_value;
      };

      // A single driver setting we touched: the value we wrote ("our_value")
      // and the value the user originally had ("undo_value", nullopt means the
      // setting was previously unset on this profile, so restore = delete).
      struct setting_undo_t {
        uint32_t our_value;
        std::optional<uint32_t> undo_value;
      };

      // Stream optimizations applied to a per-game application profile.
      // - profile_name: the SunshineStream-* profile we created or reused
      // - exe_path: the lower-cased executable basename added to the profile
      // - profile_was_created: true if we created profile_name (delete on restore if no other apps)
      // - application_was_added: true if we added exe_path to profile_name (remove on restore)
      // - vsync/frl/pstate/prerender: the four settings we may have written
      struct game_profile_t {
        std::string profile_name;
        std::string exe_path;
        bool profile_was_created = false;
        bool application_was_added = false;
        std::optional<setting_undo_t> vsync;
        std::optional<setting_undo_t> frl;
        std::optional<setting_undo_t> pstate;
        std::optional<setting_undo_t> prerender;
      };

      // Stream optimizations applied to the BASE (global) driver profile.
      // Mirrors the four settings we may write at the global level. Restore
      // semantics match game_profile_t: nullopt = not touched, undo_value
      // nullopt = setting did not exist before us.
      struct base_extras_t {
        std::optional<setting_undo_t> vsync;
        std::optional<setting_undo_t> frl;
        std::optional<setting_undo_t> pstate;
        std::optional<setting_undo_t> prerender;
      };

      std::optional<opengl_swapchain_t> opengl_swapchain;
      std::optional<game_profile_t> game_profile;
      std::optional<base_extras_t> base_extras;
    };

    void
    set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value);

    std::optional<data_t::opengl_swapchain_t>
    get_opengl_swapchain() const;

    void
    set_game_profile(const data_t::game_profile_t &game_profile);

    std::optional<data_t::game_profile_t>
    get_game_profile() const;

    void
    clear_game_profile();

    void
    set_base_extras(const data_t::base_extras_t &base_extras);

    std::optional<data_t::base_extras_t>
    get_base_extras() const;

    void
    clear_base_extras();

    std::string
    write() const;

    void
    read(const std::vector<char> &buffer);

    void
    merge(const undo_data_t &newer_data);

  private:
    data_t data;
  };

}  // namespace nvprefs
