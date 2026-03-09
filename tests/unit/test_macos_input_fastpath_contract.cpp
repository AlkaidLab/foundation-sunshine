#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {
  std::string
  read_file(const std::string &path) {
    std::ifstream file(path);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
  }
}

TEST(MacOSInputFastPathContract, MouseButtonsBypassTaskPoolOnMacOS) {
  const std::string source = read_file("src/input.cpp");

  ASSERT_FALSE(source.empty());

  const std::regex button_fast_path(
    R"(case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:\s*case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:\s*\{[\s\S]*?passthrough\(input,\s*\(PNV_MOUSE_BUTTON_PACKET\)\s*payload\);[\s\S]*?return;)"
  );

  EXPECT_TRUE(std::regex_search(source, button_fast_path))
    << "macOS fast-path routing should handle mouse button packets before "
       "queueing so the dedicated mouse thread sees button state changes in "
       "the same ordering as move events.";
}

TEST(MacOSInputFastPathContract, MoveMouseRefreshesCursorWhenCacheIsCold) {
  const std::string source = read_file("src/platform/macos/input.cpp");

  ASSERT_FALSE(source.empty());

  const std::regex move_mouse_cold_cache(
    R"(move_mouse\([\s\S]*?const auto current = macos_input->position_cache_valid \? macos_input->cached_mouse_position : get_mouse_loc\(input\);)"
  );

  EXPECT_TRUE(std::regex_search(source, move_mouse_cold_cache))
    << "move_mouse() should fall back to get_mouse_loc() until the cursor cache "
       "has been initialized, otherwise the wake-up nudge can jump from {0,0}.";
}

TEST(MacOSInputFastPathContract, RunNsAppLoopDoesNotReplaceTrayDelegate) {
  const std::string source = read_file("src/platform/macos/misc.mm");

  ASSERT_FALSE(source.empty());

  const std::regex run_loop_sets_delegate(
    R"(run_nsapp_loop\(\)\s*\{[\s\S]*?\[app setDelegate:)"
  );

  EXPECT_FALSE(std::regex_search(source, run_loop_sets_delegate))
    << "run_nsapp_loop() should not replace the NSApplication delegate, because "
       "the tray backend uses that delegate for menu callbacks on macOS.";
}

TEST(MacOSInputFastPathContract, CursorVisibilityUsesRetainedCaptureReference) {
  const std::string source = read_file("src/platform/macos/display.mm");

  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("std::mutex g_active_av_capture_mutex"), std::string::npos)
    << "Cursor visibility toggles should use a mutex-protected active capture reference.";
  EXPECT_NE(source.find("[capture retain]"), std::string::npos)
    << "set_cursor_visible() should retain the capture before releasing the lock.";
  EXPECT_NE(source.find("g_cursor_visible"), std::string::npos)
    << "Cursor visibility should be stored independently of the active capture so "
       "new macOS capture sessions inherit the current hidden/shown state.";
  EXPECT_NE(source.find("[display->av_capture setCursorVisible:g_cursor_visible ? YES : NO]"), std::string::npos)
    << "A newly registered active capture should immediately apply the current "
       "cursor visibility state instead of defaulting back to visible.";
}

TEST(MacOSInputFastPathContract, CursorVisibilityReconfiguresRunningCaptureSession) {
  const std::string source = read_file("src/platform/macos/av_video.m");

  ASSERT_FALSE(source.empty());

  const std::regex set_cursor_visible_reconfigures_session(
    R"(- \(void\)setCursorVisible:\(BOOL\)visible \{[\s\S]*?\[self\.session beginConfiguration\][\s\S]*?\[self\.screenInput setCapturesCursor:visible\][\s\S]*?\[self\.session commitConfiguration\][\s\S]*?\})"
  );

  EXPECT_TRUE(std::regex_search(source, set_cursor_visible_reconfigures_session))
    << "Changing cursor capture on an active AVCaptureScreenInput should be "
       "wrapped in an AVCaptureSession reconfiguration block so classic mouse "
       "mode updates become visible immediately while streaming.";
  EXPECT_NE(source.find("[self.videoOutputs count] > 0"), std::string::npos)
    << "Startup-time cursor initialization should avoid reconfiguring the "
       "capture session before any video outputs are attached, otherwise "
       "encoder probing can stall waiting for the first frame.";
  EXPECT_NE(source.find("self.screenInput.capturesCursor == visible"), std::string::npos)
    << "setCursorVisible() should be a no-op when the capture already matches "
       "the requested cursor state, so startup does not perturb a freshly "
       "initialized AVCaptureScreenInput for no behavioral change.";
}

