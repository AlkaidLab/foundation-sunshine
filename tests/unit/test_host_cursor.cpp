/**
 * @file tests/unit/test_host_cursor.cpp
 * @brief Cursor protocol v2: wire format, bitmap conversion, geometry, session policy and cursor_channel.
 */
#include <src/cursor_channel.h>
#include <src/host_cursor.h>

#include <cmath>

#include <gtest/gtest.h>

using namespace std::literals;
namespace proto = host_cursor_protocol;
namespace bitmap = host_cursor_bitmap;

namespace {
  std::uint32_t
  le32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset + 1]) << 8 |
           std::uint32_t(bytes[offset + 2]) << 16 | std::uint32_t(bytes[offset + 3]) << 24;
  }

  std::uint16_t
  le16(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return std::uint16_t(bytes[offset] | bytes[offset + 1] << 8);
  }

  std::array<std::uint8_t, 4>
  px(const bitmap::image_t &image, std::uint32_t x, std::uint32_t y) {
    const auto *p = &image.bgra[(std::size_t(y) * image.width + x) * 4];
    return { p[0], p[1], p[2], p[3] };
  }

  constexpr std::array<std::uint8_t, 4> black { 0, 0, 0, 0xFF };
  constexpr std::array<std::uint8_t, 4> white { 0xFF, 0xFF, 0xFF, 0xFF };
  constexpr std::array<std::uint8_t, 4> clear { 0, 0, 0, 0 };

  bitmap::image_t
  solid(std::uint16_t w, std::uint16_t h, std::uint8_t value) {
    bitmap::image_t image;
    image.width = w;
    image.height = h;
    image.bgra.assign(std::size_t(w) * h * 4, value);
    return image;
  }

  // A 1920x1080 stream of a 2560x1440 output that sits right of a 1920x1080 primary.
  host_cursor::geometry_t
  scaled_geometry() {
    return { true, 1920, 0, 1920, 1080, 0, 0, 2560.0f / 1920.0f };
  }

  // Forward mapping used by absolute input on Windows (input::client_to_touchport()
  // followed by platf::abs_mouse()), from stream coordinates to the desktop.
  std::pair<double, double>
  to_desktop(const host_cursor::geometry_t &g, double sx, double sy) {
    return { (sx - g.client_offset_x) * g.scalar_inv + g.offset_x, (sy - g.client_offset_y) * g.scalar_inv + g.offset_y };
  }

  host_cursor::snapshot_t
  visible_at(std::int32_t x, std::int32_t y, std::uint32_t shape_id) {
    host_cursor::snapshot_t snapshot;
    snapshot.available = true;
    snapshot.visible = true;
    snapshot.shape_id = shape_id;
    snapshot.x = x;
    snapshot.y = y;
    return snapshot;
  }

  void
  subscribe(host_cursor::session_state_t &session, bool client_draws) {
    host_cursor::apply_request(session, { std::uint8_t(proto::request_flags::subscribe | (client_draws ? proto::request_flags::client_draws : 0)), 0 });
  }
}  // namespace

TEST(HostCursorProtocol, StateIsExactLittleEndian) {
  proto::state_t state;
  state.flags = proto::state_flags::visible | proto::state_flags::has_position;
  state.seq = 0x01020304;
  state.shape_id = 0x0A0B0C0D;
  state.ref_width = 1920;
  state.ref_height = 1080;
  state.scale_q16 = 0x00018000;
  state.x = -2;
  state.y = 0x11223344;
  state.clip_left = 1;
  state.clip_top = 2;
  state.clip_right = 3;
  state.clip_bottom = 4;
  const auto bytes = proto::encode_state(state);
  // The payload matches moonlight-common-c's tests/CursorStreamGoldenTest.c.
  const std::vector<std::uint8_t> expected {
    0x09, 0x55, 44, 0,  // type 0x5509, payloadLength 44
    2, 1, 0x05, 0,  // version, kind, flags, reserved
    0x04, 0x03, 0x02, 0x01,
    0x0D, 0x0C, 0x0B, 0x0A,
    0x80, 0x07, 0x38, 0x04,  // 1920, 1080
    0x00, 0x80, 0x01, 0x00,
    0xFE, 0xFF, 0xFF, 0xFF,
    0x44, 0x33, 0x22, 0x11,
    1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0,
  };
  EXPECT_EQ(bytes, expected);
}

TEST(HostCursorProtocol, StateNeverCarriesUnknownFlags) {
  proto::state_t state;
  state.flags = 0xFF;
  EXPECT_EQ(proto::encode_state(state)[6], proto::state_flags::all);
}

