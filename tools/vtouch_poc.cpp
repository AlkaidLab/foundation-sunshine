/**
 * @file tools/vtouch_poc.cpp
 * @brief POC: attach the virtual USB touchscreen through usbip-win2 and poke
 *        it interactively to verify Windows-native touch keyboard auto-invoke.
 *
 * Usage: vtouch-poc [--usbip <path to usbip.exe>]
 * Commands (stdin):
 *   tap <x> <y>      single tap in primary-screen pixels
 *   down <x> <y>     contact down
 *   move <x> <y>     contact move
 *   lift             contact up
 *   circle           draw a slow circle at screen centre for ~2 s
 *   status           print keyboard/window visibility
 *   quit             detach and exit
 *
 * Flags:
 *   --auto    run the full automated verdict (attach, foreground edit,
 *             synthetic taps, focus/click/keyboard-visibility checks)
 *   --mouse   attach a boot-mouse profile instead (control experiment;
 *             verified working: moves the real cursor)
 *   --log <path>   redirect all output to a file (use with elevated runs)
 *   --verbose hex-dump every USB/IP PDU
 *
 * Known issue (open): with the touchscreen profile, hover positioning works
 * (the cursor tracks the contact pixel exactly) but tip-down click delivery
 * does not happen: the target window receives no WM_LBUTTONDOWN /
 * WM_POINTERDOWN, focus never lands, and the touch keyboard never auto-
 * invokes.  Reproduced consistently on Windows 11 26200 + usbip-win2
 * 0.9.7.7 with four report-descriptor variants (parallel multi-touch,
 * vmulti-style hybrid, minimal single-touch, +Confidence bit).  Run
 * `--auto --log <file>` and read the `tap #` / `[click]` / `RESULT` lines;
 * a mouse-profile build (`--mouse`) proves the USB/IP -> VHCI -> HIDCLASS
 * -> input-stack path itself works.  Suspects: 26200 touch-stack policy
 * for virtual HID touchscreens, remote-session interference, hvctl
 * validation of non-certified digitizers.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <boost/system/error_code.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <src/remote_usb/loopback_usbip_bridge.h>
#include <src/remote_usb/virtual_touchscreen_device.h>

namespace fs = std::filesystem;

static bool
is_elevated() {
  FARPROC proc = GetProcAddress(GetModuleHandleW(L"shell32.dll"), "IsUserAnAdmin");
  if (!proc) {
    return false;
  }
  return ((BOOL (WINAPI *)()) proc)() != FALSE;
}

/**
 * Run a child process with stdout/stderr captured, killing it after
 * `timeout_ms`.  The pipe is drained on a helper thread so the timeout is
 * enforced even while the child keeps its output handles open.
 */
static std::string
run_tool(const std::wstring &cmdline, DWORD timeout_ms = 15000) {
  SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
    return "pipe failed";
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
  mutable_cmd.push_back(L'\0');
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &si, &pi)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return "create process failed";
  }
  CloseHandle(write_end);
  std::string out;
  std::thread reader([&]() {
    char buf[1024];
    DWORD read = 0;
    while (ReadFile(read_end, buf, sizeof(buf), &read, nullptr) && read != 0) {
      out.append(buf, read);
    }
  });
  bool timed_out = false;
  if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) {
    timed_out = true;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  // Once the child is gone the write end has no more owners (usbip.exe spawns
  // no grandchildren), so the reader observes EOF and exits.
  reader.join();
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(read_end);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  char tail[64];
  snprintf(tail, sizeof(tail), timed_out ? "[timeout, killed]" : "[exit %lu]", (unsigned long) code);
  out += tail;
  return out;
}

/** Dispatch queued window messages, then sleep until `ms` has elapsed. */
static void
pump_sleep(DWORD ms) {
  const ULONGLONG deadline = GetTickCount64() + ms;
  for (;;) {
    MSG msg;
    unsigned dispatched = 0;
    while (dispatched++ < 256 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      return;
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD) (deadline - now), QS_ALLINPUT);
  }
}

