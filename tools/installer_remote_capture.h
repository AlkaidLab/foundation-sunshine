#pragma once
// Which running programs capture the desktop the way a VNC server does, without any Windows header: the installer shell
// lists the running executables (Toolhelp) and this decides, tests/unit/test_installer_remote_capture.cpp drives it.
//
// Why it matters: the web front end is a transparent window whose page lives in a DirectComposition visual. A VNC
// server that reads the window surfaces instead of the composed desktop shows only the stray frame strip of such a
// window (reported 2026-11-24 with RealVNC: the installer showed as a thin strip titled "Foundation Sunshine"). The
// native window (installer_shell_ui.inc) is opaque and drawn into the window surface; it is visible to every capture
// method (checked under TightVNC). An installer a person cannot see cannot be used, so a running VNC server selects
// the native window.
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace installer_remote_capture {

  /** Executable names (file name only) of VNC servers: RealVNC (service and per-session agent), TightVNC, UltraVNC, TigerVNC. */
  inline constexpr std::array<std::wstring_view, 5> kServers = {
    L"vncserver.exe", L"vncagent.exe", L"tvnserver.exe", L"winvnc.exe", L"winvnc4.exe",
  };

  inline bool
  is_vnc_server(std::wstring_view exe_file_name) {
    std::wstring lower(exe_file_name);
    for (auto &ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    for (const auto name : kServers)
      if (lower == name) return true;
    return false;
  }

}  // namespace installer_remote_capture
