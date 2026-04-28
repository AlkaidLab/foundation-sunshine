/**
 * @file src/clipboard_bridge.h
 * @brief Pure byte-forwarder between the streaming protocol and the user-session
 *        Rust GUI agent.
 *
 * The C++ service intentionally has zero clipboard protocol knowledge -- no
 * item-type parsing, no chunk reassembly, no PNG/UTF handling. The Rust GUI
 * does all of that.
 *
 * Wiring:
 *
 *   client -> stream.cpp::IDX_CLIPBOARD handler
 *           -> bridge.on_inbound(sid, bytes)
 *           -> inbound_sink (HTTP/SSE) -> GUI
 *
 *   GUI -> POST /api/v1/clipboard/item
 *       -> bridge.enqueue_outbound(target_sid_or_0, bytes)
 *       -> controlBroadcastThread drains via drain_outbound()
 *       -> encrypted IDX_CLIPBOARD packet -> client
 *
 * No callbacks ever cross thread boundaries holding session_t pointers; outbound
 * is strictly pull-based from the enet thread, which also owns session_t state.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace clipboard_bridge {
  using payload_t = std::vector<std::uint8_t>;
  using session_id = std::uint64_t;

  /// 0 means broadcast to every currently-active session.
  constexpr session_id kBroadcast = 0;

  /// Maximum raw clipboard payload that still fits into Sunshine's encrypted
  /// 16-bit control-frame length after adding control_header_v2, AES padding,
  /// GCM tag, and sequence number overhead.
  constexpr std::size_t kMaxPayloadBytes = 65500;

  using inbound_sink_fn = std::function<void(session_id, const payload_t &)>;

  struct outbound_msg_t {
    session_id target;  ///< kBroadcast or a specific session id
    payload_t bytes;
  };

  class bridge_t {
  public:
    static bridge_t &instance();

    // ---- Inbound: stream.cpp -> GUI ----
    void on_inbound(session_id sid, payload_t bytes);
    void set_inbound_sink(inbound_sink_fn cb);

    // ---- Outbound: GUI -> stream sessions ----
    /// Enqueue a payload to be sent on the next controlBroadcastThread tick.
    /// `target` may be `kBroadcast` to fan out to every session.
    void enqueue_outbound(session_id target, payload_t bytes);

    /// Drain all queued outbound messages into `out`. Called by
    /// controlBroadcastThread once per tick. Appends to `out`.
    void drain_outbound(std::deque<outbound_msg_t> &out);

    // ---- Session liveness (stream.cpp) ----
    void session_started(session_id sid);
    void session_stopped(session_id sid);
    /// Snapshot the count of currently active sessions. Used by HTTP layer
    /// for diagnostics; capability negotiation only uses gui_alive().
    std::size_t session_count() const;

    // ---- GUI heartbeat / capability gate ----
    void notify_gui_alive();
    /// True iff a GUI checked in within the heartbeat window AND has an
    /// inbound sink registered. Used by rtsp.cpp to decide whether to
    /// advertise `platform_caps::clipboard_*` in the SDP feature flags.
    bool gui_alive() const;

    bridge_t(const bridge_t &) = delete;
    bridge_t &operator=(const bridge_t &) = delete;

  private:
    bridge_t();
    ~bridge_t();
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace clipboard_bridge
