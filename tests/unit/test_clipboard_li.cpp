/**
 * @file tests/unit/test_clipboard_li.cpp
 * @brief The LI_CLIPBOARD <-> agent wire-format translation: reassembly of
 *        multi-packet items, the outbound packet framing, and the wire-frame
 *        codec both sides speak.
 */
#include "src/clipboard_li.h"

#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <string>
#include <vector>

namespace {
  using namespace clipboard_li;
  using namespace std::literals;

  std::uint64_t
  rd64(const bytes_t &b, std::size_t at) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
      v = (v << 8) | b[at + i];
    }
    return v;
  }

  void
  put64(bytes_t &out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      out.push_back((v >> (8 * i)) & 0xFF);
    }
  }

  void
  put32(bytes_t &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out.push_back((v >> (8 * i)) & 0xFF);
    }
  }

  void
  put16(bytes_t &out, std::uint16_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
  }

  /// ITEM_START exactly as the client builds it (ControlStream.c).
  bytes_t
  li_item_start(std::uint64_t item_id, std::uint8_t type, std::uint32_t total,
                const std::string &mime, const std::string &name = "") {
    bytes_t p;
    p.push_back(0x04);
    p.push_back(0);
    p.push_back(type);
    p.push_back(0);
    put64(p, item_id);
    put64(p, 0);
    put32(p, total);
    put16(p, static_cast<std::uint16_t>(mime.size()));
    put16(p, static_cast<std::uint16_t>(name.size()));
    p.insert(p.end(), mime.begin(), mime.end());
    p.insert(p.end(), name.begin(), name.end());
    return p;
  }

  bytes_t
  li_item_chunk(std::uint64_t item_id, std::uint32_t offset, const std::vector<std::uint8_t> &data) {
    bytes_t p;
    p.push_back(0x05);
    p.push_back(0);
    put16(p, static_cast<std::uint16_t>(data.size()));
    put64(p, item_id);
    put32(p, offset);
    p.insert(p.end(), data.begin(), data.end());
    return p;
  }

  bytes_t
  li_item_end(std::uint64_t item_id) {
    bytes_t p;
    p.push_back(0x06);
    put64(p, item_id);
    return p;
  }

  void
  append(bytes_t &out, const bytes_t &p) {
    out.insert(out.end(), p.begin(), p.end());
  }

  struct collected_t {
    std::vector<item_t> items;
    clipboard_li::inbound_router_t::item_cb sink() {
      return [this](item_t item) {
        items.push_back(std::move(item));
      };
    }
  };
}  // namespace

TEST(ClipboardLiInbound, ATextItemRoundTrips) {
  inbound_router_t router;
  collected_t got;

  bytes_t wire;
  append(wire, li_item_start(7, 1, 5, "text/plain;charset=utf-8"));
  append(wire, li_item_chunk(7, 0, { 'h', 'e', 'l', 'l', 'o' }));
  append(wire, li_item_end(7));

  const bool emitted = router.feed(1, wire, got.sink());
  ASSERT_TRUE(emitted);
  ASSERT_EQ(got.items.size(), 1u);
  EXPECT_EQ(got.items[0].item_id, 7u);
  EXPECT_EQ(got.items[0].type, 1u);
  EXPECT_EQ(got.items[0].mime, "text/plain;charset=utf-8");
  EXPECT_EQ(got.items[0].data, (bytes_t { 'h', 'e', 'l', 'l', 'o' }));
  EXPECT_FALSE(got.items[0].snapshot);
}

TEST(ClipboardLiInbound, ChunksInAnyOrderAndAcrossPacketsReassemble) {
  inbound_router_t router;
  collected_t got;

  // start
  ASSERT_FALSE(router.feed(1, li_item_start(3, 1, 16, "text/plain"), got.sink()));
  // second chunk first (out of order)
  ASSERT_FALSE(router.feed(1, li_item_chunk(3, 8, { 8, 9, 10, 11, 12, 13, 14, 15 }), got.sink()));
  // first chunk in a separate packet
  ASSERT_FALSE(router.feed(1, li_item_chunk(3, 0, { 0, 1, 2, 3, 4, 5, 6, 7 }), got.sink()));
  // end in a separate packet
  ASSERT_TRUE(router.feed(1, li_item_end(3), got.sink()));

  ASSERT_EQ(got.items.size(), 1u);
  EXPECT_EQ(got.items[0].data.size(), 16u);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(got.items[0].data[i], i);
  }
}

