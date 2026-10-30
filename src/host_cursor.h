/**
 * @file src/host_cursor.h
 * @brief Cursor protocol v2: per-session policy, shape registry and the platform tracker interface.
 *
 * See docs/client-cursor-v2-design.md.
 */
#pragma once

#include "host_cursor_bitmap.h"
#include "host_cursor_protocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace host_cursor {
  using clock = std::chrono::steady_clock;

  struct rect_t {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    bool
    operator==(const rect_t &) const = default;
  };

  // Desktop coordinates are relative to the virtual-desktop origin, the space
  // input::touch_port_t::offset_x/offset_y use on Windows.
  struct snapshot_t {
    std::uint64_t generation = 0;
    bool available = false;  // false: cursor information is unavailable (fallback)
    bool visible = false;
    std::uint32_t shape_id = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool clipped = false;
    rect_t clip;
    std::uint64_t warp_serial = 0;  // increments when the host moved the cursor itself
  };

  // The subset of input::touch_port_t that maps desktop coordinates to the stream.
  struct geometry_t {
    bool valid = false;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    std::int32_t stream_width = 0;
    std::int32_t stream_height = 0;
    float client_offset_x = 0;
    float client_offset_y = 0;
    float scalar_inv = 1;
  };

  struct point_t {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool
    operator==(const point_t &) const = default;
  };

  /**
   * @brief Map desktop coordinates to the stream.
   *
   * The inverse of input::client_to_touchport() followed by platf::abs_mouse() on
   * Windows, so a reported position is where absolute input would put the cursor.
   */
  point_t
  to_stream(const geometry_t &geometry, double x, double y);

  /**
   * @brief Converted shapes, deduplicated by content.
   *
   * Thread-safe; ids are never reused within a process.
   */
  class shape_registry_t {
  public:
    static constexpr std::size_t capacity = 64;

    std::uint32_t
    insert(host_cursor_bitmap::image_t image);

    std::shared_ptr<const host_cursor_bitmap::image_t>
    find(std::uint32_t id) const;

  private:
    struct entry_t {
      std::uint32_t id;
      std::uint64_t hash;
      std::shared_ptr<const host_cursor_bitmap::image_t> image;
    };

    mutable std::mutex _lock;
    mutable std::vector<entry_t> _entries;  // most recently used last
    std::uint32_t _next_id = 1;
  };

  shape_registry_t &
  shapes();

  /**
   * @brief Recent absolute positions this host injected.
   *
   * Lets the tracker tell a cursor the client moved from one the host moved (an
   * application, a local mouse, or relative input such as gamepad mouse
   * emulation, which a drawing client must follow).
   */
  class injection_history_t {
  public:
    static constexpr std::size_t depth = 8;
    static constexpr std::int32_t tolerance_px = 3;
    static constexpr auto absolute_window = std::chrono::milliseconds(250);

    void
    note_absolute(std::int32_t x, std::int32_t y, clock::time_point now);

    /**
     * @brief Whether the injected input explains a cursor at (x, y).
     */
    bool
    explains(std::int32_t x, std::int32_t y, clock::time_point now) const;

  private:
    struct target_t {
      std::int32_t x;
      std::int32_t y;
      clock::time_point at;
    };

    mutable std::mutex _lock;
    std::array<target_t, depth> _targets {};
    std::size_t _next = 0;
    std::size_t _count = 0;
  };

  // Implemented per platform (Windows only today; elsewhere supported() is false).
  namespace tracker {
    bool
    supported();

    /**
     * @brief Keep the tracker polling while the returned handle is alive.
     */
    std::shared_ptr<void>
    acquire();

    snapshot_t
    latest();

    /**
     * @brief Record an absolute move the input path injected, in desktop coordinates.
     */
    void
    note_absolute_injection(std::int32_t x, std::int32_t y);
  }  // namespace tracker

  /**
   * @brief Whether frames may leave the cursor out for the v2 sessions.
   *
   * Only when at least one session is connected, every connected session draws
   * the cursor itself and cursor information is available. Sessions still waiting
   * for their control peer count as connected and not drawing.
   */
  constexpr bool
  all_sessions_draw(std::size_t connected, std::size_t client_drawing, bool tracker_available) {
    return connected > 0 && client_drawing == connected && tracker_available;
  }

  struct session_state_t {
    bool subscribed = false;
    bool client_draws = false;
    bool resync_pending = false;
    std::uint32_t resend_shape_id = 0;
    std::uint32_t seq = 0;
    std::optional<host_cursor_protocol::state_t> last_sent;
    std::optional<point_t> last_position;
    std::uint64_t last_warp_serial = 0;
    std::set<std::uint32_t> shapes_sent;
    std::shared_ptr<void> tracker_handle;  // held by the owner while subscribed

    clock::time_point budget_window_start {};
    std::size_t budget_bytes = 0;
  };

  // New shapes a session may receive per second; a cursor that changes faster
  // than this keeps its previous shape until the budget recovers.
  constexpr std::size_t shape_budget_bytes_per_second = 2 * 1024 * 1024;

  /**
   * @brief Apply a v2 request. A new subscription schedules a resync.
   */
  void
  apply_request(session_state_t &session, const host_cursor_protocol::request_t &request);

  struct update_t {
    std::vector<std::shared_ptr<const host_cursor_bitmap::image_t>> shapes;  // send before the state
    std::vector<std::uint32_t> shape_ids;
    std::optional<host_cursor_protocol::state_t> state;
  };

  /**
   * @brief Decide what a session needs to receive for `snapshot` and record it as sent.
   * @param composited Whether frames currently contain the cursor.
   */
  update_t
  plan_update(session_state_t &session,
              const snapshot_t &snapshot,
              const geometry_t &geometry,
              bool composited,
              clock::time_point now,
              const shape_registry_t &registry = shapes());
}  // namespace host_cursor
