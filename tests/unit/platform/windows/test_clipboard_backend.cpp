/**
 * @file tests/unit/platform/windows/test_clipboard_backend.cpp
 * @brief Test Windows clipboard backend capability mapping.
 */

#ifdef _WIN32

  #include <src/platform/common.h>
  #include <src/platform/windows/clipboard.h>

  #include "../../../tests_common.h"

TEST(ClipboardBackendTests, MapsBackendCapabilitiesToSunshinePlatformFlags) {
  const auto caps = platf::clipboard::supported_capabilities();

  EXPECT_NE(caps & platf::platform_caps::clipboard_text, 0U);
  EXPECT_NE(caps & platf::platform_caps::clipboard_image, 0U);
}

#endif