TEST(ClipboardLiInbound, BindUnbindAndSnapshotAreConsumedWithoutEmitting) {
  inbound_router_t router;
  collected_t got;

  ASSERT_FALSE(router.feed(1, bytes_t { 0x01 }, got.sink()));  // BIND
  ASSERT_FALSE(router.feed(1, bytes_t { 0x03 }, got.sink()));  // SNAPSHOT_REQUEST
  ASSERT_FALSE(router.feed(1, bytes_t { 0x02 }, got.sink()));  // UNBIND
  EXPECT_TRUE(got.items.empty());
}

TEST(ClipboardLiInbound, BindAndItemStartShareOnePacket) {
  inbound_router_t router;
  collected_t got;

  bytes_t wire;
  wire.push_back(0x01);  // BIND
  append(wire, li_item_start(9, 1, 1, "text/plain"));
  append(wire, li_item_chunk(9, 0, { 'x' }));
  append(wire, li_item_end(9));

  ASSERT_TRUE(router.feed(1, wire, got.sink()));
  ASSERT_EQ(got.items.size(), 1u);
  EXPECT_EQ(got.items[0].item_id, 9u);
  EXPECT_EQ(got.items[0].data, (bytes_t { 'x' }));
}

TEST(ClipboardLiInbound, AnOversizedItemIsDroppedWhole) {
  inbound_router_t router;
  collected_t got;

  const std::uint32_t total = 64 * 1024 + 1;
  bytes_t wire;
  append(wire, li_item_start(4, 1, total, "text/plain"));
  // Even if the chunks arrive, the start already dropped the item.
  append(wire, li_item_chunk(4, 0, { 'a', 'b' }));
  append(wire, li_item_end(4));

  ASSERT_FALSE(router.feed(1, wire, got.sink()));
  EXPECT_TRUE(got.items.empty());
}

TEST(ClipboardLiInbound, AnUnfinishedItemBlocksNothingAndEndWithoutChunksFails) {
  inbound_router_t router;
  collected_t got;

  bytes_t wire;
  append(wire, li_item_start(5, 1, 8, "text/plain"));
  append(wire, li_item_chunk(5, 0, { 1, 2, 3, 4 }));  // only half
  append(wire, li_item_end(5));  // incomplete: received 4 of 8

  ASSERT_FALSE(router.feed(1, wire, got.sink()));
  EXPECT_TRUE(got.items.empty());

  // The state was dropped, so a repeat END for the same id is "unknown item".
  ASSERT_FALSE(router.feed(1, li_item_end(5), got.sink()));
  EXPECT_TRUE(got.items.empty());
}

TEST(ClipboardLiInbound, ItemIdReuseAfterCompletionStartsFresh) {
  inbound_router_t router;
  collected_t got;

  bytes_t first;
  append(first, li_item_start(11, 1, 1, "text/plain"));
  append(first, li_item_chunk(11, 0, { 'a' }));
  append(first, li_item_end(11));
  ASSERT_TRUE(router.feed(1, first, got.sink()));

  bytes_t second;
  append(second, li_item_start(11, 1, 2, "text/plain"));
  append(second, li_item_chunk(11, 0, { 'b', 'c' }));
  append(second, li_item_end(11));
  ASSERT_TRUE(router.feed(1, second, got.sink()));

  ASSERT_EQ(got.items.size(), 2u);
  EXPECT_EQ(got.items[0].data, (bytes_t { 'a' }));
  EXPECT_EQ(got.items[1].data, (bytes_t { 'b', 'c' }));
}

