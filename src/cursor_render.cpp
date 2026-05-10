/**
 * @file src/cursor_render.cpp
 * @brief Cursor display mode negotiation and cursor metadata helpers.
 */
#include "cursor_render.h"

#include <atomic>

namespace cursor_render {
  namespace {
    constexpr std::uint32_t fnv_offset_basis = 2166136261u;
    constexpr std::uint32_t fnv_prime = 16777619u;

    thread_local std::optional<update_t> pending_update;
    std::atomic<std::uint32_t> state_seq { 0 };

    void
    hash_byte(std::uint32_t &hash, std::uint8_t value) {
      hash ^= value;
      hash *= fnv_prime;
    }

    template <class T>
    void
    hash_value(std::uint32_t &hash, T value) {
      auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash_byte(hash, bytes[i]);
      }
    }
  }  // namespace

  mode_e
  mode_from_view(std::string_view value, mode_e fallback) {
    if (value == "remote") {
      return mode_e::remote;
    }
    if (value == "client") {
      return mode_e::client;
    }
    if (value == "auto" || value == "automatic") {
      return mode_e::automatic;
    }
    return fallback;
  }

  app_mode_e
  app_mode_from_view(std::string_view value, app_mode_e fallback) {
    if (value == "inherit") {
      return app_mode_e::inherit;
    }
    if (value == "remote") {
      return app_mode_e::remote;
    }
    if (value == "client") {
      return app_mode_e::client;
    }
    if (value == "auto" || value == "automatic") {
      return app_mode_e::automatic;
    }
    return fallback;
  }

  std::string_view
  to_string(mode_e mode) {
    switch (mode) {
      case mode_e::remote:
        return "remote";
      case mode_e::client:
        return "client";
      case mode_e::automatic:
        return "auto";
    }
    return "remote";
  }

  std::string_view
  to_string(app_mode_e mode) {
    switch (mode) {
      case app_mode_e::inherit:
        return "inherit";
      case app_mode_e::remote:
        return "remote";
      case app_mode_e::client:
        return "client";
      case app_mode_e::automatic:
        return "auto";
    }
    return "inherit";
  }

  std::string_view
  to_string(effective_mode_e mode) {
    switch (mode) {
      case effective_mode_e::remote:
        return "remote";
      case effective_mode_e::client:
        return "client";
    }
    return "remote";
  }

  mode_e
  resolve_requested_mode(mode_e global_mode, app_mode_e app_mode) {
    switch (app_mode) {
      case app_mode_e::inherit:
        return global_mode;
      case app_mode_e::remote:
        return mode_e::remote;
      case app_mode_e::client:
        return mode_e::client;
      case app_mode_e::automatic:
        return mode_e::automatic;
    }
    return global_mode;
  }

  effective_result_t
  resolve_effective_mode(mode_e global_mode, app_mode_e app_mode, const client_caps_t &client_caps, bool backend_metadata_supported, bool force_remote) {
    if (force_remote) {
      return { effective_mode_e::remote, "forced remote" };
    }

    const auto requested = resolve_requested_mode(global_mode, app_mode);
    if (requested == mode_e::remote) {
      return { effective_mode_e::remote, "requested remote" };
    }

    if (!client_caps.cursor_channel_v1) {
      return { effective_mode_e::remote, "client does not support cursor channel v1" };
    }

    if (requested == mode_e::client && !backend_metadata_supported) {
      return { effective_mode_e::remote, "requested client but capture backend lacks cursor metadata" };
    }

    if (requested == mode_e::automatic && !backend_metadata_supported) {
      return { effective_mode_e::remote, "auto fallback: capture backend lacks cursor metadata" };
    }

    return { effective_mode_e::client, requested == mode_e::client ? "requested client" : "auto selected client" };
  }

  std::uint32_t
  shape_id(shape_format_e format, std::uint16_t width, std::uint16_t height, std::uint16_t pitch, std::int16_t hotspot_x, std::int16_t hotspot_y, const std::vector<std::uint8_t> &pixels) {
    std::uint32_t hash = fnv_offset_basis;
    hash_value(hash, static_cast<std::uint8_t>(format));
    hash_value(hash, width);
    hash_value(hash, height);
    hash_value(hash, pitch);
    hash_value(hash, hotspot_x);
    hash_value(hash, hotspot_y);
    for (auto byte : pixels) {
      hash_byte(hash, byte);
    }
    return hash == 0 ? 1 : hash;
  }

  std::uint32_t
  next_state_seq() {
    return state_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  bool
  seq_newer(std::uint32_t seq, std::uint32_t last_seq) {
    return static_cast<std::int32_t>(seq - last_seq) > 0;
  }

  bool
  should_skip_video_frame(effective_mode_e mode, bool mouse_update, bool frame_update, bool fallback_remote) {
    return mode == effective_mode_e::client && mouse_update && !frame_update && !fallback_remote;
  }

  void
  publish_thread_update(update_t update) {
    pending_update = std::move(update);
  }

  std::optional<update_t>
  take_thread_update() {
    auto result = std::move(pending_update);
    pending_update.reset();
    return result;
  }

  void
  clear_thread_update() {
    pending_update.reset();
  }

}  // namespace cursor_render