TEST(HostCursorProtocol, SmallShapeIsOneChunk) {
  std::vector<std::uint8_t> pixels(32 * 32 * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(i);
  const auto chunks = proto::encode_shape(7, 32, 32, 3, 4, pixels);
  ASSERT_EQ(chunks.size(), 1u);
  const auto &c = chunks[0];
  ASSERT_EQ(c.size(), 4 + proto::shape_header_size + pixels.size());
  EXPECT_EQ(le16(c, 0), proto::packet_type);
  EXPECT_EQ(le16(c, 2), proto::shape_header_size + pixels.size());
  EXPECT_EQ(c[4], 2);
  EXPECT_EQ(c[5], proto::kind_shape);
  EXPECT_EQ(c[6], proto::format_bgra8);
  EXPECT_EQ(c[7], 0);
  EXPECT_EQ(le32(c, 8), 7u);
  EXPECT_EQ(le16(c, 12), 32);
  EXPECT_EQ(le16(c, 14), 32);
  EXPECT_EQ(le16(c, 16), 3);
  EXPECT_EQ(le16(c, 18), 4);
  EXPECT_EQ(le32(c, 20), pixels.size());
  EXPECT_EQ(le32(c, 24), 0u);
  EXPECT_EQ(le16(c, 28), pixels.size());
  EXPECT_EQ(le16(c, 30), 0);
  EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), c.begin() + 4 + proto::shape_header_size));
}

TEST(HostCursorProtocol, LargeShapeIsChunkedContiguously) {
  std::vector<std::uint8_t> pixels(256 * 256 * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(i * 7);
  const auto chunks = proto::encode_shape(9, 256, 256, 0, 0, pixels);
  ASSERT_EQ(chunks.size(), pixels.size() / proto::max_chunk_bytes);
  std::vector<std::uint8_t> joined;
  for (const auto &c : chunks) {
    EXPECT_EQ(le32(c, 24), joined.size());
    EXPECT_EQ(le32(c, 20), pixels.size());
    ASSERT_EQ(c.size(), 4 + proto::shape_header_size + le16(c, 28));
    EXPECT_LE(c.size() - 4, 0xFFFFu);
    joined.insert(joined.end(), c.begin() + 4 + proto::shape_header_size, c.end());
  }
  EXPECT_EQ(joined, pixels);

  const auto odd = proto::encode_shape(9, 100, 100, 0, 0, std::vector<std::uint8_t>(100 * 100 * 4));
  ASSERT_EQ(odd.size(), 3u);
  EXPECT_EQ(le16(odd[2], 28), 100 * 100 * 4 - 2 * proto::max_chunk_bytes);
}

TEST(HostCursorProtocol, InvalidShapesAreNotEncoded) {
  const std::vector<std::uint8_t> pixels(4 * 4 * 4);
  EXPECT_TRUE(proto::encode_shape(0, 4, 4, 0, 0, pixels).empty());
  EXPECT_TRUE(proto::encode_shape(1, 0, 4, 0, 0, {}).empty());
  EXPECT_TRUE(proto::encode_shape(1, 4, 4, 4, 0, pixels).empty());
  EXPECT_TRUE(proto::encode_shape(1, 4, 4, 0, 4, pixels).empty());
  EXPECT_TRUE(proto::encode_shape(1, 4, 5, 0, 0, pixels).empty());
  EXPECT_TRUE(proto::encode_shape(1, 257, 1, 0, 0, std::vector<std::uint8_t>(257 * 4)).empty());
}

TEST(HostCursorProtocol, RequestParsesOnlyExactVersionTwoMessages) {
  const auto valid = proto::encode_request({ proto::request_flags::subscribe | proto::request_flags::client_draws, 0x44332211 });
  const std::array<std::uint8_t, 8> expected { 2, 3, 0, 0, 0x11, 0x22, 0x33, 0x44 };
  EXPECT_EQ(valid, expected);
  const auto parsed = proto::parse_request(valid);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->flags, 3);
  EXPECT_EQ(parsed->resend_shape_id, 0x44332211u);

  for (std::size_t size = 0; size <= valid.size() + 1; ++size) {
    std::vector<std::uint8_t> bytes(valid.begin(), valid.begin() + std::min(size, valid.size()));
    bytes.resize(size);
    EXPECT_EQ(proto::parse_request(bytes).has_value(), size == valid.size()) << size;
  }

  // A cursor protocol v1 request is not a v2 request.
  EXPECT_FALSE(proto::parse_request(std::vector<std::uint8_t> { 1, 1, 0, 0 }));
  auto bad = valid;
  bad[0] = 1;
  EXPECT_FALSE(proto::parse_request(bad));
  bad = valid;
  bad[0] = 3;
  EXPECT_FALSE(proto::parse_request(bad));
  bad = valid;
  bad[1] = 0x04;
  EXPECT_FALSE(proto::parse_request(bad));
  bad = valid;
  bad[2] = 1;
  EXPECT_FALSE(proto::parse_request(bad));
  bad = valid;
  bad[3] = 1;
  EXPECT_FALSE(proto::parse_request(bad));
}

