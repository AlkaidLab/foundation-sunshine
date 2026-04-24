/**
 * @file src/clipboard_bridge.h
 * @brief Pure byte-forwarder between the streaming protocol and the user-session
 *        Rust GUI agent. The C++ service intentionally has zero clipboard
 *        protocol knowledge here -- no item-type parsing, no chunk reassembly,
 *        no PNG/UTF handling. All of that lives in the GUI.
 *
 * Inbound  (client -> host): stream.cpp receives an `IDX_CLIPBOARD` control
 *          packet, decrypts it to plaintext, and calls `on_inbound()`. The
 *          bytes are forwarded verbatim to the GUI via the registered inbound
 *          sink (HTTP/SSE).
 *
 * Outbound (host -> client): the GUI POSTs a clipboard control payload to the
 *          HTTP layer, which calls `broadcast()` (or `send_to()`). Each bound
 *          stream session re-encrypts and emits the bytes as an
 *          `IDX_CLIPBOARD` packet.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace clipboard_bridge {
  using payload_t = std::vector<std::uint8_t>;
  using session_id = std::uint64_t;

  /// Sink installed by stream.cpp on session start; emits an outbound packet
  /// to the specific remote client backing this session.
  using session_sink_fn = std::function<void(const payload_t &)>;

  /// Sink installed by the HTTP layer when a GUI subscribes; receives every
  /// inbound packet plus the originating session id.
  using inbound_sink_fn = std::function<void(session_id, const payload_t &)>;

  class bridge_t {
  public:
    static bridge_t &instance();

    // ---- Inbound: stream.cpp -> GUI ----
    void on_inbound(session_id sid, payload_t bytes);

    // ---- Outbound: GUI -> stream sessions ----
    /// Returns the number of sessions notified.
    std::size_t broadcast(const payload_t &bytes);
    /// Returns true if the session is currently registered.
    bool send_to(session_id sid, const payload_t &bytes);

    // ---- Session registration (stream.cpp) ----
    void register_session(session_id sid, session_sink_fn sink);
    void unregister_session(session_id sid);

    // ---- GUI subscription (HTTP layer) ----
    void set_inbound_sink(inbound_sink_fn cb);

    // ---- Liveness / capability ----
    void notify_gui_alive();
    /// True iff a GUI checked in within the heartbeat window AND is currently
    /// subscribed. Used by stream.cpp to decide capability negotiation.
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
