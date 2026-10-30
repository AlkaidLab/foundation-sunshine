/**
 * @file src/host_cursor.cpp
 * @brief See host_cursor.h.
 */
#include "host_cursor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace host_cursor {
  namespace {
    std::int32_t
    round_to_int32(double value) {
      constexpr double lo = std::numeric_limits<std::int32_t>::min();
      constexpr double hi = std::numeric_limits<std::int32_t>::max();
      return std::int32_t(std::clamp(std::floor(value + 0.5), lo, hi));
    }

    host_cursor_protocol::state_t
    comparable(host_cursor_protocol::state_t state) {
      using namespace host_cursor_protocol::state_flags;
      state.seq = 0;
      state.flags &= std::uint8_t(~(has_position | resync));
      state.x = 0;
      state.y = 0;
      return state;
    }

    bool
    within_budget(session_state_t &session, std::size_t bytes, clock::time_point now) {
      if (now - session.budget_window_start >= std::chrono::seconds(1)) {
        session.budget_window_start = now;
        session.budget_bytes = 0;
      }
      if (session.budget_bytes && session.budget_bytes + bytes > shape_budget_bytes_per_second) {
        return false;
      }
      session.budget_bytes += bytes;
      return true;
    }
  }  // namespace

  point_t
  to_stream(const geometry_t &geometry, double x, double y) {
    return {
      round_to_int32((x - geometry.offset_x) / geometry.scalar_inv + geometry.client_offset_x),
      round_to_int32((y - geometry.offset_y) / geometry.scalar_inv + geometry.client_offset_y),
    };
  }

  std::uint32_t
  shape_registry_t::insert(host_cursor_bitmap::image_t image) {
    const auto hash = host_cursor_bitmap::content_hash(image);
    std::lock_guard lg { _lock };
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
      if (it->hash == hash && *it->image == image) {
        auto entry = std::move(*it);
        _entries.erase(it);
        _entries.push_back(std::move(entry));
        return _entries.back().id;
      }
    }
    if (_entries.size() >= capacity) {
      _entries.erase(_entries.begin());
    }
    const auto id = _next_id++;
    if (!_next_id) _next_id = 1;
    _entries.push_back({ id, hash, std::make_shared<const host_cursor_bitmap::image_t>(std::move(image)) });
    return id;
  }

  std::shared_ptr<const host_cursor_bitmap::image_t>
  shape_registry_t::find(std::uint32_t id) const {
    std::lock_guard lg { _lock };
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
      if (it->id == id) {
        auto entry = std::move(*it);
        _entries.erase(it);
        _entries.push_back(std::move(entry));
        return _entries.back().image;
      }
    }
    return nullptr;
  }

  shape_registry_t &
  shapes() {
    static shape_registry_t registry;
    return registry;
  }

  void
  injection_history_t::note_absolute(std::int32_t x, std::int32_t y, clock::time_point now) {
    std::lock_guard lg { _lock };
    _targets[_next] = { x, y, now };
    _next = (_next + 1) % depth;
    _count = std::min(_count + 1, depth);
  }

  bool
  injection_history_t::explains(std::int32_t x, std::int32_t y, clock::time_point now) const {
    std::lock_guard lg { _lock };
    for (std::size_t i = 0; i < _count; ++i) {
      const auto &target = _targets[i];
      if (now - target.at <= absolute_window &&
          std::abs(std::int64_t(target.x) - x) <= tolerance_px &&
          std::abs(std::int64_t(target.y) - y) <= tolerance_px) {
        return true;
      }
    }
    return false;
  }

  void
  apply_request(session_state_t &session, const host_cursor_protocol::request_t &request) {
    const bool subscribe = request.flags & host_cursor_protocol::request_flags::subscribe;
    if (subscribe && !session.subscribed) {
      session.resync_pending = true;
    }
    else if (!subscribe) {
      session.resync_pending = false;
      session.last_sent.reset();
      session.shapes_sent.clear();
    }
    session.subscribed = subscribe;
    session.client_draws = subscribe && (request.flags & host_cursor_protocol::request_flags::client_draws);
    if (subscribe && request.resend_shape_id) {
      session.resend_shape_id = request.resend_shape_id;
    }
  }

  update_t
  plan_update(session_state_t &session,
              const snapshot_t &snapshot,
              const geometry_t &geometry,
              bool composited,
              clock::time_point now,
              const shape_registry_t &registry) {
    using namespace host_cursor_protocol::state_flags;
    update_t update;
    if (!session.subscribed) {
      return update;
    }

    // Until the session's encoder publishes its input geometry there is no stream
    // coordinate space; a state now would carry a zero reference size, which the
    // client rejects. Keep any resync pending until then.
    if (geometry.stream_width <= 0 || geometry.stream_height <= 0) {
      return update;
    }

    const bool resync_now = session.resync_pending;
    if (resync_now) {
      session.shapes_sent.clear();
      session.last_sent.reset();
      session.last_position.reset();
      session.last_warp_serial = snapshot.warp_serial;
    }

    const bool usable = snapshot.available && geometry.valid && geometry.stream_width > 0 &&
                        geometry.stream_height > 0 && geometry.stream_width <= 0xFFFF &&
                        geometry.stream_height <= 0xFFFF && geometry.scalar_inv > 0;

    host_cursor_protocol::state_t state;
    state.ref_width = std::uint16_t(std::clamp(geometry.stream_width, 0, 0xFFFF));
    state.ref_height = std::uint16_t(std::clamp(geometry.stream_height, 0, 0xFFFF));
    if (!usable) state.flags |= fallback;
    if (composited) state.flags |= host_cursor_protocol::state_flags::composited;
    if (usable && snapshot.visible) state.flags |= visible;

    const auto queue_shape = [&](std::uint32_t id, const std::shared_ptr<const host_cursor_bitmap::image_t> &image) {
      if (std::find(update.shape_ids.begin(), update.shape_ids.end(), id) != update.shape_ids.end()) {
        return true;
      }
      if (!within_budget(session, image->bgra.size(), now)) {
        return false;
      }
      update.shapes.push_back(image);
      update.shape_ids.push_back(id);
      return true;
    };

    // The current shape, unless the client has not had it and the budget is spent,
    // in which case it keeps the shape it has.
    std::uint32_t shape_id = 0;
    std::shared_ptr<const host_cursor_bitmap::image_t> shape;
    if (usable && snapshot.shape_id) {
      shape = registry.find(snapshot.shape_id);
      if (shape && (session.shapes_sent.contains(snapshot.shape_id) || queue_shape(snapshot.shape_id, shape))) {
        shape_id = snapshot.shape_id;
      }
      else if (session.last_sent && session.last_sent->shape_id) {
        shape_id = session.last_sent->shape_id;
        shape = registry.find(shape_id);
      }
      else {
        shape.reset();
      }
    }
    if (session.resend_shape_id) {
      if (auto image = registry.find(session.resend_shape_id)) {
        (void) queue_shape(session.resend_shape_id, image);
      }
      session.resend_shape_id = 0;
    }
    state.shape_id = shape ? shape_id : 0;

    point_t position;
    if (usable) {
      const double pixel_scale = shape ? shape->pixel_scale : 1;
      state.scale_q16 = std::uint32_t(std::clamp<double>(std::floor(65536.0 * pixel_scale / geometry.scalar_inv + 0.5), 1, 0xFFFFFFFF));
      position = to_stream(geometry, snapshot.x, snapshot.y);
      if (snapshot.clipped) {
        state.flags |= has_clip;
        const auto top_left = to_stream(geometry, snapshot.clip.left, snapshot.clip.top);
        const auto bottom_right = to_stream(geometry, snapshot.clip.right, snapshot.clip.bottom);
        state.clip_left = std::clamp(top_left.x, 0, std::int32_t(state.ref_width));
        state.clip_top = std::clamp(top_left.y, 0, std::int32_t(state.ref_height));
        state.clip_right = std::clamp(bottom_right.x, state.clip_left, std::int32_t(state.ref_width));
        state.clip_bottom = std::clamp(bottom_right.y, state.clip_top, std::int32_t(state.ref_height));
      }
    }

    const auto &previous = session.last_sent;
    const bool is_visible = state.flags & visible;
    const bool became_visible = is_visible && (!previous || !(previous->flags & visible));
    const bool clip_changed = previous &&
                              ((previous->flags & has_clip) != (state.flags & has_clip) ||
                                previous->clip_left != state.clip_left || previous->clip_top != state.clip_top ||
                                previous->clip_right != state.clip_right || previous->clip_bottom != state.clip_bottom);
    const bool moved = !session.last_position || *session.last_position != position;
    const bool warped = snapshot.warp_serial != session.last_warp_serial && moved;
    // A client that draws the cursor already knows where it is (it put it there),
    // so only host-originated moves are reported to it. A client that does not
    // draw it needs the position whenever it may switch back to absolute input.
    const bool report_position = usable &&
                                 (resync_now ||
                                   (is_visible && (became_visible || clip_changed || warped || (!session.client_draws && moved))));
    if (report_position) {
      state.flags |= has_position;
      state.x = position.x;
      state.y = position.y;
    }
    if (resync_now) state.flags |= resync;

    session.last_warp_serial = snapshot.warp_serial;
    if (usable) session.last_position = position;
    session.shapes_sent.insert(update.shape_ids.begin(), update.shape_ids.end());

    const auto normalized = comparable(state);
    if (!resync_now && !report_position && previous && *previous == normalized) {
      return update;
    }

    state.seq = ++session.seq;
    update.state = state;
    session.last_sent = normalized;
    session.resync_pending = false;
    return update;
  }

#ifndef _WIN32
  namespace tracker {
    bool
    supported() {
      return false;
    }

    std::shared_ptr<void>
    acquire() {
      return nullptr;
    }

    snapshot_t
    latest() {
      return {};
    }

    void
    note_absolute_injection(std::int32_t, std::int32_t) {
    }
  }  // namespace tracker
#endif
}  // namespace host_cursor