static bool
keyboard_visible(RECT *location = nullptr) {
  IFrameworkInputPane *pane = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FrameworkInputPane, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IFrameworkInputPane, reinterpret_cast<void **>(&pane)))) {
    RECT rect {};
    const bool visible = SUCCEEDED(pane->Location(&rect)) &&
                         rect.right > rect.left && rect.bottom > rect.top;
    pane->Release();
    if (location) {
      *location = rect;
    }
    if (visible) {
      return true;
    }
  }

  HWND tip = FindWindowW(L"IPTip_Main_Window", nullptr);
  return tip && IsWindowVisible(tip);
}

static void
print_status() {
  RECT rect {};
  const bool visible = keyboard_visible(&rect);
  printf("keyboard: visible=%d rect=(%ld,%ld)-(%ld,%ld)\n", (int) visible,
         (long) rect.left, (long) rect.top, (long) rect.right, (long) rect.bottom);
}

// ---- auto mode: create a foreground edit window, tap it, watch the keyboard ----

static HWND g_host = nullptr, g_edit = nullptr;
static std::atomic_bool g_imported { false };  // written on the bridge I/O thread
static bool g_verbose = false;
static int g_raw_input_frames = 0;

static void
print_raw_digitizer_count(const char *tag) {
  UINT count = 0;
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) {
    printf("[raw] %s: device list unavailable\n", tag);
    return;
  }
  std::vector<RAWINPUTDEVICELIST> devs(count);
  GetRawInputDeviceList(devs.data(), &count, sizeof(RAWINPUTDEVICELIST));
  int digitizers = 0;
  for (const auto &d : devs) {
    RID_DEVICE_INFO info {};
    UINT size = sizeof(info);
    if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &size) > 0 &&
        info.hid.usUsagePage == 0x0D) {
      ++digitizers;
      wchar_t name[512] = {};
      UINT nsize = sizeof(name);
      GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICENAME, name, &nsize);
      printf("[raw] %s: digitizer vid=0x%04X pid=0x%04X usage=0x%02X path=%ls\n", tag,
             info.hid.dwVendorId, info.hid.dwProductId, info.hid.usUsage, name);
    }
  }
  printf("[raw] %s: %d digitizer(s) total\n", tag, digitizers);
}

static void
dump_pdu(const char *tag, const std::vector<std::uint8_t> &pdu) {
  if (!g_verbose) return;
  printf("%s %zu bytes:", tag, pdu.size());
  for (size_t i = 0; i < pdu.size() && i < 64; ++i) {
    if (i % 16 == 0) printf("\n  ");
    printf("%02X ", pdu[i]);
  }
  printf("\n");
}

static LRESULT CALLBACK
host_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_INPUT) {
    UINT size = 0;
    GetRawInputData((HRAWINPUT) l, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size > 0 && size <= 512) {
      static thread_local std::vector<BYTE> buf(512);
      if (GetRawInputData((HRAWINPUT) l, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) > 0) {
        auto *raw = (RAWINPUT *) buf.data();
        if (raw->header.dwType == RIM_TYPEHID) {
          ++g_raw_input_frames;
          if (g_verbose) {
            printf("[raw] WM_INPUT hid dwSize=%lu count=%lu first=%02X %02X %02X %02X\n",
                   (unsigned long) raw->data.hid.dwSizeHid,
                   (unsigned long) raw->data.hid.dwCount,
                   raw->data.hid.bRawData[0], raw->data.hid.bRawData[1],
                   raw->data.hid.bRawData[2], raw->data.hid.bRawData[3]);
          }
        }
      }
    }
  }
  if (m >= 0x0246 && m <= 0x024A) {  // WM_POINTERDOWN/UP/UPDATE/... window
    UINT32 pid = (UINT32) (w & 0xFFFF);
    POINTER_INFO pi {};
    if (GetPointerInfo(pid, &pi)) {
      printf("[ptr] msg=0x%03X id=%u type=%u flags=0x%X at (%ld,%ld)\n",
             m, pid, pi.pointerType, pi.pointerFlags,
             (long) pi.ptPixelLocation.x, (long) pi.ptPixelLocation.y);
    }
    else {
      printf("[ptr] msg=0x%03X id=%u GetPointerInfo failed %lu\n", m, pid, GetLastError());
    }
  }
  return DefWindowProcW(h, m, w, l);
}

