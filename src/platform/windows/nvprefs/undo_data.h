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
      struct setting_t {
        uint32_t our_value;
        std::optional<uint32_t> undo_value;
      };

      std::optional<setting_t> opengl_swapchain;
      std::optional<setting_t> vsync;
    };

    void
    set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value);

    std::optional<data_t::setting_t>
    get_opengl_swapchain() const;

    void
    set_vsync(uint32_t our_value, std::optional<uint32_t> undo_value);

    std::optional<data_t::setting_t>
    get_vsync() const;

    void
    clear_vsync();

    bool
    empty() const;

    std::string
    write() const;

    bool
    read(const std::vector<char> &buffer);

    void
    merge(const undo_data_t &newer_data);

  private:
    data_t data;
  };

}  // namespace nvprefs
