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
    std::atomic_size_t enabled_session_count {0};
    std::atomic_bool producer_is_available {false};
    snapshot_t latest;
    std::uint64_t next_revision = 1;

    void
    clear_latest() {
      latest = {};
    }
  }  // namespace

  void
  set_session_enabled(std::uint32_t session_id, bool enabled) {
    std::lock_guard lock(state_mutex);
    const bool was_empty = enabled_sessions.empty();
    if (enabled) {
      enabled_sessions.insert(session_id);
      if (was_empty && !enabled_sessions.empty()) {
        // Wait for capture to republish the current shape. Sending a cached
        // shape here could race a display or mode change.
        clear_latest();
      }
    }
    else {
      enabled_sessions.erase(session_id);
      if (enabled_sessions.empty()) {
        clear_latest();
      }
    }
    enabled_session_count.store(enabled_sessions.size(), std::memory_order_release);
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
  set_producer_available(bool available) {
    producer_is_available.store(available, std::memory_order_release);
  }

  bool
  producer_available() {
    return producer_is_available.load(std::memory_order_acquire);
  }

  void
  publish_visibility(bool visible, std::uint32_t shape_id) {
    std::lock_guard lock(state_mutex);
    if (enabled_sessions.empty()) {
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
    if (enabled_sessions.empty()) {
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
}  // namespace cursor_channel
