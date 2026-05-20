#include "src/alkaidlab_session_bridge.h"

extern "C" {
#include "third-party/moonlight-common-c/src/Session.h"
}

#include <gtest/gtest.h>

namespace {

LI_SESSION
make_base_session() {
  LI_SESSION session {};
  LiInitializeSession(&session);
  session.logicalSessionKey = 0x1010;
  session.runtimeId = 0x2020;
  session.launchSessionId = 3;
  session.controlGeneration = 4;
  alk_session_copy_string(session.sessionId.value,
                          sizeof(session.sessionId.value),
                          "session-bridge-test");
  session.client.clientKey = 0x1111;
  session.client.deviceKey = 0x2222;
  session.client.participantKey = 0x3333;
  alk_session_copy_string(session.client.displayName,
                          sizeof(session.client.displayName),
                          "Moonlight macOS Enhanced");
  alk_session_copy_string(session.host.displayName,
                          sizeof(session.host.displayName),
                          "Foundation Sunshine");
  return session;
}

LI_SESSION_CURSOR_PLANE
make_full_cursor_plane() {
  LI_SESSION_CURSOR_PLANE cursor {};
  LiInitializeSessionCursorPlane(&cursor);
  cursor.flags = LI_SESSION_CURSOR_FLAG_VISIBLE |
                 LI_SESSION_CURSOR_FLAG_REMOTE_PLANE |
                 LI_SESSION_CURSOR_FLAG_SYSTEM_CURSOR_ACTIVE;
  cursor.cursorShapeId = 0xC0FFEE;
  cursor.renderPolicy = LI_SESSION_CURSOR_RENDER_POLICY_CLIENT_NATIVE;
  cursor.sizeSource = LI_SESSION_CURSOR_SIZE_SOURCE_ICON_BITMAP;
  cursor.confidencePpm = 987654;
  cursor.streamWidth = 3840;
  cursor.streamHeight = 2160;
  cursor.positionX = 1200;
  cursor.positionY = 800;
  cursor.displayWidth = 64;
  cursor.displayHeight = 64;
  cursor.hotspotX = 6;
  cursor.hotspotY = 4;
  cursor.bitmapWidth = 64;
  cursor.bitmapHeight = 64;
  cursor.bitmapStride = 256;
  cursor.bitmapFormat = LI_SESSION_CURSOR_BITMAP_FORMAT_BGRA;
  cursor.hostDpiScalePpm = 2000000;
  cursor.userScalePpm = 1000000;
  cursor.minClientPointSize = 8;
  cursor.maxClientPointSize = 128;
  cursor.epoch = 12;
  return cursor;
}

}  // namespace

TEST(AlkaidLabSessionBridgeTests, CursorModuleUpdateProjectsCompleteCursorPlane) {
  AlkSessionAdapterContext context {};
  auto base_session = make_base_session();
  ASSERT_TRUE(stream::alkaidlab_session_bridge::update_from_li_session(context, base_session));

  const auto cursor = make_full_cursor_plane();
  ASSERT_TRUE(stream::alkaidlab_session_bridge::update_cursor_plane(context, cursor));

  LI_SESSION projected {};
  LiInitializeSession(&projected);
  ASSERT_TRUE(stream::alkaidlab_session_bridge::project_to_li_session(context, projected));

  EXPECT_EQ(projected.cursorPlane.flags, cursor.flags);
  EXPECT_EQ(projected.cursorPlane.cursorShapeId, cursor.cursorShapeId);
  EXPECT_EQ(projected.cursorPlane.renderPolicy, cursor.renderPolicy);
  EXPECT_EQ(projected.cursorPlane.sizeSource, cursor.sizeSource);
  EXPECT_EQ(projected.cursorPlane.confidencePpm, cursor.confidencePpm);
  EXPECT_EQ(projected.cursorPlane.streamWidth, cursor.streamWidth);
  EXPECT_EQ(projected.cursorPlane.streamHeight, cursor.streamHeight);
  EXPECT_EQ(projected.cursorPlane.positionX, cursor.positionX);
  EXPECT_EQ(projected.cursorPlane.positionY, cursor.positionY);
  EXPECT_EQ(projected.cursorPlane.displayWidth, cursor.displayWidth);
  EXPECT_EQ(projected.cursorPlane.displayHeight, cursor.displayHeight);
  EXPECT_EQ(projected.cursorPlane.hotspotX, cursor.hotspotX);
  EXPECT_EQ(projected.cursorPlane.hotspotY, cursor.hotspotY);
  EXPECT_EQ(projected.cursorPlane.bitmapWidth, cursor.bitmapWidth);
  EXPECT_EQ(projected.cursorPlane.bitmapHeight, cursor.bitmapHeight);
  EXPECT_EQ(projected.cursorPlane.bitmapStride, cursor.bitmapStride);
  EXPECT_EQ(projected.cursorPlane.bitmapFormat, cursor.bitmapFormat);
  EXPECT_EQ(projected.cursorPlane.hostDpiScalePpm, cursor.hostDpiScalePpm);
  EXPECT_EQ(projected.cursorPlane.userScalePpm, cursor.userScalePpm);
  EXPECT_EQ(projected.cursorPlane.minClientPointSize, cursor.minClientPointSize);
  EXPECT_EQ(projected.cursorPlane.maxClientPointSize, cursor.maxClientPointSize);
  EXPECT_EQ(projected.cursorPlane.epoch, cursor.epoch);
}
