/**
 * @file tests/unit/platform/windows/test_display_cursor.cpp
 * @brief Test Windows cursor image conversion.
 */
#ifdef _WIN32

  #include <algorithm>
  #include <cstdint>
  #include <cstring>
  #include <initializer_list>

  #include <src/platform/windows/display_cursor.h>
  #include <src/cursor_channel.h>

  #include "../../../tests_common.h"

namespace {
  util::buffer_t<std::uint8_t>
  make_buffer(std::initializer_list<std::uint8_t> bytes) {
    util::buffer_t<std::uint8_t> buffer(bytes.size());
    std::copy(bytes.begin(), bytes.end(), buffer.begin());
    return buffer;
  }

  std::uint32_t
  read_pixel(const util::buffer_t<std::uint8_t> &image, std::size_t index) {
    std::uint32_t pixel;
    std::memcpy(&pixel, image.begin() + index * sizeof(pixel), sizeof(pixel));
    return pixel;
  }

  void
  write_pixel(util::buffer_t<std::uint8_t> &image,
              std::size_t index,
              std::uint32_t pixel) {
    std::memcpy(image.begin() + index * sizeof(pixel), &pixel, sizeof(pixel));
  }
}  // namespace

TEST(WindowsCursorImage, IgnoresMonochromeMaskRowPadding) {
  DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};
  shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
  shape_info.Width = 9;
  shape_info.Height = 4;
  shape_info.Pitch = 4;

  const auto masks = make_buffer({
    0x80, 0x00, 0xFF, 0xFF,
    0x00, 0x00, 0xFF, 0xFF,
    0x80, 0x00, 0xFF, 0xFF,
    0xFF, 0x80, 0xFF, 0xFF,
  });

  const auto alpha = platf::dxgi::make_cursor_alpha_image(masks, shape_info);
  const auto xor_mask = platf::dxgi::make_cursor_xor_image(masks, shape_info);

  ASSERT_EQ(alpha.size(), 9u * 2u * sizeof(std::uint32_t));
  ASSERT_EQ(xor_mask.size(), alpha.size());

  EXPECT_EQ(read_pixel(alpha, 0), 0x00000000u);
  EXPECT_EQ(read_pixel(xor_mask, 0), 0xFFFFFFFFu);
  for (std::size_t column = 1; column < 9; ++column) {
    EXPECT_EQ(read_pixel(alpha, column), 0xFF000000u);
    EXPECT_EQ(read_pixel(xor_mask, column), 0x00000000u);
  }
  for (std::size_t column = 0; column < 9; ++column) {
    EXPECT_EQ(read_pixel(alpha, 9 + column), 0xFFFFFFFFu);
    EXPECT_EQ(read_pixel(xor_mask, 9 + column), 0x00000000u);
  }
}

TEST(WindowsCursorImage, PreservesMaskedColorPixelConversion) {
  DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};
  shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;

  util::buffer_t<std::uint8_t> image(2 * sizeof(std::uint32_t));
  write_pixel(image, 0, 0x00112233u);
  write_pixel(image, 1, 0xFF445566u);

  const auto alpha = platf::dxgi::make_cursor_alpha_image(image, shape_info);
  const auto xor_mask = platf::dxgi::make_cursor_xor_image(image, shape_info);

  ASSERT_EQ(alpha.size(), image.size());
  ASSERT_EQ(xor_mask.size(), image.size());
  EXPECT_EQ(read_pixel(alpha, 0), 0xFF112233u);
  EXPECT_EQ(read_pixel(alpha, 1), 0x00000000u);
  EXPECT_EQ(read_pixel(xor_mask, 0), 0x00000000u);
  EXPECT_EQ(read_pixel(xor_mask, 1), 0xFF445566u);
}

TEST(WindowsCursorImage, RepublishesDesktopDuplicationShapeOnLocalModeActivation) {
  constexpr std::uint32_t session_id = 0xDDC00001u;
  struct session_guard_t {
    std::uint32_t session_id;

    ~session_guard_t() {
      cursor_channel::remove_session(session_id);
    }
  } session_guard {session_id};
  cursor_channel::set_session_enabled(session_id, true);

  platf::dxgi::duplication_t duplication;
  auto &cursor = duplication.cursor;
  cursor.visible = true;
  cursor.shape_id = 7;
  cursor.shape_info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
  cursor.shape_info.Width = 2;
  cursor.shape_info.Height = 1;
  cursor.shape_info.Pitch = 12;
  cursor.shape_info.HotSpot.x = 1;
  cursor.shape_info.HotSpot.y = 0;
  cursor.img_data = {
    0x33, 0x22, 0x11, 0xFF,
    0x66, 0x55, 0x44, 0xFF,
    0xEE, 0xEE, 0xEE, 0xEE,
  };

  ASSERT_TRUE(platf::dxgi::sync_local_cursor_mode(duplication));

  cursor_channel::snapshot_t published;
  ASSERT_TRUE(cursor_channel::copy_latest(0, published));
  EXPECT_TRUE(published.visible);
  EXPECT_TRUE(published.has_shape);
  EXPECT_EQ(published.shape_id, 7u);
  EXPECT_EQ(published.width, 2u);
  EXPECT_EQ(published.height, 1u);
  EXPECT_EQ(published.hotspot_x, 1);
  EXPECT_EQ(published.hotspot_y, 0);
  EXPECT_EQ(published.bgra, (std::vector<std::uint8_t> {
    0x33, 0x22, 0x11, 0xFF,
    0x66, 0x55, 0x44, 0xFF,
  }));

  EXPECT_TRUE(platf::dxgi::sync_local_cursor_mode(duplication));
  cursor_channel::snapshot_t duplicate;
  EXPECT_FALSE(cursor_channel::copy_latest(published.revision, duplicate));
}

#endif
