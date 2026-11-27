/**
 * @file src/clipboard_li.cpp
 * @brief See clipboard_li.h.
 */
#include "clipboard_li.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "logging.h"

namespace clipboard_li {
  using namespace std::literals;

  namespace {
    // ---- Client LI_CLIPBOARD protocol (Limelight-internal.h @ 093098f5 pin) ----
    constexpr std::uint8_t kMsgBind = 0x01;
    constexpr std::uint8_t kMsgUnbind = 0x02;
    constexpr std::uint8_t kMsgSnapshotRequest = 0x03;
    constexpr std::uint8_t kMsgItemStart = 0x04;
    constexpr std::uint8_t kMsgItemChunk = 0x05;
    constexpr std::uint8_t kMsgItemEnd = 0x06;
    constexpr std::uint8_t kMsgItemCancel = 0x07;
    constexpr std::uint8_t kTransferFlagSnapshot = 0x01;
    constexpr std::size_t kMaxChunkSize = 16384;
    // Reassembly ceiling: bigger items are dropped whole (a forged itemId
    // must not be able to exhaust service memory).
    constexpr std::size_t kMaxItemBytes = 64 * 1024;
    constexpr std::size_t kItemStartHeaderMin = 1 + 1 + 1 + 1 + 8 + 8 + 4 + 2 + 2;

    // ---- Agent wire format (foundation-panel clipboard.rs) ----
    constexpr std::uint8_t kWireVersion = 1;
    constexpr std::uint8_t kKindText = 1;
    constexpr std::uint8_t kKindPng = 2;
    constexpr std::size_t kAgentFrameHeader = 10;

    // A session that stops mid-item keeps its state at most this long.
    constexpr auto kItemStall = std::chrono::seconds(10);

    std::atomic<std::uint64_t> g_outbound_item_counter { 1 };

    std::uint16_t
    rd16(const bytes_t &b, std::size_t at) {
      return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
    }

    std::uint32_t
    rd32(const bytes_t &b, std::size_t at) {
      return static_cast<std::uint32_t>(b[at]) |
             (static_cast<std::uint32_t>(b[at + 1]) << 8) |
             (static_cast<std::uint32_t>(b[at + 2]) << 16) |
             (static_cast<std::uint32_t>(b[at + 3]) << 24);
    }

    std::uint64_t
    rd64(const bytes_t &b, std::size_t at) {
      return static_cast<std::uint64_t>(rd32(b, at)) | (static_cast<std::uint64_t>(rd32(b, at + 4)) << 32);
    }

    void
    put16(bytes_t &out, std::uint16_t v) {
      out.push_back(v & 0xFF);
      out.push_back((v >> 8) & 0xFF);
    }

    void
    put32(bytes_t &out, std::uint32_t v) {
      put16(out, v & 0xFFFF);
      put16(out, (v >> 16) & 0xFFFF);
    }

    void
    put64(bytes_t &out, std::uint64_t v) {
      put32(out, v & 0xFFFFFFFFu);
      put32(out, (v >> 32) & 0xFFFFFFFFu);
    }
  }  // namespace

  bytes_t
  make_agent_frame(std::uint8_t kind, std::uint32_t token, const bytes_t &payload) {
    bytes_t out;
    out.reserve(kAgentFrameHeader + payload.size());
    out.push_back(kWireVersion);
    out.push_back(kind);
    put32(out, token);
    put32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
  }

  bool
  parse_agent_frame(const bytes_t &bytes, std::uint8_t &kind, std::uint32_t &token, bytes_t &payload) {
    if (bytes.size() < kAgentFrameHeader || bytes[0] != kWireVersion) {
      return false;
    }
    kind = bytes[1];
    if (kind < kKindText || kind > 4) {
      return false;
    }
    token = rd32(bytes, 2);
    const auto len = rd32(bytes, 6);
    if (len > bytes.size() - kAgentFrameHeader) {
      return false;
    }
    payload.assign(bytes.begin() + kAgentFrameHeader, bytes.begin() + kAgentFrameHeader + len);
    return true;
  }