TEST(HostCursorBitmap, MonochromeTruthTableAndInvertOutline) {
  // 8x1 visible pixels; AND row then XOR row.
  // x:   0    1    2    3    4    5    6    7
  // AND: 0    0    1    1    1    1    1    1
  // XOR: 0    1    0    1    0    0    0    0
  const std::vector<std::uint8_t> masks { 0b00111111, 0, 0b01010000, 0 };
  const auto image = bitmap::from_monochrome(8, 1, 2, masks, 1, 0);
  ASSERT_TRUE(image);
  EXPECT_EQ(image->width, 8);
  EXPECT_EQ(image->height, 1);
  EXPECT_EQ(image->hotspot_x, 1);
  EXPECT_EQ(px(*image, 0, 0), black);
  EXPECT_EQ(px(*image, 1, 0), white);
  EXPECT_EQ(px(*image, 2, 0), black);  // transparent, but next to the inverting pixel
  EXPECT_EQ(px(*image, 3, 0), white);  // inverting
  EXPECT_EQ(px(*image, 4, 0), black);  // outline
  EXPECT_EQ(px(*image, 5, 0), clear);
  EXPECT_EQ(px(*image, 7, 0), clear);
}

TEST(HostCursorBitmap, InvertingIBeamGetsOneRingOfOutline) {
  // 5x5, a vertical inverting bar in column 2. Same look as
  // platf::dxgi::make_local_cursor_image(): white with a black outline.
  std::vector<std::uint8_t> masks(2 * 5 * 2);
  for (int y = 0; y < 5; ++y) {
    masks[y * 2] = 0xFF;  // AND: everything transparent or inverting
    masks[(5 + y) * 2] = y == 0 || y == 4 ? 0 : 0b00100000;  // XOR: the bar, rows 1..3
  }
  const auto image = bitmap::from_monochrome(5, 5, 2, masks, 2, 2);
  ASSERT_TRUE(image);
  for (int y = 1; y <= 3; ++y) {
    EXPECT_EQ(px(*image, 2, y), white) << y;
    EXPECT_EQ(px(*image, 1, y), black) << y;
    EXPECT_EQ(px(*image, 3, y), black) << y;
    EXPECT_EQ(px(*image, 0, y), clear) << y;
    EXPECT_EQ(px(*image, 4, y), clear) << y;
  }
  EXPECT_EQ(px(*image, 2, 0), black);
  EXPECT_EQ(px(*image, 2, 4), black);
}

TEST(HostCursorBitmap, ColorKeepsStraightAlphaAndHonoursPitch) {
  // 2x2 with 4 bytes of row padding.
  const std::vector<std::uint8_t> bgra {
    1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,
    9, 10, 11, 0, 13, 14, 15, 255, 0xEE, 0xEE, 0xEE, 0xEE,
  };
  const auto image = bitmap::from_color(2, 2, 12, bgra, 1, 1);
  ASSERT_TRUE(image);
  const std::vector<std::uint8_t> expected { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 13, 14, 15, 255 };
  EXPECT_EQ(image->bgra, expected);
  EXPECT_EQ(image->pixel_scale, 1.0);
}

TEST(HostCursorBitmap, ColorAndMaskMatchesGdiSemantics) {
  const std::vector<std::uint8_t> bgra {
    10, 20, 30, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0, 40, 50, 60, 0,
  };
  const std::vector<std::uint8_t> and_mask { 0b01100000, 0 };  // pixels 1, 2: screen-dependent
  const auto image = bitmap::from_color_and_mask(4, 1, 16, bgra, 2, and_mask, 0, 0);
  ASSERT_TRUE(image);
  EXPECT_EQ(px(*image, 0, 0), (std::array<std::uint8_t, 4> { 10, 20, 30, 0xFF }));
  EXPECT_EQ(px(*image, 1, 0), black);  // transparent, outlined
  EXPECT_EQ(px(*image, 2, 0), white);  // invert
  EXPECT_EQ(px(*image, 3, 0), (std::array<std::uint8_t, 4> { 40, 50, 60, 0xFF }));
}

