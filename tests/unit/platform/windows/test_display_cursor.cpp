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
  // Exercise the production cursor update without switching the user's desktop.
  class pointer_shape_duplication_t final: public IDXGIOutputDuplication {
  public:
    explicit pointer_shape_duplication_t(HRESULT status): status_(status) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
      *object = nullptr;
      return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
      const auto remaining = --references_;
      if (!remaining) delete this;
      return remaining;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *) override {}
    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT, DXGI_OUTDUPL_FRAME_INFO *, IDXGIResource **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT *, UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT, DXGI_OUTDUPL_MOVE_RECT *, UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseFrame() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT size, void *buffer, UINT *required,
                                                   DXGI_OUTDUPL_POINTER_SHAPE_INFO *info) override {
      if (FAILED(status_)) return status_;
      *required = 4;
      if (size < *required) return DXGI_ERROR_MORE_DATA;
      std::memcpy(buffer, "\x33\x22\x11\xFF", 4);
      info->Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
      info->Width = info->Height = 1;
      info->Pitch = 4;
      return S_OK;
    }

  private:
    HRESULT status_;
    ULONG references_ = 1;
  };

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

TEST(WindowsCursorImage, ReinitializesLostDuplicationWithoutPublishingCursorState) {
  platf::dxgi::duplication_t duplication;
  duplication.dup.reset(new pointer_shape_duplication_t(DXGI_ERROR_ACCESS_LOST));
  duplication.cursor.img_data = {0xAA};
  duplication.cursor.shape_id = 7;
  duplication.cursor.x = 10;
  duplication.cursor.visible = true;
  DXGI_OUTDUPL_FRAME_INFO frame {};
  frame.PointerShapeBufferSize = 4;
  frame.LastMouseUpdateTime.QuadPart = 1;
  frame.PointerPosition.Position.x = 99;
  bool updated = true;

  EXPECT_EQ(duplication.update_cursor(frame, updated), platf::capture_e::reinit);
  EXPECT_FALSE(updated);
  EXPECT_EQ(duplication.cursor.img_data, (std::vector<std::uint8_t> {0xAA}));
  EXPECT_EQ(duplication.cursor.shape_id, 7u);
  EXPECT_EQ(duplication.cursor.x, 10);
  EXPECT_TRUE(duplication.cursor.visible);
}

TEST(WindowsCursorImage, KeepsInvalidPointerShapeCallsAsErrors) {
  for (const auto status : {E_INVALIDARG, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_MORE_DATA}) {
    platf::dxgi::duplication_t duplication;
    duplication.dup.reset(new pointer_shape_duplication_t(status));
    DXGI_OUTDUPL_FRAME_INFO frame {};
    frame.PointerShapeBufferSize = 4;
    bool updated = true;
    EXPECT_EQ(duplication.update_cursor(frame, updated), platf::capture_e::error);
    EXPECT_FALSE(updated);
    EXPECT_EQ(duplication.cursor.shape_id, 0u);
    EXPECT_TRUE(duplication.cursor.img_data.empty());
  }
}

TEST(WindowsCursorImage, PublishesPointerShapeAfterSuccessfulNativeRead) {
  platf::dxgi::duplication_t duplication;
  duplication.dup.reset(new pointer_shape_duplication_t(S_OK));
  DXGI_OUTDUPL_FRAME_INFO frame {};
  frame.PointerShapeBufferSize = 4;
  frame.LastMouseUpdateTime.QuadPart = 1;
  frame.PointerPosition.Position.x = 99;
  frame.PointerPosition.Visible = true;
  bool updated = false;
  ASSERT_EQ(duplication.update_cursor(frame, updated), platf::capture_e::ok);
  EXPECT_TRUE(updated);
  EXPECT_EQ(duplication.cursor.shape_id, 1u);
  EXPECT_EQ(duplication.cursor.img_data, (std::vector<std::uint8_t> {0x33, 0x22, 0x11, 0xFF}));
  EXPECT_EQ(duplication.cursor.x, 99);
  EXPECT_TRUE(duplication.cursor.visible);
}

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

TEST(WindowsCursorImage, MakesXorCursorVisibleOnLightAndDarkBackgrounds) {
  platf::dxgi::normalized_cursor_shape_t normalized;
  normalized.info.Width = 3;
  normalized.info.Height = 3;
  normalized.alpha = util::buffer_t<std::uint8_t>(9 * sizeof(std::uint32_t));
  normalized.xor_mask = util::buffer_t<std::uint8_t>(9 * sizeof(std::uint32_t));
  std::fill(normalized.alpha.begin(), normalized.alpha.end(), 0);
  std::fill(normalized.xor_mask.begin(), normalized.xor_mask.end(), 0);
  write_pixel(normalized.alpha, 0, 0xFF112233u);
  write_pixel(normalized.xor_mask, 4, 0xFFFFFFFFu);

  const auto local = platf::dxgi::make_local_cursor_image(normalized);

  ASSERT_EQ(local.size(), normalized.alpha.size());
  EXPECT_EQ(read_pixel(local, 0), 0xFF112233u);
  EXPECT_EQ(read_pixel(local, 4), 0xFFFFFFFFu);
  for (std::size_t index = 1; index < 9; ++index) {
    if (index != 4) {
      EXPECT_EQ(read_pixel(local, index), 0xFF000000u);
    }
  }
}

TEST(WindowsCursorImage, RejectsMalformedCursorShapes) {
  platf::dxgi::normalized_cursor_shape_t normalized;

  DXGI_OUTDUPL_POINTER_SHAPE_INFO monochrome {};
  monochrome.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
  monochrome.Width = 9;
  monochrome.Height = 4;
  monochrome.Pitch = 1;
  EXPECT_FALSE(platf::dxgi::normalize_cursor_shape(
    std::vector<std::uint8_t>(4),
    monochrome,
    true,
    normalized
  ));

  DXGI_OUTDUPL_POINTER_SHAPE_INFO color {};
  color.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
  color.Width = 2;
  color.Height = 2;
  color.Pitch = 8;
  EXPECT_FALSE(platf::dxgi::normalize_cursor_shape(
    std::vector<std::uint8_t>(15),
    color,
    true,
    normalized
  ));
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
