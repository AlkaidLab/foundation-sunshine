#pragma once
// Which running programs capture the desktop the way a VNC server does, without any Windows header: the installer shell
// lists the running executables (Toolhelp) and this decides, tests/unit/test_installer_remote_capture.cpp drives it.
//
// Why it matters: the web front end is a transparent window whose page lives in a WebView2 child drawn through
// DirectComposition. Reported 2026-11-24 with RealVNC Server: the installer showed up in the viewer as a thin strip titled
// "Foundation Sunshine" and nothing else. The native window (installer_shell_ui.inc) is opaque and drawn into the window's
// own surface, so every capture method sees it (checked under TightVNC, whose GDI capture shows the web page too but leaves
// the transparent margin black). An installer a person cannot see cannot be used, so a running VNC server selects the
// native window.
//
// What is established and what is not (docs/authority/source-index.tsv, keys vnc-* and ms-*):
//  - Documented: a window created without a redirection surface draws "by mechanisms other than surfaces"
//    (WS_EX_NOREDIRECTIONBITMAP, Microsoft Learn, Extended Window Styles); a screen BitBlt includes layered windows only with
//    CAPTUREBLT (BitBlt); RealVNC Server captures with DirectX by default and has CaptureMethod 1 (poll) and UseCaptureBlt
//    (semi-transparent windows) for the cases where that misses content (RealVNC Server Parameter Reference, 2026-11-18).
//  - Not established: which of these RealVNC's default path trips on for this window. RealVNC Server cannot be run without a
//    licence, so the strip was reproduced only without any VNC server (a stock 1123 installer on a GPU-less VM, in 2 of 3
//    starts) and the RealVNC symptom is taken from the report. The process list is therefore a guard, not a diagnosis.
//
// Names, each read from the vendor's own material:
//  - RealVNC: vncserver.exe (service and user mode) and vncagent.exe (the per-session capture and input agent), both in the
//    VNC-Server-7.13.1-Windows-64bit.msi file table; Starting and Stopping RealVNC Connect names vncserver.exe.
//  - TightVNC: tvnserver.exe (Server Command-Line Options 2.7, "Command-line options are accepted by the tvnserver.exe file").
//  - UltraVNC: winvnc.exe (Server Commandline Parameters; the service wrapper uvnc_service.exe starts winvnc.exe in the user
//    session, so winvnc.exe is the one that captures).
//  - TigerVNC: winvnc4.exe (the CMake target of win/winvnc, master d07907c8a). The Windows server is unmaintained upstream.
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace installer_remote_capture {

  /** Executable names (file name only) of VNC servers: RealVNC (server and per-session agent), TightVNC, UltraVNC, TigerVNC. */
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