TEST(MacOSInputFastPathContract, DummyImgTimesOutInsteadOfBlockingStartupForever) {
  const std::string source = read_file("src/platform/macos/display.mm");

  ASSERT_FALSE(source.empty());

  const std::regex dummy_img_timeout(
    R"(dummy_img\(img_t \*img\) override \{[\s\S]*?dispatch_time\(DISPATCH_TIME_NOW,\s*5\s*\*\s*NSEC_PER_SEC\)[\s\S]*?Timed out waiting for initial capture frame[\s\S]*?return -1;[\s\S]*?\})"
  );

  EXPECT_TRUE(std::regex_search(source, dummy_img_timeout))
    << "The startup encoder probe should time out waiting for the first "
       "captured frame instead of blocking forever, otherwise Sunshine never "
       "starts listening for Moonlight connections when AVFoundation stalls.";
}

TEST(MacOSInputFastPathContract, MacOSStartupDefersEncoderProbeUntilLaunch) {
  const std::string source = read_file("src/main.cpp");

  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("Deferring startup encoder probe on macOS"), std::string::npos)
    << "macOS startup should not block HTTP/NVHTTP listeners on synchronous "
       "encoder probing, because launch/resume already reprobe encoders after "
       "display preparation.";
}

TEST(MacOSInputFastPathContract, MouseModeVisibilityStateIsPerSession) {
  const std::string source = read_file("src/input.cpp");

  ASSERT_FALSE(source.empty());

  EXPECT_EQ(source.find("static bool server_cursor_visible"), std::string::npos)
    << "Cursor visibility state should be tracked per input session, not in "
       "function-local statics that leak across reconnects.";
  EXPECT_EQ(source.find("MODE_SWITCH_THRESHOLD"), std::string::npos)
    << "Mouse mode switching should not depend on a packet-count heuristic "
       "that can miss mixed REL/ABS streams from tablet clients.";
  EXPECT_NE(source.find("server_cursor_visible { true }"), std::string::npos)
    << "Each input session should start with cursor visibility state reset to visible.";
  EXPECT_NE(source.find("local_cursor_session { false }"), std::string::npos)
    << "Each input session should track whether the client entered a local-cursor session.";
  EXPECT_NE(source.find("Keeping server cursor hidden during local-cursor session"), std::string::npos)
    << "Absolute mouse packets should not re-show the server cursor after a "
       "tablet client has already entered local-cursor mode.";
  EXPECT_NE(source.find("Shortcut toggled streamed cursor mode"), std::string::npos)
    << "The existing cursor shortcut should also synchronize macOS cursor-mode "
       "state so users can deterministically switch back to classic system mouse mode.";
  EXPECT_NE(source.find("display_cursor = false;"), std::string::npos)
    << "Automatic entry into local mouse mode should update the global cursor "
       "visibility flag so manual toggles do not invert the wrong state.";
  EXPECT_NE(source.find("display_cursor = true;"), std::string::npos)
    << "Returning to classic system cursor mode should restore the global "
       "cursor visibility flag so later toggles stay in sync.";
  EXPECT_EQ(source.find("LOCAL_CURSOR_REL_SILENCE_BEFORE_REMOTE_RESTORE"), std::string::npos)
    << "Sunshine should not guess classic-vs-local cursor mode from ABS packet "
       "timing, because Moonlight V+ uses absolute packets for both classic "
       "mouse mode and local cursor rendering.";
}

TEST(MacOSInputFastPathContract, MacOSSupportsExplicitCursorModeShortcuts) {
  const std::string source = read_file("src/input.cpp");

  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("set_streamed_cursor_mode"), std::string::npos)
    << "macOS should expose a helper that sets streamed cursor visibility "
       "deterministically instead of only supporting a toggle.";
  EXPECT_NE(source.find("case 0x4D /* VKEY_M */"), std::string::npos)
    << "Sunshine should expose an explicit shortcut for classic system mouse "
       "mode so clients can request host cursor visibility without relying on "
       "packet-type heuristics.";
  EXPECT_NE(source.find("case 0x48 /* VKEY_H */"), std::string::npos)
    << "Sunshine should expose an explicit shortcut for local cursor mode so "
       "clients can hide the host cursor deterministically when entering local "
       "pointer rendering.";
}

TEST(MacOSInputFastPathContract, SunshineExecutableKeepsStableBuildIdentity) {
  const std::string source = read_file("cmake/targets/common.cmake");

  ASSERT_FALSE(source.empty());

  const std::regex versioned_executable_output(
    R"(set_target_properties\(sunshine PROPERTIES[\s\S]*?\bVERSION\s+\$\{PROJECT_VERSION\})"
  );

  EXPECT_FALSE(std::regex_search(source, versioned_executable_output))
    << "The sunshine executable should not use the VERSION target property, "
       "because macOS emits a versioned real binary and a symlink, which "
       "breaks stable TCC identity for synthesized input permissions.";
}