TEST(HostCursorBitmap, HugeCursorsAreBoxFilteredToTheWireLimit) {
  std::vector<std::uint8_t> bgra(600 * 300 * 4);
  for (std::size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 0x40;
    bgra[i + 3] = (i / 4) % 2 ? 0xFF : 0;  // alternating opaque and transparent columns
  }
  const auto image = bitmap::from_color(600, 300, 600 * 4, bgra, 599, 10);
  ASSERT_TRUE(image);
  EXPECT_EQ(image->pixel_scale, 3.0);
  EXPECT_EQ(image->width, 200);
  EXPECT_EQ(image->height, 100);
  EXPECT_EQ(image->hotspot_x, 199);
  EXPECT_EQ(image->hotspot_y, 3);
  // Premultiplied filtering keeps the colour of the opaque third of each box.
  const auto p = px(*image, 0, 0);
  EXPECT_EQ(p[0], 0x40);
  EXPECT_NEAR(p[3], 0xFF / 3, 1);
}

TEST(HostCursorBitmap, StandardSizeGdiCursorsAreDrawnAtTheDisplaysStandardSize) {
  // Sunshine started at 250% (standard 64 px), streaming a 100% display (32 px).
  EXPECT_EQ(bitmap::gdi_display_scale(64, 64, 64, 32), 0.5);
  // Started at 100%, streaming a 150% display (48 px).
  EXPECT_EQ(bitmap::gdi_display_scale(32, 32, 32, 48), 1.5);
  EXPECT_EQ(bitmap::gdi_display_scale(32, 32, 32, 32), 1.0);
  // An application's own size is kept.
  EXPECT_EQ(bitmap::gdi_display_scale(32, 32, 64, 32), 1.0);
  EXPECT_EQ(bitmap::gdi_display_scale(64, 32, 64, 32), 1.0);
  EXPECT_EQ(bitmap::gdi_display_scale(64, 64, 0, 32), 1.0);
}

TEST(HostCursorBitmap, MalformedInputIsRejected) {
  EXPECT_FALSE(bitmap::from_monochrome(9, 1, 1, std::vector<std::uint8_t>(4), 0, 0));  // pitch too small
  EXPECT_FALSE(bitmap::from_monochrome(8, 2, 1, std::vector<std::uint8_t>(3), 0, 0));  // short masks
  EXPECT_FALSE(bitmap::from_monochrome(0, 1, 1, std::vector<std::uint8_t>(2), 0, 0));
  EXPECT_FALSE(bitmap::from_color(2, 2, 8, std::vector<std::uint8_t>(15), 0, 0));
  EXPECT_FALSE(bitmap::from_color(2, 2, 7, std::vector<std::uint8_t>(16), 0, 0));
  EXPECT_FALSE(bitmap::from_color(5000, 1, 20000, std::vector<std::uint8_t>(20000), 0, 0));  // not a real cursor
  EXPECT_FALSE(bitmap::from_color_and_mask(2, 2, 8, std::vector<std::uint8_t>(16), 2, std::vector<std::uint8_t>(3), 0, 0));
}

TEST(HostCursorBitmap, HashTracksContentAndGeometry) {
  auto a = solid(4, 4, 1);
  auto b = solid(4, 4, 1);
  EXPECT_EQ(bitmap::content_hash(a), bitmap::content_hash(b));
  b.hotspot_x = 1;
  EXPECT_NE(bitmap::content_hash(a), bitmap::content_hash(b));
  b = a;
  b.bgra[5] = 2;
  EXPECT_NE(bitmap::content_hash(a), bitmap::content_hash(b));
  b = a;
  b.pixel_scale = 0.5;
  EXPECT_NE(bitmap::content_hash(a), bitmap::content_hash(b));
}

TEST(HostCursorGeometry, InverseOfAbsoluteInputMapping) {
  const auto g = scaled_geometry();
  EXPECT_EQ(host_cursor::to_stream(g, 1920 + 1280, 720), (host_cursor::point_t { 960, 540 }));
  EXPECT_EQ(host_cursor::to_stream(g, 1920, 0), (host_cursor::point_t { 0, 0 }));
  for (int sx = 0; sx <= 1920; sx += 37) {
    for (int sy = 0; sy <= 1080; sy += 41) {
      const auto [dx, dy] = to_desktop(g, sx, sy);
      EXPECT_EQ(host_cursor::to_stream(g, std::lround(dx), std::lround(dy)), (host_cursor::point_t { sx, sy }));
    }
  }
}

