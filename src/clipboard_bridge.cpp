/**
 * @file src/clipboard_bridge.cpp
 * @brief See clipboard_bridge.h.
 */
#include "clipboard_bridge.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace clipboard_bridge {
  namespace {
    constexpr auto kGuiAliveWindow = std::chrono::seconds(30);
  }  // namespace

  struct bridge_t::impl_t {
    mutable std::mutex mu;
    inbound_sink_fn inbound_sink;
    std::unordered_map<session_id, session_sink_fn> sessions;
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

  std::size_t
  bridge_t::broadcast(const payload_t &bytes) {
    std::vector<session_sink_fn> sinks;
    {
      std::lock_guard<std::mutex> lk(_impl->mu);
      sinks.reserve(_impl->sessions.size());
      for (auto &kv : _impl->sessions) {
        sinks.push_back(kv.second);
      }
    }
    for (auto &s : sinks) {
      if (s) {
        s(bytes);
      }
    }
    return sinks.size();
  }

  bool
  bridge_t::send_to(session_id sid, const payload_t &bytes) {
    session_sink_fn s;
    {
      std::lock_guard<std::mutex> lk(_impl->mu);
      auto it = _impl->sessions.find(sid);
      if (it == _impl->sessions.end()) {
        return false;
      }
      s = it->second;
    }
    if (s) {
      s(bytes);
    }
    return true;
  }

  void
  bridge_t::register_session(session_id sid, session_sink_fn sink) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->sessions[sid] = std::move(sink);
  }

  void
  bridge_t::unregister_session(session_id sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->sessions.erase(sid);
  }

  void
  bridge_t::set_inbound_sink(inbound_sink_fn cb) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->inbound_sink = std::move(cb);
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