/** Returns true when the touch keyboard window becomes visible. */
static bool
wait_keyboard_visible(int timeout_ms) {
  for (int i = 0; i < timeout_ms / 250; ++i) {
    if (keyboard_visible()) {
      return true;
    }
    pump_sleep(250);
  }
  return false;
}

static int g_clicks = 0;
static WNDPROC g_old_edit_proc = nullptr;

static LRESULT CALLBACK
edit_proc_hook(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == 0x0201 || m == 0x0202 || (m >= 0x0240 && m <= 0x025F)) {
    ++g_clicks;
    printf("[click] edit got msg 0x%03X (total %d)\n", m, g_clicks);
  }
  return CallWindowProcW(g_old_edit_proc, h, m, w, l);
}

static void
create_test_window() {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = host_proc;
  wc.lpszClassName = L"VtouchPocHost";
  wc.hInstance = GetModuleHandleW(nullptr);
  RegisterClassW(&wc);
  g_host = CreateWindowExW(0, wc.lpszClassName, L"Vtouch POC", WS_OVERLAPPEDWINDOW,
                           200, 200, 640, 320, nullptr, nullptr, wc.hInstance, nullptr);
  g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"click target",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           10, 10, 600, 48, g_host, nullptr, wc.hInstance, nullptr);
  g_old_edit_proc = (WNDPROC) SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR) edit_proc_hook);
  ShowWindow(g_host, SW_SHOW);
}

static void
StealForeground() {
  HWND fg = GetForegroundWindow();
  DWORD fg_thread = GetWindowThreadProcessId(fg, nullptr);
  DWORD cur = GetCurrentThreadId();
  AttachThreadInput(cur, fg_thread, TRUE);
  SetForegroundWindow(g_host);
  SetFocus(g_host);
  AttachThreadInput(cur, fg_thread, FALSE);
}