TEST(HostCursorGeometry, LetterboxedOutput) {
  // A 4:3 1440x1080 output in a 16:9 1920x1080 stream: 240 px bars either side.
  const host_cursor::geometry_t g { true, 0, 0, 1920, 1080, 240, 0, 1.0f };
  EXPECT_EQ(host_cursor::to_stream(g, 0, 0), (host_cursor::point_t { 240, 0 }));
  EXPECT_EQ(host_cursor::to_stream(g, 1440, 1080), (host_cursor::point_t { 1680, 1080 }));
  // Off the streamed output (another monitor) is reported as is.
  EXPECT_EQ(host_cursor::to_stream(g, -100, 0), (host_cursor::point_t { 140, 0 }));
}

TEST(HostCursorRegistry, DeduplicatesAndEvictsLeastRecentlyUsed) {
  host_cursor::shape_registry_t registry;
  const auto first = registry.insert(solid(2, 2, 1));
  EXPECT_NE(first, 0u);
  EXPECT_EQ(registry.insert(solid(2, 2, 1)), first);
  const auto second = registry.insert(solid(2, 2, 2));
  EXPECT_NE(second, first);
  ASSERT_TRUE(registry.find(first));
  EXPECT_EQ(registry.find(first)->bgra[0], 1);
  EXPECT_FALSE(registry.find(0));

  // Touch `first` so `second` is the oldest, then overflow by one.
  (void) registry.find(first);
  for (std::size_t i = 0; i < host_cursor::shape_registry_t::capacity - 1; ++i) {
    (void) registry.insert(solid(3, 3, std::uint8_t(i)));
  }
  EXPECT_TRUE(registry.find(first));
  EXPECT_FALSE(registry.find(second));
  // A re-inserted shape gets a new id; ids are never reused.
  const auto again = registry.insert(solid(2, 2, 2));
  EXPECT_NE(again, second);
}

TEST(HostCursorInjections, ExplainsRecentAbsoluteTargetsOnly) {
  host_cursor::injection_history_t history;
  const auto t0 = host_cursor::clock::now();
  EXPECT_FALSE(history.explains(10, 10, t0));
  history.note_absolute(100, 100, t0);
  history.note_absolute(200, 200, t0 + 1ms);
  EXPECT_TRUE(history.explains(100, 100, t0 + 2ms));  // the cursor still shows the older target
  EXPECT_TRUE(history.explains(203, 197, t0 + 2ms));
  EXPECT_FALSE(history.explains(204, 200, t0 + 2ms));
  EXPECT_FALSE(history.explains(100, 100, t0 + 300ms));
  for (int i = 0; i < 8; ++i) history.note_absolute(i, 0, t0 + 3ms);
  EXPECT_FALSE(history.explains(200, 200, t0 + 4ms));  // pushed out of the history
}

TEST(HostCursorCompositing, OnlyWhenEveryConnectedSessionDraws) {
  EXPECT_FALSE(host_cursor::all_sessions_draw(0, 0, true));
  EXPECT_FALSE(host_cursor::all_sessions_draw(2, 1, true));
  EXPECT_TRUE(host_cursor::all_sessions_draw(2, 2, true));
  EXPECT_FALSE(host_cursor::all_sessions_draw(1, 1, false));
}

TEST(HostCursorSession, NothingWithoutSubscription) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  const auto update = host_cursor::plan_update(session, visible_at(0, 0, 0), scaled_geometry(), true, host_cursor::clock::now(), registry);
  EXPECT_FALSE(update.state);
  EXPECT_TRUE(update.shapes.empty());
}

TEST(HostCursorSession, SubscribeResyncsWithShapeAndPosition) {
  host_cursor::shape_registry_t registry;
  const auto id = registry.insert(solid(32, 32, 9));
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  const auto update = host_cursor::plan_update(session, visible_at(1920 + 1280, 720, id), scaled_geometry(), false, now, registry);
  ASSERT_TRUE(update.state);
  ASSERT_EQ(update.shape_ids, std::vector<std::uint32_t> { id });
  const auto &s = *update.state;
  using namespace proto::state_flags;
  EXPECT_EQ(s.flags, visible | has_position | resync);
  EXPECT_EQ(s.seq, 1u);
  EXPECT_EQ(s.shape_id, id);
  EXPECT_EQ(s.ref_width, 1920);
  EXPECT_EQ(s.ref_height, 1080);
  EXPECT_EQ(s.scale_q16, 49152u);  // 0.75 stream px per desktop px
  EXPECT_EQ(s.x, 960);
  EXPECT_EQ(s.y, 540);

  // Nothing changed: nothing to send, and the shape is not sent twice.
  const auto idle = host_cursor::plan_update(session, visible_at(1920 + 1280, 720, id), scaled_geometry(), false, now, registry);
  EXPECT_FALSE(idle.state);
  EXPECT_TRUE(idle.shapes.empty());
}

