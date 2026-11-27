/**
 * @file src/clipboard_li.h
 * @brief Translation between the Moonlight client's LI_CLIPBOARD multi-packet
 *        protocol (the 0x5508 control payload) and the Panel agent's
 *        single-frame wire format.
 *
 * Contract sources (deliberate fork, pinned for review):
 *   Client side: moonlight-ci (AlkaidLab fork of Moonlight),
 *     moonlight-common-c `Limelight-internal.h` @ 093098f5 (the upstream pin
 *     recorded in that fork's UPSTREAM.json). LI message layout, little-endian:
 *       BIND            [0x01]
 *       UNBIND          [0x02]
 *       SNAPSHOT_REQ    [0x03]
 *       ITEM_START      [0x04][0x00][type][0x00][itemId:8][contentHash:8]
 *                        [totalLength:4][mimeLen:2][nameLen:2][mime][name]
 *       ITEM_CHUNK      [0x05][0x00][chunkLen:2][itemId:8][offset:4][data]
 *       ITEM_END        [0x06][itemId:8]
 *       ITEM_CANCEL     [0x07][itemId:8]
 *     type: 1 = text, 2 = image (png). Max chunk size 16384.
 *   Agent side: foundation-panel `clipboard.rs`, WIRE_VERSION 1:
 *       [0x01][kind 1|2|3|4][token:4 LE][len:4 LE][payload]
 *     kind: 1 = text, 2 = png, 3 = blob ref, 4 = file offer.
 *
 * The bridge (clipboard_bridge) stays a byte forwarder; everything protocol-
 * aware lives here, with a BOOST_LOG line on every translation outcome so a
 * future mis-match is grep-able instead of silent.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clipboard_li {
  using bytes_t = std::vector<std::uint8_t>;

  /// LI item types as used by the client (Limelight-internal.h @ 093098f5 pin).
  constexpr std::uint8_t kItemTypeText = 1;
  constexpr std::uint8_t kItemTypeImage = 2;

  /// Agent wire kinds (foundation-panel clipboard.rs, WIRE_VERSION 1).
  constexpr std::uint8_t kAgentKindText = 1;
  constexpr std::uint8_t kAgentKindPng = 2;

  /// One complete clipboard item reassembled from LI_CLIPBOARD packets.
  struct item_t {
    std::uint64_t item_id = 0;
    std::uint8_t type = 0;  ///< LI_CLIPBOARD_ITEM_TYPE_TEXT (1) or _IMAGE (2)
    bool snapshot = false;  ///< LI_CLIPBOARD_TRANSFER_FLAG_SNAPSHOT
    std::string mime;
    std::string name;
    std::vector<std::uint8_t> data;
  };

  /// Build the agent wire frame `[1][kind][token:4 LE][len:4 LE][payload]`.
  bytes_t make_agent_frame(std::uint8_t kind, std::uint32_t token, const bytes_t &payload);

  /// Parse the agent wire frame; false on any shape mismatch (the bytes then
  /// cannot have come from the Panel agent and are reported by the caller).
  bool parse_agent_frame(const bytes_t &bytes, std::uint8_t &kind, std::uint32_t &token, bytes_t &payload);

  /// Outbound (agent -> client): the LI_CLIPBOARD packets for one item.
  /// kind 1 (text) and 2 (png) are translated; 3 (blob ref) and 4 (file
  /// offer) return an empty list (logged at the call site) until the client
  /// grows a matching consumer. The item id is a process-wide monotonic
  /// counter, fresh per call.
  std::vector<bytes_t> outbound_packets(std::uint8_t kind, const bytes_t &payload);

  /// Per-session inbound aggregation (client -> agent): feed the raw 0x5508
  /// payloads of one streaming session; a completed item is handed to
  /// `on_item` exactly once, atomically with any chunk reassembly state.
  /// The router owns one state per session id; states idle past
  /// `kItemStall` are swept, and `reset(sid)` drops the session's state on
  /// stream stop.
  class inbound_router_t {
  public:
    using item_cb = std::function<void(item_t)>;

    inbound_router_t();
    explicit inbound_router_t(std::function<std::chrono::steady_clock::time_point()> now_fn);
    ~inbound_router_t();

    /// Feed one raw 0x5508 payload belonging to session `sid`. Returns true
    /// when at least one item was emitted; BIND/UNBIND/SNAPSHOT_REQUEST are
    /// consumed (info-logged) and return false.
    bool feed(std::uint64_t sid, const bytes_t &payload, const item_cb &on_item);

    /// Drop the session's aggregation state (session stopped).
    void reset(std::uint64_t sid);

    std::size_t session_count() const;

  private:
    struct state_t;
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };

  /// The instance the HTTP/SSE layer and stream.cpp share.
  inbound_router_t &inbound_instance();
}  // namespace clipboard_li