static int
run_auto(remote_usb::virtual_touchscreen_device &dev, int hid_w, int hid_h) {
  // Source domain: physical screen pixels (GetWindowRect / SetCursorPos are
  // physical in a per-monitor-v2 aware process).  Target domain: the HID
  // digitizer's declared logical range.
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);
  WNDCLASSW wc = {};
  wc.lpfnWndProc = host_proc;
  wc.lpszClassName = L"VtouchPocHost";
  wc.hInstance = GetModuleHandleW(nullptr);
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    printf("[auto] RegisterClass failed err=%lu\n", GetLastError());
    return 1;
  }
  g_host = CreateWindowExW(0, wc.lpszClassName, L"Vtouch POC", WS_OVERLAPPEDWINDOW,
                           200, 200, 640, 320, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_host) {
    printf("[auto] CreateWindow(host) failed err=%lu\n", GetLastError());
    return 1;
  }
  g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"click target",
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           10, 10, 600, 48, g_host, nullptr, wc.hInstance, nullptr);
  if (!g_edit) {
    printf("[auto] CreateWindow(edit) failed err=%lu\n", GetLastError());
    DestroyWindow(g_host);
    g_host = nullptr;
    return 1;
  }
  g_old_edit_proc = (WNDPROC) SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR) edit_proc_hook);
  ShowWindow(g_host, SW_SHOW);
  EnableMouseInPointer(TRUE);  // observe WM_POINTER with pointerType

  // Sink for digitizer raw input so we can see whether our HID frames reach
  // the input stack at all.
  RAWINPUTDEVICE rid {};
  rid.usUsagePage = 0x0D;
  rid.usUsage = 0x04;  // touchscreen
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = g_host;
  if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
    printf("[raw] RegisterRawInputDevices failed err=%lu\n", GetLastError());
  }

  // Activate the host but leave the edit unfocused; the touch must focus it.
  HWND fg = GetForegroundWindow();
  DWORD fg_thread = GetWindowThreadProcessId(fg, nullptr);
  DWORD cur = GetCurrentThreadId();
  AttachThreadInput(cur, fg_thread, TRUE);
  SetForegroundWindow(g_host);
  SetFocus(g_host);
  AttachThreadInput(cur, fg_thread, FALSE);
  pump_sleep(400);

  RECT r {};
  if (!GetWindowRect(g_edit, &r) || IsRectEmpty(&r)) {
    printf("[auto] GetWindowRect(edit) failed err=%lu rect=(%ld,%ld)-(%ld,%ld)\n",
           GetLastError(), (long) r.left, (long) r.top, (long) r.right, (long) r.bottom);
    return 1;
  }
  int x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
  printf("[auto] foreground set; tapping edit centre at (%d,%d)\n", x, y);

  auto to_digitizer = [&](int px, int py) {
    dev.update_contacts({ remote_usb::touchscreen_contact {
      1,
      (std::uint16_t) ((std::int64_t) px * hid_w / screen_w),
      (std::uint16_t) ((std::int64_t) py * hid_h / screen_h),
      100, true, true } });
  };
  // Three tap attempts; each checks whether the edit gains keyboard focus.
  bool ever_focused = false;
  for (int attempt = 1; attempt <= 3 && !ever_focused; ++attempt) {
    StealForeground();
    pump_sleep(200);
    to_digitizer(x, y);
    pump_sleep(120);
    to_digitizer(x, y + 1);
    pump_sleep(60);
    to_digitizer(x, y);
    pump_sleep(120);
    dev.update_contacts({});
    pump_sleep(600);
    GUITHREADINFO gti {};
    gti.cbSize = sizeof(gti);
    GetGUIThreadInfo(0, &gti);
    bool focused = gti.hwndFocus == g_edit;
    ever_focused |= focused;
    POINT cur;
    GetCursorPos(&cur);
    printf("[auto] tap #%d: focus=%s cursor=(%ld,%ld) target=(%d,%d)\n",
           attempt, focused ? "ON EDIT" : "not on edit", (long) cur.x, (long) cur.y, x, y);
    print_status();
  }
  printf("[auto] edit focus ever gained: %s\n", ever_focused ? "YES" : "NO");

  // Real-click control: a genuine mouse click, same trigger as the human
  // verification, judged via IFrameworkInputPane.
  {
    StealForeground();
    Sleep(200);
    RECT rr;
    GetWindowRect(g_edit, &rr);
    int mx = (rr.left + rr.right) / 2, my = (rr.top + rr.bottom) / 2;
    SetCursorPos(mx, my);
    Sleep(150);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    pump_sleep(60);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    pump_sleep(1000);
    GUITHREADINFO g2 {};
    g2.cbSize = sizeof(g2);
    GetGUIThreadInfo(0, &g2);
    RECT krect {};
    bool kv = keyboard_visible(&krect);
    printf("[realclick] click at (%d,%d): focus on edit=%s keyboard visible=%s rect=(%ld,%ld)-(%ld,%ld)\n",
           mx, my, g2.hwndFocus == g_edit ? "YES" : "no", kv ? "YES" : "no",
           (long) krect.left, (long) krect.top, (long) krect.right, (long) krect.bottom);
  }

  printf("[auto] tap sent; waiting up to 8 s for the touch keyboard...\n");
  bool visible = wait_keyboard_visible(8000);
  printf("[auto] raw input frames seen at sink: %d\n", g_raw_input_frames);
  printf("[auto] RESULT: touch keyboard %s\n", visible ? "VISIBLE (auto-invoke works!)"
                                                      : "not visible (auto-invoke did not fire)");
  print_status();
  return visible ? 0 : 2;
}

