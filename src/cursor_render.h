/**
 * @file src/cursor_render.h
 * @brief Cursor display mode negotiation and cursor metadata packets.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cursor_render {

  enum class mode_e : std::uint8_t {
    remote,
    client,
    automatic,
  };

  enum class app_mode_e : std::uint8_t {
    inherit,
    remote,
    client,
    automatic,
  };

  enum class effective_mode_e : std::uint8_t {
    remote,
    client,
  };

  enum class shape_format_e : std::uint8_t {
    bgra32_straight = 1,
    bgra32_premul = 2,
    mono_xor = 3,
    masked_color = 4,
  };

  enum state_flags_e : std::uint8_t {
    state_flag_visible = 1 << 0,
    state_flag_fallback_remote = 1 << 1,
    state_flag_shape_valid = 1 << 2,
    state_flag_client_overlay_allowed = 1 << 3,
  };

  struct client_caps_t {
    bool cursor_channel_v1 {};
    bool cursor_native_supported {};
    bool cursor_overlay_supported {};
    bool cursor_state_unreliable_supported {};
  };

  struct effective_result_t {
    effective_mode_e mode { effective_mode_e::remote };
    std::string reason;
  };

  struct shape_t {
    std::uint8_t version { 1 };
    shape_format_e format { shape_format_e::bgra32_straight };
    std::uint16_t width {};
    std::uint16_t height {};
    std::uint16_t pitch {};
    std::int16_t hotspot_x {};
    std::int16_t hotspot_y {};
    std::uint32_t shape_id {};
    std::vector<std::uint8_t> pixels;
  };

  struct state_t {
    std::uint8_t version { 1 };
    std::uint8_t flags {};
    std::uint16_t display_id {};
    std::uint32_t seq {};
    std::uint32_t shape_id {};
    std::int32_t shape_left {};
    std::int32_t shape_top {};
    std::int16_t hotspot_x {};
    std::int16_t hotspot_y {};
    std::uint16_t dpi_scale_q8 { 256 };
    std::uint16_t reserved {};
    std::uint64_t host_qpc_time {};
  };

  struct update_t {
    std::optional<shape_t> shape;
    state_t state;
  };

  mode_e
  mode_from_view(std::string_view value, mode_e fallback = mode_e::remote);

  app_mode_e
  app_mode_from_view(std::string_view value, app_mode_e fallback = app_mode_e::inherit);

  std::string_view
  to_string(mode_e mode);

  std::string_view
  to_string(app_mode_e mode);

  std::string_view
  to_string(effective_mode_e mode);

  mode_e
  resolve_requested_mode(mode_e global_mode, app_mode_e app_mode);

  effective_result_t
  resolve_effective_mode(mode_e global_mode, app_mode_e app_mode, const client_caps_t &client_caps, bool backend_metadata_supported, bool force_remote);

  std::uint32_t
  shape_id(shape_format_e format, std::uint16_t width, std::uint16_t height, std::uint16_t pitch, std::int16_t hotspot_x, std::int16_t hotspot_y, const std::vector<std::uint8_t> &pixels);

  std::uint32_t
  next_state_seq();

  bool
  seq_newer(std::uint32_t seq, std::uint32_t last_seq);

  bool
  should_skip_video_frame(effective_mode_e mode, bool mouse_update, bool frame_update, bool fallback_remote);

  void
  publish_thread_update(update_t update);

  std::optional<update_t>
  take_thread_update();

  void
  clear_thread_update();

}  // namespace cursor_render