TEST(ClipboardLiInbound, ResetDropsPendingStateAndStallSweepsIdleSessions) {
  auto now = std::chrono::steady_clock::now();
  inbound_router_t router([&]() {
    return now;
  });
  collected_t got;

  // Pending item on session 2; the session then "stops" (reset).
  router.feed(2, li_item_start(1, 1, 8, "text/plain"), got.sink());
  router.reset(2);
  EXPECT_EQ(router.session_count(), 0u);

  // Session 3 goes idle with a pending item; time passes; the next feed
  // sweeps it.
  router.feed(3, li_item_start(2, 1, 8, "text/plain"), got.sink());
  EXPECT_EQ(router.session_count(), 1u);
  now += std::chrono::seconds(11);
  router.feed(4, bytes_t { 0x01 }, got.sink());
  EXPECT_EQ(router.session_count(), 1u);  // only session 4 remains
  EXPECT_TRUE(got.items.empty());
}

TEST(ClipboardLiOutbound, ATextItemFramesIntoStartChunksAndEnd) {
  const bytes_t content(5, 't');
  const auto packets = outbound_packets(kAgentKindText, content);

  ASSERT_EQ(packets.size(), 3u);  // start, one chunk, end
  EXPECT_EQ(packets[0][0], 0x04);

  const std::uint64_t id = rd64(packets[2], 1);
  EXPECT_EQ(rd64(packets[0], 4), id);
  EXPECT_EQ(packets[1][0], 0x05);
  EXPECT_EQ(rd64(packets[1], 4), id);
  EXPECT_EQ(packets[1].size(), 1 + 1 + 2 + 8 + 4 + content.size());
  EXPECT_EQ(packets[1][1 + 1 + 2 + 8 + 4], 't');
}

TEST(ClipboardLiOutbound, LargePayloadSplitsAtSixteenKChunks) {
  bytes_t content(40000);
  std::iota(content.begin(), content.end(), std::uint8_t { 0 });

  const auto packets = outbound_packets(kAgentKindPng, content);
  // start + ceil(40000/16384) chunks + end = 1 + 3 + 1
  ASSERT_EQ(packets.size(), 5u);
  for (std::size_t i = 1; i + 1 < packets.size(); ++i) {
    EXPECT_EQ(packets[i][0], 0x05);
  }
  EXPECT_EQ(packets.back()[0], 0x06);
}

TEST(ClipboardLiOutbound, BlobRefAndFileOfferHaveNoLiCounterpart) {
  EXPECT_TRUE(outbound_packets(3, { 1, 2, 3 }).empty());
  EXPECT_TRUE(outbound_packets(4, { 1, 2, 3 }).empty());
}

TEST(ClipboardLiAgentFrame, TheCodecRoundTripsAndRejectsForeignBytes) {
  const bytes_t payload = { 'c', 'l', 'i', 'p' };
  auto frame = make_agent_frame(kAgentKindText, 0, payload);
  ASSERT_EQ(frame.size(), 10 + payload.size());
  EXPECT_EQ(frame[0], 1);
  EXPECT_EQ(frame[1], 1);

  std::uint8_t kind = 0;
  std::uint32_t token = 99;
  bytes_t out;
  ASSERT_TRUE(parse_agent_frame(frame, kind, token, out));
  EXPECT_EQ(kind, 1u);
  EXPECT_EQ(token, 0u);
  EXPECT_EQ(out, payload);

  // The LI frames the client sends today must all fail to parse as agent
  // frames: that is the bug this layer fixes, and a regression of the
  // wire format would show up here.
  EXPECT_FALSE(parse_agent_frame({ 0x01 }, kind, token, out));  // 1B BIND
  EXPECT_FALSE(parse_agent_frame({ 0x03 }, kind, token, out));  // SNAPSHOT_REQUEST
  EXPECT_FALSE(parse_agent_frame(li_item_start(1, 1, 1, "text/plain"), kind, token, out));
  // A frame claiming more payload than it carries.
  bytes_t short_frame = frame;
  short_frame.pop_back();
  EXPECT_FALSE(parse_agent_frame(short_frame, kind, token, out));
  // Unknown kind.
  bytes_t bad_kind = frame;
  bad_kind[1] = 7;
  EXPECT_FALSE(parse_agent_frame(bad_kind, kind, token, out));
}
