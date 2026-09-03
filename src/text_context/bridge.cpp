/**
 * @file src/text_context/bridge.cpp
 * @brief See text_context/bridge.h.
 */
#include "bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace text_context {
  namespace {
    using clock_t = std::chrono::steady_clock;
    constexpr auto kMatchWindow = std::chrono::milliseconds(1200);
    constexpr auto kGuiAliveWindow = std::chrono::seconds(30);
    constexpr std::int32_t kHitSlopPx = 12;
    constexpr std::int64_t kDragSlopSquared = 24 * 24;
    constexpr std::size_t kMaxRecentCandidates = 128;
    constexpr std::size_t kMaxActiveTouchesPerSession = 16;
    constexpr auto kActiveTouchLifetime = std::chrono::seconds(30);

    constexpr std::uint16_t kFlagActive = 0x0001;
    constexpr std::uint16_t kFlagEditable = 0x0002;
    constexpr std::uint16_t kFlagPassword = 0x0004;
    constexpr std::uint16_t kFlagMultiline = 0x0008;
    constexpr std::uint16_t kFlagAnchorPoint = 0x0010;
    constexpr std::uint16_t kFlagElementRect = 0x0020;
    constexpr std::uint16_t kFlagCaretRect = 0x0040;
    constexpr std::uint16_t kFlagInputMatched = 0x0080;
    constexpr std::uint16_t kFlagPaneVisible = 0x0100;
    constexpr std::uint16_t kFlagAutoShow = 0x0200;

    struct pointer_key_t {
      session_id sid;
      std::uint32_t pointer_id;
      bool operator==(const pointer_key_t &) const = default;
    };

    struct pointer_key_hash_t {
      std::size_t operator()(const pointer_key_t &key) const {
        return std::hash<session_id> {}(key.sid) ^
               (std::hash<std::uint32_t> {}(key.pointer_id) << 1);
      }
    };

    struct candidate_t {
      session_id sid {};
      std::uint64_t token {};
      cause_e cause {cause_e::unknown};
      clock_t::time_point created {};
      std::int32_t down_x {};
      std::int32_t down_y {};
      std::int32_t screen_x {};
      std::int32_t screen_y {};
      std::int32_t capture_left {};
      std::int32_t capture_top {};
      std::uint32_t capture_width {};
      std::uint32_t capture_height {};
      bool moved {false};
      bool consumed {false};
    };

    struct session_state_t {
      std::uint64_t next_token {1};
      std::uint64_t next_activation {1};
      std::uint32_t next_revision {1};
    };

    struct active_context_t {
      std::uint64_t activation_id {};
      candidate_t candidate;
    };

    void put_u16(payload_t &out, std::size_t offset, std::uint16_t value) {
      out[offset] = static_cast<std::uint8_t>(value);
      out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }

    void put_u32(payload_t &out, std::size_t offset, std::uint32_t value) {
      for (unsigned i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    void put_i32(payload_t &out, std::size_t offset, std::int32_t value) {
      put_u32(out, offset, static_cast<std::uint32_t>(value));
    }

    void put_u64(payload_t &out, std::size_t offset, std::uint64_t value) {
      for (unsigned i = 0; i < 8; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }

    screen_rect_t to_capture_rect(const screen_rect_t &rect, const candidate_t &candidate);

    payload_t encode_payload(const candidate_t &candidate, const observation_t &observation,
                             std::uint16_t flags, std::uint32_t revision,
                             std::uint64_t activation_id) {
      payload_t payload(kWireSize, 0);
      payload[0] = kWireVersion;
      payload[1] = static_cast<std::uint8_t>(kWireSize);
      put_u16(payload, 2, flags);
      put_u32(payload, 4, revision);
      put_u64(payload, 8, activation_id);
      put_u64(payload, 16, candidate.token);
      payload[24] = static_cast<std::uint8_t>(observation.source);
      payload[25] = static_cast<std::uint8_t>(candidate.cause);
      put_i32(payload, 28, candidate.screen_x - candidate.capture_left);
      put_i32(payload, 32, candidate.screen_y - candidate.capture_top);
      if (observation.element_rect) {
        const auto rect = to_capture_rect(*observation.element_rect, candidate);
        put_i32(payload, 36, rect.left); put_i32(payload, 40, rect.top);
        put_i32(payload, 44, rect.right); put_i32(payload, 48, rect.bottom);
      }
      if (observation.caret_rect) {
        const auto rect = to_capture_rect(*observation.caret_rect, candidate);
        put_i32(payload, 52, rect.left); put_i32(payload, 56, rect.top);
        put_i32(payload, 60, rect.right); put_i32(payload, 64, rect.bottom);
      }
      put_u32(payload, 68, candidate.capture_width);
      put_u32(payload, 72, candidate.capture_height);
      return payload;
    }

    bool contains_with_slop(const screen_rect_t &rect, std::int32_t x, std::int32_t y) {
      return x >= rect.left - kHitSlopPx && x <= rect.right + kHitSlopPx &&
             y >= rect.top - kHitSlopPx && y <= rect.bottom + kHitSlopPx;
    }

    screen_rect_t to_capture_rect(const screen_rect_t &rect, const candidate_t &candidate) {
      return {
        rect.left - candidate.capture_left,
        rect.top - candidate.capture_top,
        rect.right - candidate.capture_left,
        rect.bottom - candidate.capture_top,
      };
    }
  }  // namespace

  struct bridge_t::impl_t {
    mutable std::mutex mu;
    std::unordered_map<session_id, session_state_t> sessions;
    std::unordered_map<pointer_key_t, candidate_t, pointer_key_hash_t> active_touches;
    std::unordered_map<session_id, candidate_t> active_mice;
    std::deque<candidate_t> recent;
    std::deque<outbound_msg_t> outbox;
    std::unordered_map<session_id, active_context_t> active_contexts;
    std::optional<observation_t> current_uia;
    clock_t::time_point last_gui_alive {};
    bool input_pane {false};
    bool uia {false};
  };

  bridge_t &bridge_t::instance() {
    static bridge_t bridge;
    return bridge;
  }

  bridge_t::bridge_t(): _impl(std::make_unique<impl_t>()) {}
  bridge_t::~bridge_t() = default;

  void bridge_t::session_started(session_id sid) {
    std::lock_guard lock(_impl->mu);
    _impl->sessions.try_emplace(sid);
  }

  void bridge_t::session_stopped(session_id sid) {
    std::lock_guard lock(_impl->mu);
    _impl->sessions.erase(sid);
    std::erase_if(_impl->active_touches, [sid](const auto &entry) { return entry.first.sid == sid; });
    _impl->active_mice.erase(sid);
    _impl->active_contexts.erase(sid);
    std::erase_if(_impl->recent, [sid](const candidate_t &candidate) { return candidate.sid == sid; });
    std::erase_if(_impl->outbox, [sid](const outbound_msg_t &msg) { return msg.target == sid; });
  }

  void bridge_t::record_touch(session_id sid, std::uint8_t event_type, std::uint32_t pointer_id,
                              std::int32_t screen_x, std::int32_t screen_y,
                              std::int32_t capture_left, std::int32_t capture_top,
                              std::uint32_t capture_width, std::uint32_t capture_height) {
    // Moonlight touch event values: hover/hover-leave/down/up/move/cancel/cancel-all.
    constexpr std::uint8_t kDown = 1;
    constexpr std::uint8_t kUp = 2;
    constexpr std::uint8_t kMove = 3;
    constexpr std::uint8_t kCancel = 4;
    constexpr std::uint8_t kCancelAll = 7;
    std::lock_guard lock(_impl->mu);
    const auto now = clock_t::now();
    const pointer_key_t key {sid, pointer_id};
    if (event_type == kCancelAll) {
      std::erase_if(_impl->active_touches, [sid](const auto &entry) { return entry.first.sid == sid; });
      return;
    }
    if (event_type == kDown) {
      auto session = _impl->sessions.find(sid);
      if (session == _impl->sessions.end()) return;
      std::erase_if(_impl->active_touches, [now](const auto &entry) {
        return now - entry.second.created > kActiveTouchLifetime;
      });
      // A repeated down for the same pointer must not move its original hit
      // candidate or allocate another token.
      if (_impl->active_touches.find(key) != _impl->active_touches.end()) return;
      const auto session_touch_count = std::count_if(
        _impl->active_touches.begin(), _impl->active_touches.end(),
        [sid](const auto &entry) { return entry.first.sid == sid; });
      if (session_touch_count >= kMaxActiveTouchesPerSession) return;
      _impl->active_touches.emplace(key, candidate_t {
        sid, session->second.next_token++, cause_e::remote_touch, now,
        screen_x, screen_y, screen_x, screen_y, capture_left, capture_top,
        capture_width, capture_height, false, false,
      });
      if (_impl->current_uia && _impl->uia &&
          now - _impl->last_gui_alive < kGuiAliveWindow) {
        correlate_locked(*_impl->current_uia);
      }
      return;
    }
    auto it = _impl->active_touches.find(key);
    if (it == _impl->active_touches.end()) return;
    auto &candidate = it->second;
    candidate.screen_x = screen_x;
    candidate.screen_y = screen_y;
    const auto dx = static_cast<std::int64_t>(screen_x) - candidate.down_x;
    const auto dy = static_cast<std::int64_t>(screen_y) - candidate.down_y;
    candidate.moved |= dx * dx + dy * dy > kDragSlopSquared;
    if (event_type == kUp) {
      if (!candidate.moved && !candidate.consumed) {
        _impl->recent.push_back(candidate);
        if (_impl->recent.size() > kMaxRecentCandidates) _impl->recent.pop_front();
      }
      _impl->active_touches.erase(it);
    }
    else if (event_type == kCancel) {
      _impl->active_touches.erase(it);
    }
    else if (event_type != kMove) {
      _impl->active_touches.erase(it);
    }
  }

  void bridge_t::record_mouse_button(session_id sid, bool release,
                                     std::int32_t screen_x, std::int32_t screen_y,
                                     std::int32_t capture_left, std::int32_t capture_top,
                                     std::uint32_t capture_width, std::uint32_t capture_height) {
    std::lock_guard lock(_impl->mu);
    if (!release) {
      auto session = _impl->sessions.find(sid);
      if (session == _impl->sessions.end()) return;
      _impl->active_mice[sid] = candidate_t {
        sid, session->second.next_token++, cause_e::remote_mouse, clock_t::now(),
        screen_x, screen_y, screen_x, screen_y, capture_left, capture_top,
        capture_width, capture_height, false, false,
      };
      if (_impl->current_uia && _impl->uia &&
          clock_t::now() - _impl->last_gui_alive < kGuiAliveWindow) {
        correlate_locked(*_impl->current_uia);
      }
    }
    else if (auto it = _impl->active_mice.find(sid); it != _impl->active_mice.end()) {
      auto &candidate = it->second;
      candidate.screen_x = screen_x;
      candidate.screen_y = screen_y;
      const auto dx = static_cast<std::int64_t>(screen_x) - candidate.down_x;
      const auto dy = static_cast<std::int64_t>(screen_y) - candidate.down_y;
      candidate.moved |= dx * dx + dy * dy > kDragSlopSquared;
      if (!candidate.moved && !candidate.consumed) {
        _impl->recent.push_back(candidate);
        if (_impl->recent.size() > kMaxRecentCandidates) _impl->recent.pop_front();
      }
      _impl->active_mice.erase(it);
    }
  }

  bool bridge_t::observe(const observation_t &observation) {
    std::lock_guard lock(_impl->mu);
    if (observation.source != source_e::input_pane && observation.source != source_e::uia) {
      return false;
    }
    // InputPane is an edge-triggered signal and must never be replayed. UIA is
    // level-triggered: cache its latest state so a click into an already-focused
    // editor can still be correlated without polling the HTTP endpoint.
    if (observation.source == source_e::uia) {
      _impl->current_uia = observation;
    }
    return correlate_locked(observation);
  }

  bool bridge_t::correlate_locked(const observation_t &observation) {
    const auto now = clock_t::now();
    while (!_impl->recent.empty() && now - _impl->recent.front().created > kMatchWindow) {
      _impl->recent.pop_front();
    }

    const bool is_uia_deactivation = observation.source == source_e::uia &&
                                     (!observation.active || !observation.editable);
    candidate_t *match = nullptr;
    std::optional<session_id> matched_session;
    bool ambiguous_session = false;
    auto eligible = [&](candidate_t &candidate) {
      if (candidate.consumed || candidate.moved || now - candidate.created > kMatchWindow ||
          _impl->sessions.find(candidate.sid) == _impl->sessions.end()) return false;
      if (observation.source == source_e::input_pane &&
          (candidate.cause != cause_e::remote_touch || !observation.pane_visible || !observation.auto_show)) return false;
      if (observation.source == source_e::uia) {
        if (is_uia_deactivation) {
          return _impl->active_contexts.find(candidate.sid) != _impl->active_contexts.end();
        }
        if (!observation.active || !observation.editable || !observation.element_rect ||
            !contains_with_slop(*observation.element_rect, candidate.screen_x, candidate.screen_y)) return false;
      }
      return true;
    };
    auto consider = [&](candidate_t &candidate) {
      if (!eligible(candidate)) return;
      if (matched_session && *matched_session != candidate.sid) {
        ambiguous_session = true;
        return;
      }
      matched_session = candidate.sid;
      if (!match || candidate.created > match->created) match = &candidate;
    };
    for (auto &[_, candidate] : _impl->active_touches) {
      consider(candidate);
    }
    for (auto &[_, candidate] : _impl->active_mice) {
      consider(candidate);
    }
    for (auto &candidate : _impl->recent) {
      consider(candidate);
    }
    if (!match || ambiguous_session) return false;

    // A transient UIA focus gap may be observed while focus moves between two
    // editors. Keep the candidate available after a deactivation so the final
    // editable observation from the same click can establish a new activation.
    if (!is_uia_deactivation) match->consumed = true;
    std::uint16_t flags = kFlagAnchorPoint | kFlagInputMatched;
    if (observation.active || observation.source == source_e::input_pane) flags |= kFlagActive;
    if (observation.editable || observation.source == source_e::input_pane) flags |= kFlagEditable;
    if (observation.password) flags |= kFlagPassword;
    if (observation.multiline) flags |= kFlagMultiline;
    if (observation.pane_visible) flags |= kFlagPaneVisible;
    if (observation.auto_show) flags |= kFlagAutoShow;
    if (observation.element_rect && observation.element_rect->valid()) flags |= kFlagElementRect;
    if (observation.caret_rect && observation.caret_rect->valid()) flags |= kFlagCaretRect;

    std::uint64_t activation_id;
    if (is_uia_deactivation) {
      auto active = _impl->active_contexts.find(match->sid);
      if (active == _impl->active_contexts.end()) return false;
      activation_id = active->second.activation_id;
      _impl->active_contexts.erase(active);
    }
    else {
      auto &session = _impl->sessions.at(match->sid);
      activation_id = session.next_activation++;
      // Windows has one foreground keyboard focus. Before transferring it to
      // another streaming session, explicitly clear the previous client's
      // context using the candidate that established that trusted activation.
      observation_t deactivation;
      deactivation.source = source_e::uia;
      for (auto active = _impl->active_contexts.begin(); active != _impl->active_contexts.end();) {
        if (active->first == match->sid) {
          ++active;
          continue;
        }
        auto old_session = _impl->sessions.find(active->first);
        if (old_session != _impl->sessions.end()) {
          const auto close_flags = static_cast<std::uint16_t>(kFlagAnchorPoint | kFlagInputMatched);
          _impl->outbox.push_back({
            active->first,
            encode_payload(active->second.candidate, deactivation, close_flags,
                           old_session->second.next_revision++, active->second.activation_id),
          });
        }
        active = _impl->active_contexts.erase(active);
      }
      _impl->active_contexts[match->sid] = active_context_t {activation_id, *match};
    }

    auto &session = _impl->sessions.at(match->sid);
    _impl->outbox.push_back({
      match->sid,
      encode_payload(*match, observation, flags, session.next_revision++, activation_id),
    });
    return true;
  }

  void bridge_t::drain_outbound(std::deque<outbound_msg_t> &out) {
    std::lock_guard lock(_impl->mu);
    out.insert(out.end(), std::make_move_iterator(_impl->outbox.begin()),
               std::make_move_iterator(_impl->outbox.end()));
    _impl->outbox.clear();
  }

  void bridge_t::notify_gui_alive(bool input_pane, bool uia) {
    std::lock_guard lock(_impl->mu);
    _impl->last_gui_alive = clock_t::now();
    _impl->input_pane = input_pane;
    _impl->uia = uia;
  }

  bool bridge_t::gui_alive() const {
    std::lock_guard lock(_impl->mu);
    return (_impl->input_pane || _impl->uia) &&
           clock_t::now() - _impl->last_gui_alive < kGuiAliveWindow;
  }

  bool bridge_t::input_pane_available() const {
    std::lock_guard lock(_impl->mu);
    return _impl->input_pane && clock_t::now() - _impl->last_gui_alive < kGuiAliveWindow;
  }

  bool bridge_t::uia_available() const {
    std::lock_guard lock(_impl->mu);
    return _impl->uia && clock_t::now() - _impl->last_gui_alive < kGuiAliveWindow;
  }
}  // namespace text_context