int
wmain(int argc, wchar_t **argv) {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // Physical-pixel coordinates everywhere: without this, GetWindowRect /
  // SetCursorPos / synthetic touch coordinates are virtualised on scaled
  // displays and taps land on the wrong pixel.
  {
    using SetCtxFn = BOOL(WINAPI *)(void *);
    auto fn = (SetCtxFn) GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    void *per_monitor_v2 = (void *) -4;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (fn && fn(per_monitor_v2)) {
      printf("dpi awareness: per-monitor v2\n");
    }
    else {
      printf("dpi awareness: unavailable or set failed\n");
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::wstring usbip_path;
  bool auto_mode = false;
  bool mouse_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--usbip") == 0 && i + 1 < argc) {
      usbip_path = argv[++i];
    }
    else if (wcscmp(argv[i], L"--auto") == 0) {
      auto_mode = true;
    }
    else if (wcscmp(argv[i], L"--verbose") == 0) {
      g_verbose = true;
    }
    else if (wcscmp(argv[i], L"--mouse") == 0) {
      mouse_mode = true;
    }
    else if (wcscmp(argv[i], L"--log") == 0 && i + 1 < argc) {
      _wfreopen(argv[++i], L"w", stdout);
      setvbuf(stdout, nullptr, _IONBF, 0);
    }
  }
  if (usbip_path.empty()) {
    usbip_path = L"usbip.exe";
  }

  if (!is_elevated()) {
    printf("not elevated; the attach step will request UAC.\n");
  }

  remote_usb::virtual_touchscreen_device::config cfg;
  cfg.mouse_mode = mouse_mode;
  cfg.product_id = mouse_mode ? 0x5402 : 0x5401;
  cfg.width_px = 1920;
  cfg.height_px = 1080;
  remote_usb::virtual_touchscreen_device dev(cfg);

  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code ec;

  auto callbacks = remote_usb::callbacks {};
  callbacks.on_request = [&dev](std::vector<std::uint8_t> pdu) {
    dump_pdu("REQ <<", pdu);
    return dev.handle_request(pdu);
  };
  callbacks.on_imported = []() {
    g_imported.store(true);
    printf(">>> vhci imported the device\n");
  };
  callbacks.on_closed = [&dev](remote_usb::close_reason reason) {
    dev.reset();
    printf(">>> bridge closed (%d)\n", (int) reason);
  };

  auto endpoint = bridge.start(dev.info(), std::move(callbacks), ec);
  if (!endpoint) {
    printf("bridge start failed: %s\n", ec.message().c_str());
    return 1;
  }
  dev.set_send_reply([&bridge](std::vector<std::uint8_t> wire) {
    dump_pdu("REP >>", wire);
    bridge.send_reply(std::move(wire));
  });

  printf("bridge listening on %s:%u busid=%s\n", endpoint->address.c_str(),
         endpoint->port, endpoint->busid.c_str());

  // Attach through usbip-win2.  The attach command needs admin rights; when
  // we are not elevated, launch it via UAC and wait for the IMPORT request.
  {
    const std::wstring args = L"--tcp-port " + std::to_wstring(endpoint->port) +
                              L" attach --remote " +
                              std::wstring(endpoint->address.begin(), endpoint->address.end()) +
                              L" --bus-id " +
                              std::wstring(endpoint->busid.begin(), endpoint->busid.end()) +
                              L" --terse";
    if (is_elevated()) {
      std::string out = run_tool(L"\"" + usbip_path + L"\" " + args);
      printf("usbip attach: %s\n", out.c_str());
    }
    else {
      // Launch usbip.exe itself (never a shell) so the caller-supplied path
      // cannot be interpreted as command syntax under elevation.  Its output
      // is not captured; success is observed through on_imported below.
      printf("not elevated; requesting UAC for the attach command...\n");
      SHELLEXECUTEINFOW sei = {};
      sei.cbSize = sizeof(sei);
      sei.fMask = SEE_MASK_NOCLOSEPROCESS;
      sei.lpVerb = L"runas";
      sei.lpFile = usbip_path.c_str();
      sei.lpParameters = args.c_str();
      sei.nShow = SW_HIDE;
      if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        printf("UAC declined or launch failed (err=%lu); cannot attach without admin.\n",
               GetLastError());
        bridge.stop();
        return 1;
      }
      DWORD code = 0;
      if (WaitForSingleObject(sei.hProcess, 30000) == WAIT_TIMEOUT) {
        printf("usbip attach still running after 30 s; continuing to wait for import.\n");
      }
      else if (GetExitCodeProcess(sei.hProcess, &code)) {
        printf("usbip attach exit code: %lu\n", (unsigned long) code);
      }
      CloseHandle(sei.hProcess);
    }
  }

  // The bridge answers OP_REQ_IMPORT itself; wait for its callback.  The
  // VHCI service retries attach attempts in the background, so be generous.
  for (int i = 0; i < 600 && !g_imported.load(); ++i) {
    Sleep(100);
  }
  if (!g_imported.load()) {
    printf("device was not imported (attach failed or timed out).\n");
    bridge.stop();
    return 1;
  }
  printf("device imported by VHCI; checking raw input device list...\n");
  Sleep(1500);  // let HIDCLASS finish enumeration
  print_raw_digitizer_count("after-attach");
  // Fixed command text, launched without a shell; output goes to our log.
  printf("[pnp] %s\n",
         run_tool(L"powershell.exe -NoProfile -NonInteractive -Command "
                  L"\"Get-PnpDevice | Where-Object { $_.Present -and $_.InstanceId -match 'VID_5355' } "
                  L"| Format-List FriendlyName,Status,Class,InstanceId | Out-String -Width 200\"")
           .c_str());

  // Digitizer logical range == the descriptor's declared size.  With DPI
  // awareness on, GetSystemMetrics would return physical pixels of the whole
  // monitor, which is NOT the digitizer's coordinate basis.
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = GetSystemMetrics(SM_CYSCREEN);

  if (mouse_mode) {
    POINT start_pos;
    GetCursorPos(&start_pos);
    std::atomic_bool stop_sampling { false };
    std::mutex sample_mutex;
    double max_dist = 0;  // guarded by sample_mutex
    std::thread sampler([&]() {
      while (!stop_sampling.load()) {
        Sleep(40);
        POINT p;
        GetCursorPos(&p);
        const double d = hypot(p.x - start_pos.x, p.y - start_pos.y);
        std::lock_guard<std::mutex> lock(sample_mutex);
        if (d > max_dist) max_dist = d;
      }
    });
    printf("[mouse] wiggling the pointer for 4 s -- watch the cursor\n");
    for (int i = 0; i < 200; ++i) {
      double a = 2.0 * 3.141592653589793 * i / 50.0;
      dev.update_mouse((std::int8_t) (cos(a) * 6), (std::int8_t) (sin(a) * 6), 0);
      Sleep(20);
    }
    Sleep(300);
    stop_sampling.store(true);
    sampler.join();
    POINT now_pos;
    GetCursorPos(&now_pos);
    double final_dist = hypot(now_pos.x - start_pos.x, now_pos.y - start_pos.y);
    printf("[mouse] cursor moved: max=%.0f px final=%.0f px -> %s\n", max_dist, final_dist,
           (max_dist > 20 ? "INPUT PATH WORKS" : "cursor did not move"));
    printf("[mouse] wiggle done\n");

    // Secondary control experiment: point the cursor at the edit field and
    // send a left button press+release through the virtual mouse.  This
    // checks the click path of the virtual HID device; the touchscreen
    // profile's own tip-click issue is tracked separately.
    {
      create_test_window();
      Sleep(300);
      StealForeground();
      Sleep(200);
      SetForegroundWindow(g_host);
      SetFocus(g_edit);
      Sleep(200);
      // Nudge the cursor onto the edit centre with relative moves.
      POINT cur;
      GetCursorPos(&cur);
      RECT r;
      GetWindowRect(g_edit, &r);
      int tx = (r.left + r.right) / 2, ty = (r.top + r.bottom) / 2;
      while ((abs(cur.x - tx) > 60 || abs(cur.y - ty) > 60)) {
        dev.update_mouse((std::int8_t) ((tx - cur.x) / 8), (std::int8_t) ((ty - cur.y) / 8), 0);
        cur.x += (tx - cur.x) / 8;
        cur.y += (ty - cur.y) / 8;
        Sleep(15);
      }
      g_clicks = 0;
      dev.update_mouse(0, 0, 0x01);  // left down
      Sleep(60);
      dev.update_mouse(0, 0, 0x00);  // left up
      Sleep(600);
      GUITHREADINFO gti {};
      gti.cbSize = sizeof(gti);
      GetGUIThreadInfo(0, &gti);
      printf("[mouse] click messages seen at edit: %d, focus on edit: %s\n",
             g_clicks, gti.hwndFocus == g_edit ? "YES" : "no");
      printf("[mouse] VERDICT: virtual-mouse left-click %s\n",
             (g_clicks > 0 && gti.hwndFocus == g_edit) ? "WORKS"
             : (g_clicks > 0 ? "delivered but focus off" : "NOT delivered"));
    }

    Sleep(2000);
    bridge.stop();
    return 0;
  }

  if (auto_mode) {
    int rc = run_auto(dev, cfg.width_px, cfg.height_px);
    pump_sleep(3000);
    bridge.stop();
    return rc;
  }

  printf("screen %dx%d. commands: tap x y | down x y | move x y | lift | circle | status | quit\n",
         screen_w, screen_h);

  std::vector<remote_usb::touchscreen_contact> contacts;
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 1;
  bool held = false;

  auto to_digitizer = [&](int x, int y) {
    finger.x = (std::uint16_t) ((std::int64_t) (x - 0) * cfg.width_px / screen_w);
    finger.y = (std::uint16_t) ((std::int64_t) (y - 0) * cfg.height_px / screen_h);
  };

  char line[256];
  bool running = true;
  while (running && fgets(line, sizeof(line), stdin)) {
    char cmd[32] = { 0 };
    int x = 0, y = 0;
    if (sscanf(line, "%31s %d %d", cmd, &x, &y) < 1) {
      continue;
    }
    if (strcmp(cmd, "tap") == 0) {
      to_digitizer(x, y);
      finger.tip = true;
      finger.in_range = true;
      finger.pressure = 100;
      dev.update_contacts({ finger });
      Sleep(60);
      finger.tip = false;
      finger.in_range = false;
      finger.pressure = 0;
      dev.update_contacts({ finger });
      print_status();
    }
    else if (strcmp(cmd, "down") == 0) {
      to_digitizer(x, y);
      finger.tip = true;
      finger.in_range = true;
      finger.pressure = 100;
      held = true;
      dev.update_contacts({ finger });
      print_status();
    }
    else if (strcmp(cmd, "move") == 0) {
      to_digitizer(x, y);
      dev.update_contacts({ finger });
    }
    else if (strcmp(cmd, "lift") == 0) {
      if (held) {
        finger.tip = false;
        finger.in_range = false;
        finger.pressure = 0;
        held = false;
        dev.update_contacts({ finger });
      }
      print_status();
    }
    else if (strcmp(cmd, "circle") == 0) {
      const int cx = screen_w / 2, cy = screen_h / 2, r = screen_h / 8;
      for (int i = 0; i <= 128 && running; ++i) {
        double a = 2.0 * 3.141592653589793 * i / 128.0;
        to_digitizer(cx + (int) (r * cos(a)), cy + (int) (r * sin(a)));
        finger.tip = true;
        finger.in_range = true;
        finger.pressure = 100;
        dev.update_contacts({ finger });
        Sleep(12);
        if (i % 32 == 0) {
          print_status();
        }
      }
      finger.tip = false;
      finger.in_range = false;
      finger.pressure = 0;
      dev.update_contacts({ finger });
      print_status();
    }
    else if (strcmp(cmd, "status") == 0) {
      print_status();
    }
    else if (strcmp(cmd, "quit") == 0) {
      running = false;
    }
  }

  bridge.stop();
  printf("bye\n");
  if (SUCCEEDED(com_result)) {
    CoUninitialize();
  }
  return 0;
}