TEST(HostCursorSession, ResyncWaitsForTheSessionGeometry) {
  // A client can subscribe before the encoder publishes the session's input
  // geometry. Nothing may be sent until then: a zero reference size is invalid on
  // the wire, and the resync must not be spent on it.
  host_cursor::shape_registry_t registry;
  const auto id = registry.insert(solid(4, 4, 1));
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  const host_cursor::geometry_t unpublished;
  const auto early = host_cursor::plan_update(session, visible_at(2000, 100, id), unpublished, true, now, registry);
  EXPECT_FALSE(early.state);
  EXPECT_TRUE(early.shapes.empty());
  EXPECT_TRUE(session.resync_pending);

  const auto later = host_cursor::plan_update(session, visible_at(2000, 100, id), scaled_geometry(), true, now, registry);
  ASSERT_TRUE(later.state);
  EXPECT_TRUE(later.state->flags & proto::state_flags::resync);
  EXPECT_EQ(later.state->ref_width, 1920);
  EXPECT_EQ(later.shape_ids, std::vector<std::uint32_t> { id });
}

TEST(HostCursorSession, DrawingClientOnlyHearsAboutHostMoves) {
  host_cursor::shape_registry_t registry;
  const auto id = registry.insert(solid(4, 4, 1));
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  auto snapshot = visible_at(2000, 100, id);
  (void) host_cursor::plan_update(session, snapshot, scaled_geometry(), false, now, registry);

  // The client moved it (absolute input): not echoed.
  snapshot.x = 2100;
  EXPECT_FALSE(host_cursor::plan_update(session, snapshot, scaled_geometry(), false, now, registry).state);

  // The host moved it.
  snapshot.x = 2400;
  snapshot.warp_serial++;
  const auto warp = host_cursor::plan_update(session, snapshot, scaled_geometry(), false, now, registry);
  ASSERT_TRUE(warp.state);
  EXPECT_TRUE(warp.state->flags & proto::state_flags::has_position);
  EXPECT_EQ(warp.state->x, 360);
  EXPECT_EQ(warp.state->seq, 2u);
}

TEST(HostCursorSession, NonDrawingClientTracksPositionWhileVisible) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  subscribe(session, false);
  const auto now = host_cursor::clock::now();
  auto snapshot = visible_at(1920, 0, 0);
  snapshot.visible = false;
  (void) host_cursor::plan_update(session, snapshot, scaled_geometry(), true, now, registry);

  // Hidden: moving it (a game recentring) is not reported.
  snapshot.x = 2000;
  EXPECT_FALSE(host_cursor::plan_update(session, snapshot, scaled_geometry(), true, now, registry).state);

  // Becoming visible carries the position.
  snapshot.visible = true;
  const auto shown = host_cursor::plan_update(session, snapshot, scaled_geometry(), true, now, registry);
  ASSERT_TRUE(shown.state);
  EXPECT_EQ(shown.state->flags & (proto::state_flags::visible | proto::state_flags::has_position),
    proto::state_flags::visible | proto::state_flags::has_position);
  EXPECT_TRUE(shown.state->flags & proto::state_flags::composited);

  // And so does every move while visible, so the client can warp when it returns to absolute.
  snapshot.x = 2080;
  const auto moved = host_cursor::plan_update(session, snapshot, scaled_geometry(), true, now, registry);
  ASSERT_TRUE(moved.state);
  EXPECT_EQ(moved.state->x, 120);
}

TEST(HostCursorSession, CompositedChangeIsSent) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  (void) host_cursor::plan_update(session, visible_at(1920, 0, 0), scaled_geometry(), true, now, registry);

  // Another session left, so frames stopped carrying the cursor.
  const auto update = host_cursor::plan_update(session, visible_at(1920, 0, 0), scaled_geometry(), false, now, registry);
  ASSERT_TRUE(update.state);
  EXPECT_FALSE(update.state->flags & proto::state_flags::composited);
}

TEST(HostCursorSession, ClipIsMappedAndClamped) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  subscribe(session, true);
  auto snapshot = visible_at(2000, 100, 0);
  snapshot.clipped = true;
  snapshot.clip = { 1920 + 400, 200, 1920 + 4000, 1300 };
  const auto update = host_cursor::plan_update(session, snapshot, scaled_geometry(), false, host_cursor::clock::now(), registry);
  ASSERT_TRUE(update.state);
  EXPECT_TRUE(update.state->flags & proto::state_flags::has_clip);
  EXPECT_EQ(update.state->clip_left, 300);
  EXPECT_EQ(update.state->clip_top, 150);
  EXPECT_EQ(update.state->clip_right, 1920);
  EXPECT_EQ(update.state->clip_bottom, 975);

  // Releasing the clip is a change, and carries the position.
  snapshot.clipped = false;
  const auto released = host_cursor::plan_update(session, snapshot, scaled_geometry(), false, host_cursor::clock::now(), registry);
  ASSERT_TRUE(released.state);
  EXPECT_FALSE(released.state->flags & proto::state_flags::has_clip);
  EXPECT_TRUE(released.state->flags & proto::state_flags::has_position);
}