  std::vector<bytes_t>
  outbound_packets(std::uint8_t kind, const bytes_t &payload) {
    const std::uint8_t item_type = (kind == kKindText) ? kItemTypeText : kItemTypeImage;
    const char *mime = (kind == kKindText) ? "text/plain;charset=utf-8" : "image/png";
    if (kind != kKindText && kind != kKindPng) {
      // Blob ref / file offer: no matching LI consumer in the client yet.
      BOOST_LOG(info) << "clipboard_li: outbound kind=" << static_cast<unsigned>(kind)
                      << " (" << payload.size() << " bytes) has no LI counterpart; dropped"sv;
      return {};
    }

    const std::uint64_t item_id = g_outbound_item_counter.fetch_add(1, std::memory_order_relaxed);
    const auto total = static_cast<std::uint32_t>(payload.size());

    bytes_t start;
    start.push_back(kMsgItemStart);
    start.push_back(0);  // transferFlags: not a snapshot
    start.push_back(item_type);
    start.push_back(0);  // reserved
    put64(start, item_id);
    put64(start, 0);  // contentHash: the client recomputes and suppresses echo on its side
    put32(start, total);
    const std::uint16_t mime_len = static_cast<std::uint16_t>(std::strlen(mime));
    put16(start, mime_len);
    put16(start, 0);  // name length
    start.insert(start.end(), mime, mime + mime_len);

    std::vector<bytes_t> packets;
    packets.push_back(std::move(start));

    for (std::size_t offset = 0; offset < payload.size(); offset += kMaxChunkSize) {
      const auto chunk_len = std::min<std::size_t>(kMaxChunkSize, payload.size() - offset);
      bytes_t chunk;
      chunk.push_back(kMsgItemChunk);
      chunk.push_back(0);  // reserved
      put16(chunk, static_cast<std::uint16_t>(chunk_len));
      put64(chunk, item_id);
      put32(chunk, static_cast<std::uint32_t>(offset));
      chunk.insert(chunk.end(), payload.begin() + offset, payload.begin() + offset + chunk_len);
      packets.push_back(std::move(chunk));
    }

    bytes_t end;
    end.push_back(kMsgItemEnd);
    put64(end, item_id);
    packets.push_back(std::move(end));

    BOOST_LOG(info) << "clipboard_li: outbound item " << item_id << " kind=" << static_cast<unsigned>(kind)
                    << " bytes=" << total << " packets=" << packets.size();
    return packets;
  }

  struct inbound_router_t::state_t {
    struct pending_t {
      std::uint8_t type = 0;
      bool snapshot = false;
      std::string mime;
      std::string name;
      std::vector<std::uint8_t> data;  // sized to totalLength on ITEM_START
      std::uint64_t item_id = 0;
      std::uint64_t total = 0;
      std::uint64_t received = 0;
    };

    pending_t active;
    bool has_active = false;
    std::chrono::steady_clock::time_point last_touch {};
  };

  struct inbound_router_t::impl_t {
    std::mutex mu;
    std::unordered_map<std::uint64_t, state_t> sessions;
    std::function<std::chrono::steady_clock::time_point()> now;
  };

  inbound_router_t::~inbound_router_t() = default;

  inbound_router_t::inbound_router_t():
      inbound_router_t([] {
        return std::chrono::steady_clock::now();
      }) {}

  inbound_router_t::inbound_router_t(std::function<std::chrono::steady_clock::time_point()> now_fn):
      _impl(std::make_unique<impl_t>()) {
    _impl->now = std::move(now_fn);
  }

