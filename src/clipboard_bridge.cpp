/**
 * @file src/clipboard_bridge.cpp
 * @brief See clipboard_bridge.h.
 */
#include "clipboard_bridge.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
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
    std::unordered_map<session_id, session_note_t> notes;
  };

  const char *
  offer_name(offer_e offer) {
    switch (offer) {
      case offer_e::offered:
        return "offered";
      case offer_e::off_config:
        return "off_config";
      case offer_e::no_agent:
        return "no_agent";
      case offer_e::agent_stale:
        return "agent_stale";
      case offer_e::unknown:
      default:
        return "unknown";
    }
  }

  bridge_t &
  bridge_t::instance() {
    static bridge_t inst;
    return inst;
  }

  bridge_t::bridge_t():
      _impl(std::make_unique<impl_t>()) {}

  bridge_t::~bridge_t() = default;

  bool
  bridge_t::on_inbound(session_id sid, payload_t bytes) {
    inbound_sink_fn cb;
    {
      std::lock_guard<std::mutex> lk(_impl->mu);
      cb = _impl->inbound_sink;
    }
    if (!cb) {
      return false;
    }
    cb(sid, bytes);
    return true;
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
    return agent_state() == offer_e::offered;
  }

  offer_e
  bridge_t::agent_state(std::int64_t *age_ms) const {
    std::lock_guard<std::mutex> lk(_impl->mu);
    const bool ever_alive = _impl->last_gui_alive != std::chrono::steady_clock::time_point {};
    const auto age = ever_alive ?
                       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _impl->last_gui_alive).count() :
                       static_cast<std::int64_t>(-1);
    if (age_ms) {
      *age_ms = age;
    }
    if (!_impl->inbound_sink) {
      return offer_e::no_agent;
    }
    return ever_alive && age < std::chrono::duration_cast<std::chrono::milliseconds>(kGuiAliveWindow).count() ? offer_e::offered : offer_e::agent_stale;
  }

  void
  bridge_t::note_offer(session_id sid, offer_e offer) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    // a client that sets up the same launch twice keeps the latest decision; the table is bounded so a session that never ends cannot grow it
    if (_impl->notes.size() > 64 && _impl->notes.find(sid) == _impl->notes.end()) {
      // drop a record whose session is not streaming (a launch that never started); only when every record belongs to a live stream is the oldest one given up
      auto victim = _impl->notes.begin();
      for (auto it = _impl->notes.begin(); it != _impl->notes.end(); ++it) {
        if (_impl->active_sessions.find(it->first) == _impl->active_sessions.end()) {
          victim = it;
          break;
        }
      }
      _impl->notes.erase(victim);
    }
    _impl->notes[sid].offer = offer;
  }

  void
  bridge_t::note_inbound(session_id sid, bool forwarded) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    const auto found = _impl->notes.find(sid);
    if (found == _impl->notes.end()) {
      return;
    }
    auto &count = forwarded ? found->second.inbound_forwarded : found->second.inbound_dropped;
    if (count < UINT32_MAX) {
      ++count;
    }
  }

  void
  bridge_t::note_outbound(session_id sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    const auto found = _impl->notes.find(sid);
    if (found != _impl->notes.end() && found->second.outbound_sent < UINT32_MAX) {
      ++found->second.outbound_sent;
    }
  }

  session_note_t
  bridge_t::take_note(session_id sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    const auto found = _impl->notes.find(sid);
    if (found == _impl->notes.end()) {
      return {};
    }
    const auto note = found->second;
    _impl->notes.erase(found);
    return note;
  }
}  // namespace clipboard_bridge
