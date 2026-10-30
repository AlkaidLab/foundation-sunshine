/**
 * @file src/cursor_channel.cpp
 * @brief See cursor_channel.h.
 */
#include "cursor_channel.h"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace cursor_channel {
  namespace {
    std::mutex state_mutex;
    std::unordered_set<std::uint32_t> enabled_sessions;
    std::size_t shape_consumers = 0;
    std::atomic_size_t enabled_session_count {0};
    std::atomic_bool shapes_are_wanted {false};
    std::atomic_uint64_t shape_publish_epoch {0};
    std::atomic_bool every_session_draws {false};
    std::atomic_bool producer_is_available {false};
    std::atomic_bool capture_draws_cursor {true};
    snapshot_t latest;
    std::uint64_t next_revision = 1;
    std::uint64_t shape_revision = 0;

    void
    clear_latest() {
      latest = {};
    }

    bool
    has_consumers() {
      return !enabled_sessions.empty() || shape_consumers != 0;
    }

    // Called with state_mutex held after a v1 session or a v2 consumer came or went.
    void
    consumers_changed(bool had_consumers) {
      const bool has = has_consumers();
      if (has && !had_consumers) {
        // Wait for capture to republish the current shape. Sending a cached
        // shape here could race a display or mode change.
        clear_latest();
        shape_publish_epoch.fetch_add(1, std::memory_order_acq_rel);
      }
      else if (!has && had_consumers) {
        clear_latest();
      }
      enabled_session_count.store(enabled_sessions.size(), std::memory_order_release);
      shapes_are_wanted.store(has, std::memory_order_release);
    }
  }  // namespace

  void
  set_session_enabled(std::uint32_t session_id, bool enabled) {
    std::lock_guard lock(state_mutex);
    const bool had_consumers = has_consumers();
    if (enabled) {
      enabled_sessions.insert(session_id);
    }
    else {
      enabled_sessions.erase(session_id);
    }
    consumers_changed(had_consumers);
  }

  void
  remove_session(std::uint32_t session_id) {
    set_session_enabled(session_id, false);
  }

  bool
  local_mode_active() {
    return enabled_session_count.load(std::memory_order_acquire) != 0;
  }

  void
  add_shape_consumer() {
    std::lock_guard lock(state_mutex);
    const bool had_consumers = has_consumers();
    ++shape_consumers;
    consumers_changed(had_consumers);
  }

  void
  remove_shape_consumer() {
    std::lock_guard lock(state_mutex);
    const bool had_consumers = has_consumers();
    if (shape_consumers != 0) {
      --shape_consumers;
    }
    consumers_changed(had_consumers);
  }

  bool
  shapes_wanted() {
    return shapes_are_wanted.load(std::memory_order_acquire);
  }

  std::uint64_t
  publish_epoch() {
    return shape_publish_epoch.load(std::memory_order_acquire);
  }

  void
  set_all_sessions_draw(bool all_draw) {
    every_session_draws.store(all_draw, std::memory_order_release);
  }

  bool
  all_sessions_draw() {
    return every_session_draws.load(std::memory_order_acquire);
  }

  bool
  frames_exclude_cursor() {
    return local_mode_active() || all_sessions_draw();
  }

  void
  set_producer_available(bool available) {
    producer_is_available.store(available, std::memory_order_release);
  }

  bool
  producer_available() {
    return producer_is_available.load(std::memory_order_acquire);
  }

  void
  set_capture_composites_cursor(bool composites) {
    capture_draws_cursor.store(composites, std::memory_order_release);
  }

  bool
  capture_composites_cursor() {
    return capture_draws_cursor.load(std::memory_order_acquire);
  }

  void
  publish_visibility(bool visible, std::uint32_t shape_id) {
    std::lock_guard lock(state_mutex);
    if (!has_consumers()) {
      return;
    }
    if (latest.valid && latest.visible == visible && latest.shape_id == shape_id) {
      return;
    }

    if (latest.shape_id != shape_id) {
      latest.has_shape = false;
      latest.width = 0;
      latest.height = 0;
      latest.hotspot_x = 0;
      latest.hotspot_y = 0;
      latest.bgra.clear();
    }
    latest.valid = true;
    latest.visible = visible;
    latest.shape_id = shape_id;
    latest.revision = next_revision++;
  }

  void
  publish_shape(bool visible,
                std::uint32_t shape_id,
                std::uint16_t width,
                std::uint16_t height,
                std::int16_t hotspot_x,
                std::int16_t hotspot_y,
                std::vector<std::uint8_t> bgra) {
    std::lock_guard lock(state_mutex);
    if (!has_consumers()) {
      return;
    }

    latest.valid = true;
    latest.visible = visible;
    latest.has_shape = true;
    latest.shape_id = shape_id;
    latest.width = width;
    latest.height = height;
    latest.hotspot_x = hotspot_x;
    latest.hotspot_y = hotspot_y;
    latest.bgra = std::move(bgra);
    latest.revision = next_revision++;
    ++shape_revision;
  }

  bool
  copy_latest(std::uint64_t known_revision, snapshot_t &out) {
    std::lock_guard lock(state_mutex);
    if (!latest.valid || latest.revision == known_revision) {
      return false;
    }
    out = latest;
    return true;
  }

  bool
  copy_shape_if_newer(std::uint64_t &known_shape_revision, snapshot_t &out) {
    std::lock_guard lock(state_mutex);
    if (!latest.valid || !latest.has_shape || shape_revision == known_shape_revision) {
      return false;
    }
    out = latest;
    known_shape_revision = shape_revision;
    return true;
  }
}  // namespace cursor_channel