  bool
  inbound_router_t::feed(std::uint64_t sid, const bytes_t &payload, const item_cb &on_item) {
    std::vector<item_t> completed;
    {
      std::lock_guard<std::mutex> lk(_impl->mu);
      auto &st = _impl->sessions[sid];
      st.last_touch = _impl->now();

      const bytes_t &b = payload;
      std::size_t pos = 0;
      while (pos < payload.size()) {
        const std::uint8_t kind = payload[pos];

        auto fail = [&](const char *why) {
          BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " dropping packet: "sv << why;
          return false;
        };

        switch (kind) {
          case kMsgBind:
            if (pos + 1 > b.size()) { { const bool ok = fail("BIND truncated"); if (!ok) break; } }
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " BIND (client bound the clipboard channel)"sv;
            pos += 1;
            break;
          case kMsgUnbind:
            if (pos + 1 > b.size()) { { const bool ok = fail("UNBIND truncated"); if (!ok) break; } }
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " UNBIND"sv;
            st.has_active = false;
            st.active = {};
            pos += 1;
            break;
          case kMsgSnapshotRequest:
            if (pos + 1 > b.size()) { { const bool ok = fail("SNAPSHOT_REQUEST truncated"); if (!ok) break; } }
            // The host cannot push a snapshot back to the client today; the
            // client times out after 1.5 s and falls back to its own copy.
            // Logging this (the old code was silent) is the diagnostic hook.
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " SNAPSHOT_REQUEST (no host-side snapshot yet; client will time out)"sv;
            pos += 1;
            break;
          case kMsgItemStart: {
            if (b.size() - pos < kItemStartHeaderMin) { { const bool ok = fail("ITEM_START header truncated"); if (!ok) break; } }
            const std::uint8_t transfer_flags = b[pos + 1];
            const std::uint8_t item_type = b[pos + 2];
            const std::uint64_t item_id = rd64(b, pos + 4);
            const std::uint32_t total_length = rd32(b, pos + 4 + 8 + 8);
            const std::uint16_t mime_len = rd16(b, pos + 4 + 8 + 8 + 4);
            const std::uint16_t name_len = rd16(b, pos + 4 + 8 + 8 + 4 + 2);
            const std::size_t header_size = kItemStartHeaderMin + mime_len + name_len;
            if (b.size() - pos < header_size) { { const bool ok = fail("ITEM_START mime/name truncated"); if (!ok) break; } }
            if (item_type != kItemTypeText && item_type != kItemTypeImage) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_START unsupported type "
                              << static_cast<unsigned>(item_type) << "; dropped"sv;
              pos += header_size;
              break;
            }
            if (total_length > kMaxItemBytes) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_START item " << item_id
                              << " bytes=" << total_length << " exceeds " << kMaxItemBytes << "; dropped"sv;
              pos += header_size;
              break;
            }
            if (st.has_active && st.active.item_id != item_id) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_START item " << item_id
                              << " replaces unfinished item " << st.active.item_id << " (" << st.active.received << "/" << st.active.total << " received)"sv;
            }
            st.active = {};
            st.active.item_id = item_id;
            st.active.type = item_type;
            st.active.snapshot = (transfer_flags & kTransferFlagSnapshot) != 0;
            st.active.total = total_length;
            st.active.mime.assign(b.begin() + pos + kItemStartHeaderMin, b.begin() + pos + kItemStartHeaderMin + mime_len);
            st.active.name.assign(b.begin() + pos + kItemStartHeaderMin + mime_len, b.begin() + pos + header_size);
            st.active.data.assign(total_length, 0);
            st.has_active = true;
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_START item " << item_id
                            << " type=" << static_cast<unsigned>(item_type) << " bytes=" << total_length
                            << (st.active.snapshot ? " snapshot" : "");
            pos += header_size;
            break;
          }
          case kMsgItemChunk: {
            const std::size_t header_size = 1 + 1 + 2 + 8 + 4;
            if (b.size() - pos < header_size) { { const bool ok = fail("ITEM_CHUNK header truncated"); if (!ok) break; } }
            const std::uint16_t chunk_len = rd16(b, pos + 2);
            const std::uint64_t item_id = rd64(b, pos + 4);
            const std::uint32_t chunk_offset = rd32(b, pos + 4 + 8);
            if (!st.has_active || st.active.item_id != item_id) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_CHUNK for unknown item " << item_id << "; dropped"sv;
              pos += header_size + chunk_len;
              break;
            }
            if (b.size() - pos < header_size + chunk_len) { { const bool ok = fail("ITEM_CHUNK payload truncated"); if (!ok) break; } }
            if (chunk_offset > st.active.total || chunk_len > st.active.total - chunk_offset) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_CHUNK out of bounds offset=" << chunk_offset
                              << " len=" << chunk_len << " total=" << st.active.total << "; item dropped"sv;
              st.has_active = false;
              st.active = {};
              pos += header_size + chunk_len;
              break;
            }
            std::copy_n(b.begin() + pos + header_size, chunk_len, st.active.data.begin() + chunk_offset);
            st.active.received = std::max(st.active.received, static_cast<std::uint64_t>(chunk_offset) + chunk_len);
            pos += header_size + chunk_len;
            break;
          }
          case kMsgItemEnd: {
            if (b.size() - pos < 1 + 8) { { const bool ok = fail("ITEM_END truncated"); if (!ok) break; } }
            const std::uint64_t item_id = rd64(b, pos + 1);
            if (!st.has_active || st.active.item_id != item_id) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_END for unknown item " << item_id << "; dropped"sv;
              pos += 1 + 8;
              break;
            }
            if (st.active.received != st.active.total) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_END item " << item_id
                              << " incomplete (" << st.active.received << "/" << st.active.total << "); dropped"sv;
              st.has_active = false;
              st.active = {};
              pos += 1 + 8;
              break;
            }
            item_t item;
            item.item_id = item_id;
            item.type = st.active.type;
            item.snapshot = st.active.snapshot;
            item.mime = std::move(st.active.mime);
            item.name = std::move(st.active.name);
            item.data = std::move(st.active.data);
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " item " << item_id << " complete"
                            << " type=" << static_cast<unsigned>(item.type) << " bytes=" << item.data.size()
                            << "; forwarded to agent";
            st.has_active = false;
            st.active = {};
            completed.push_back(std::move(item));
            pos += 1 + 8;
            break;
          }
          case kMsgItemCancel: {
            if (b.size() - pos < 1 + 8) { { const bool ok = fail("ITEM_CANCEL truncated"); if (!ok) break; } }
            const std::uint64_t item_id = rd64(b, pos + 1);
            if (st.has_active && (item_id == 0 || st.active.item_id == item_id)) {
              BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " ITEM_CANCEL item " << item_id
                              << "; pending item dropped"sv;
              st.has_active = false;
              st.active = {};
            }
            pos += 1 + 8;
            break;
          }
          default:
            BOOST_LOG(info) << "clipboard_li: inbound session " << sid << " unknown message kind "
                            << static_cast<unsigned>(kind) << " (" << b.size() - pos << " bytes); dropped"sv;
            pos = b.size();
            break;
        }

      }

      // Sweep sessions that stopped mid-item.
      const auto now = st.last_touch;
      for (auto it = _impl->sessions.begin(); it != _impl->sessions.end();) {
        if (&it->second == &st || now - it->second.last_touch < kItemStall) {
          ++it;
          continue;
        }
        if (it->second.has_active) {
          BOOST_LOG(info) << "clipboard_li: session " << it->first << " stalled with item " << it->second.active.item_id
                          << " incomplete (" << it->second.active.received << "/" << it->second.active.total << "); dropped"sv;
        }
        it = _impl->sessions.erase(it);
      }
    }

    for (auto &item : completed) {
      on_item(item);
    }
    return !completed.empty();
  }

  void
  inbound_router_t::reset(std::uint64_t sid) {
    std::lock_guard<std::mutex> lk(_impl->mu);
    auto it = _impl->sessions.find(sid);
    if (it != _impl->sessions.end() && it->second.has_active) {
      BOOST_LOG(info) << "clipboard_li: session " << sid << " stopped with item " << it->second.active.item_id
                      << " incomplete (" << it->second.active.received << "/" << it->second.active.total << "); dropped"sv;
    }
    _impl->sessions.erase(it);
  }

  std::size_t
  inbound_router_t::session_count() const {
    std::lock_guard<std::mutex> lk(_impl->mu);
    return _impl->sessions.size();
  }

  inbound_router_t &
  inbound_instance() {
    static inbound_router_t inst;
    return inst;
  }
}  // namespace clipboard_li
