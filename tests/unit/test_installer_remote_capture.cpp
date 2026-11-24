#include "tools/installer_remote_capture.h"

#include <gtest/gtest.h>

using installer_remote_capture::is_vnc_server;

TEST(InstallerRemoteCapture, KnownServersMatchWhateverTheCase) {
  EXPECT_TRUE(is_vnc_server(L"vncserver.exe"));
  EXPECT_TRUE(is_vnc_server(L"VNCAgent.exe"));
  EXPECT_TRUE(is_vnc_server(L"tvnserver.exe"));
  EXPECT_TRUE(is_vnc_server(L"WinVNC.exe"));
  EXPECT_TRUE(is_vnc_server(L"winvnc4.exe"));
}

TEST(InstallerRemoteCapture, OtherProgramsDoNot) {
  EXPECT_FALSE(is_vnc_server(L""));
  EXPECT_FALSE(is_vnc_server(L"vncviewer.exe"));
  EXPECT_FALSE(is_vnc_server(L"vncserver"));
  EXPECT_FALSE(is_vnc_server(L"notvncserver.exe"));
  EXPECT_FALSE(is_vnc_server(L"sunshine.exe"));
}
