/**
 * @file src/clipboard_bridge.cpp
 * @brief See clipboard_bridge.h.
 */
#include "clipboard_bridge.h"

#include <chrono>
#include <mutex>
#include <unordered_set>

namespace clipboard_bridge {
  namespace {
    constexpr auto kGuiAliveWindow = std::chrono::seconds(30);
  }  // namespace

  struct bridge_t::impl_t {
    mutable std::mutex mu;
    inbound_sink_fn inbound_sink;
    std::deque<outbound_msg_t> outbox;
    std::unordered_set<session_id> active_sessions;
    std::chrono::steady_clock::time_point last_gui_alive {};
  };

  bridge_t &
  bridge_t::instance() {
    static bridge_t inst;
    return inst;
  }

  bridge_t::bridge_t():
      _impl(std::make_unique<impl_t>()) {}

  bridge_t::~bridge_t() = default;

  void
  bridge_t::on_inbound(session_id sid, payload_t bytes) {
    inbound_sink_fn cb;
    {
      std::lock_guard<std::mutex> lk(_impl->mu);
      cb = _impl->inbound_sink;
    }
    if (cb) {
      cb(sid, bytes);
    }
  }

  void
  bridge_t::set_inbound_sink(inbound_sink_fn cb) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->inbound_sink = std::move(cb);
  }

  void
  bridge_t::enqueue_outbound(session_id target, payload_t bytes) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->outbox.push_back({target, std::move(bytes)});
  }

  void
  bridge_t::drain_outbound(std::deque<outbound_msg_t> &out) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    if (_impl->outbox.empty()) {
      return;
    }
    out.insert(out.end(),
               std::make_move_iterator(_impl->outbox.begin()),
               std::make_move_iterator(_impl->outbox.end()));
    _impl->outbox.clear();
  }

  void
  bridge_t::session_started(session_id sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->active_sessions.insert(sid);
  }

  void
  bridge_t::session_stopped(session_id sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->active_sessions.erase(sid);
  }

  std::size_t
  bridge_t::session_count() const {
    std::lock_guard<std::mutex> lk(_impl->mu);
    return _impl->active_sessions.size();
  }

  void
  bridge_t::notify_gui_alive() {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->last_gui_alive = std::chrono::steady_clock::now();
  }

  bool
  bridge_t::gui_alive() const {
    std::lock_guard<std::mutex> lk(_impl->mu);
    if (!_impl->inbound_sink) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    return (now - _impl->last_gui_alive) < kGuiAliveWindow;
  }
}  // namespace clipboard_bridge