TEST(HostCursorSession, FallbackWhenInformationOrGeometryIsUnavailable) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  subscribe(session, true);
  host_cursor::snapshot_t unavailable;
  const auto update = host_cursor::plan_update(session, unavailable, scaled_geometry(), true, host_cursor::clock::now(), registry);
  ASSERT_TRUE(update.state);
  using namespace proto::state_flags;
  EXPECT_EQ(update.state->flags, fallback | composited | resync);

  auto invalid = scaled_geometry();
  invalid.valid = false;
  const auto no_geometry = host_cursor::plan_update(session, visible_at(0, 0, 0), invalid, true, host_cursor::clock::now(), registry);
  EXPECT_FALSE(no_geometry.state);  // still the same fallback state

  const auto recovered = host_cursor::plan_update(session, visible_at(1920, 0, 0), scaled_geometry(), false, host_cursor::clock::now(), registry);
  ASSERT_TRUE(recovered.state);
  EXPECT_EQ(recovered.state->flags, visible | has_position);
}

TEST(HostCursorSession, ShapeBudgetKeepsThePreviousShape) {
  host_cursor::shape_registry_t registry;
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  std::vector<std::uint32_t> ids;
  for (int i = 0; i < 40; ++i) ids.push_back(registry.insert(solid(256, 256, std::uint8_t(i))));  // 256 KiB each

  std::uint32_t last_shape = 0;
  std::size_t sent = 0;
  for (const auto id : ids) {
    const auto update = host_cursor::plan_update(session, visible_at(1920, 0, id), scaled_geometry(), false, now, registry);
    sent += update.shapes.size();
    if (update.state) last_shape = update.state->shape_id;
  }
  EXPECT_EQ(sent, host_cursor::shape_budget_bytes_per_second / (256 * 256 * 4));
  EXPECT_EQ(last_shape, ids[sent - 1]);  // the client keeps a shape it has

  // A second later the newest shape goes out.
  const auto later = host_cursor::plan_update(session, visible_at(1920, 0, ids.back()), scaled_geometry(), false, now + 1s, registry);
  ASSERT_EQ(later.shape_ids, std::vector<std::uint32_t> { ids.back() });
  ASSERT_TRUE(later.state);
  EXPECT_EQ(later.state->shape_id, ids.back());
}

TEST(HostCursorSession, ResendAndResubscribe) {
  host_cursor::shape_registry_t registry;
  const auto id = registry.insert(solid(4, 4, 3));
  host_cursor::session_state_t session;
  subscribe(session, true);
  const auto now = host_cursor::clock::now();
  (void) host_cursor::plan_update(session, visible_at(1920, 0, id), scaled_geometry(), false, now, registry);

  host_cursor::apply_request(session, { proto::request_flags::subscribe | proto::request_flags::client_draws, id });
  const auto resend = host_cursor::plan_update(session, visible_at(1920, 0, id), scaled_geometry(), false, now, registry);
  EXPECT_EQ(resend.shape_ids, std::vector<std::uint32_t> { id });
  EXPECT_FALSE(resend.state);

  host_cursor::apply_request(session, { 0, 0 });
  EXPECT_FALSE(session.subscribed);
  EXPECT_FALSE(session.client_draws);
  EXPECT_FALSE(host_cursor::plan_update(session, visible_at(1920, 0, id), scaled_geometry(), false, now, registry).state);

  // A new subscription (a reconnecting client) gets everything again.
  subscribe(session, true);
  const auto again = host_cursor::plan_update(session, visible_at(1920, 0, id), scaled_geometry(), false, now, registry);
  EXPECT_EQ(again.shape_ids, std::vector<std::uint32_t> { id });
  ASSERT_TRUE(again.state);
  EXPECT_TRUE(again.state->flags & proto::state_flags::resync);
}

namespace {
  // cursor_channel is process-wide; leave it as each test found it.
  class CursorChannelTest: public ::testing::Test {
  protected:
    void
    TearDown() override {
      cursor_channel::remove_session(1);
      cursor_channel::remove_session(2);
      while (cursor_channel::shapes_wanted()) {
        cursor_channel::remove_shape_consumer();
      }
      cursor_channel::set_all_sessions_draw(false);
    }

