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

  /// Why the clipboard capability is, or is not, advertised to a client when its session is set up. The words are the ones that leave
  /// the computer as statistics (stream_end `clip_offer`), so a name must stay a fixed lower-case word.
  enum class offer_e {
    offered,  ///< advertised: the setting is on and the Panel agent is subscribed and alive
    off_config,  ///< `clipboard_sync` is off in the configuration
    no_agent,  ///< nobody is subscribed to the clipboard stream (the Panel agent is not running or has not connected yet)
    agent_stale,  ///< subscribed, but the last sign of life is older than the alive window (the agent's heartbeat stopped)
    unknown,  ///< no capability decision was made for the session
  };
  const char *offer_name(offer_e offer);

  /// What the bridge saw of the clipboard during one streaming session (no content, only counts).
  struct session_note_t {
    offer_e offer = offer_e::unknown;
    std::uint32_t inbound_forwarded = 0;  ///< client packets handed to the Panel agent
    std::uint32_t inbound_dropped = 0;  ///< client packets dropped because the setting is off or the agent is not alive
    std::uint32_t outbound_sent = 0;  ///< Panel agent items sent to the client
  };

  struct outbound_msg_t {
    session_id target;  ///< kBroadcast or a specific session id
    payload_t bytes;
  };

  class bridge_t {
  public:
    static bridge_t &instance();

    // ---- Inbound: stream.cpp -> GUI ----
    /// Returns true when a sink took the payload, false when none was registered at that moment.
    bool on_inbound(session_id sid, payload_t bytes);
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
    /// The same decision as gui_alive() with the reason when it is false; `age_ms` gets the time since the last sign of life (-1: never).
    offer_e agent_state(std::int64_t *age_ms = nullptr) const;

    // ---- Per-session counters for statistics and logs (no content) ----
    void note_offer(session_id sid, offer_e offer);
    void note_inbound(session_id sid, bool forwarded);
    void note_outbound(session_id sid);
    /// Returns what was recorded for the session and forgets it.
    session_note_t take_note(session_id sid);

    bridge_t(const bridge_t &) = delete;
    bridge_t &operator=(const bridge_t &) = delete;

  private:
    bridge_t();
    ~bridge_t();
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace clipboard_bridge