    static void
    publish(std::uint32_t shape_id, std::uint8_t fill) {
      cursor_channel::publish_visibility(true, shape_id);
      cursor_channel::publish_shape(true, shape_id, 2, 2, 0, 0, std::vector<std::uint8_t>(16, fill));
    }
  };
}  // namespace

TEST_F(CursorChannelTest, NothingIsPublishedWithoutConsumers) {
  publish(1, 1);
  cursor_channel::snapshot_t snapshot;
  EXPECT_FALSE(cursor_channel::copy_latest(0, snapshot));
  std::uint64_t revision = 0;
  EXPECT_FALSE(cursor_channel::copy_shape_if_newer(revision, snapshot));
  EXPECT_FALSE(cursor_channel::shapes_wanted());
}

TEST_F(CursorChannelTest, VersionOneSessionsKeepTheirRules) {
  const auto epoch = cursor_channel::publish_epoch();
  cursor_channel::set_session_enabled(1, true);
  EXPECT_TRUE(cursor_channel::local_mode_active());
  EXPECT_TRUE(cursor_channel::shapes_wanted());
  EXPECT_TRUE(cursor_channel::frames_exclude_cursor());
  EXPECT_NE(cursor_channel::publish_epoch(), epoch);  // capture republishes its stored shape

  publish(7, 3);
  cursor_channel::snapshot_t snapshot;
  ASSERT_TRUE(cursor_channel::copy_latest(0, snapshot));
  EXPECT_TRUE(snapshot.has_shape);
  EXPECT_EQ(snapshot.shape_id, 7u);

  cursor_channel::remove_session(1);
  EXPECT_FALSE(cursor_channel::local_mode_active());
  EXPECT_FALSE(cursor_channel::frames_exclude_cursor());
  EXPECT_FALSE(cursor_channel::copy_latest(0, snapshot));
}

TEST_F(CursorChannelTest, VersionTwoConsumerGetsShapesWithoutLocalMode) {
  const auto epoch = cursor_channel::publish_epoch();
  cursor_channel::add_shape_consumer();
  EXPECT_TRUE(cursor_channel::shapes_wanted());
  EXPECT_FALSE(cursor_channel::local_mode_active());
  EXPECT_FALSE(cursor_channel::frames_exclude_cursor());
  EXPECT_NE(cursor_channel::publish_epoch(), epoch);

  publish(9, 5);
  std::uint64_t revision = 0;
  cursor_channel::snapshot_t snapshot;
  ASSERT_TRUE(cursor_channel::copy_shape_if_newer(revision, snapshot));
  EXPECT_EQ(snapshot.shape_id, 9u);
  EXPECT_EQ(snapshot.bgra, std::vector<std::uint8_t>(16, 5));
  // Visibility changes alone do not copy the pixels again.
  cursor_channel::publish_visibility(false, 9);
  EXPECT_FALSE(cursor_channel::copy_shape_if_newer(revision, snapshot));
  publish(10, 6);
  ASSERT_TRUE(cursor_channel::copy_shape_if_newer(revision, snapshot));
  EXPECT_EQ(snapshot.shape_id, 10u);
}

TEST_F(CursorChannelTest, VersionOneSessionJoiningAVersionTwoConsumerGetsTheCurrentShape) {
  cursor_channel::add_shape_consumer();
  publish(4, 2);
  const auto epoch = cursor_channel::publish_epoch();

  // The shape is current (capture kept publishing), so it is not cleared.
  cursor_channel::set_session_enabled(2, true);
  EXPECT_EQ(cursor_channel::publish_epoch(), epoch);
  cursor_channel::snapshot_t snapshot;
  ASSERT_TRUE(cursor_channel::copy_latest(0, snapshot));
  EXPECT_TRUE(snapshot.has_shape);
  EXPECT_EQ(snapshot.shape_id, 4u);

  // The v2 consumer keeps the shapes when the v1 session leaves.
  cursor_channel::remove_session(2);
  EXPECT_TRUE(cursor_channel::shapes_wanted());
  EXPECT_TRUE(cursor_channel::copy_latest(0, snapshot));
}

TEST_F(CursorChannelTest, FramesExcludeTheCursorWhenEverySessionDraws) {
  EXPECT_FALSE(cursor_channel::frames_exclude_cursor());
  cursor_channel::set_all_sessions_draw(true);
  EXPECT_TRUE(cursor_channel::frames_exclude_cursor());
  EXPECT_FALSE(cursor_channel::local_mode_active());
  cursor_channel::set_all_sessions_draw(false);
  EXPECT_FALSE(cursor_channel::frames_exclude_cursor());
}
