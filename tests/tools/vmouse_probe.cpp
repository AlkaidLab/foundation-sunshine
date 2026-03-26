#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <src/platform/windows/virtual_mouse.h>

namespace {
  struct options_t {
    bool list_only = false;
    bool require_device = false;
    bool require_events = false;
    bool send_test_sequence = false;
    bool quiet = false;
    DWORD timeout_ms = 5000;
    std::string match_substring = "VID_1ACE&PID_0002";
  };

  struct raw_device_t {
    HANDLE handle {};
    std::string name;
  };

  struct probe_result_t {
    bool matched_device_present = false;
    std::size_t matched_device_count = 0;
    std::size_t total_event_count = 0;
    std::size_t matched_event_count = 0;
    bool send_sequence_ok = false;
  };

  std::string
  to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  std::string
  wide_to_utf8(const std::wstring &wstr) {
    if (wstr.empty()) {
      return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
      return {};
    }

    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
  }

  std::vector<raw_device_t>
  enumerate_raw_mouse_devices() {
    UINT device_count = 0;
    if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) != 0) {
      return {};
    }

    std::vector<RAWINPUTDEVICELIST> raw_devices(device_count);
    if (GetRawInputDeviceList(raw_devices.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
      return {};
    }

    std::vector<raw_device_t> result;
    for (const auto &device: raw_devices) {
      if (device.dwType != RIM_TYPEMOUSE) {
        continue;
      }

      UINT name_length = 0;
      if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, nullptr, &name_length) != 0 || name_length == 0) {
        continue;
      }

      std::wstring device_name(name_length, L'\0');
      if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, device_name.data(), &name_length) == static_cast<UINT>(-1)) {
        continue;
      }

      if (!device_name.empty() && device_name.back() == L'\0') {
        device_name.pop_back();
      }

      result.push_back(raw_device_t {
        device.hDevice,
        wide_to_utf8(device_name),
      });
    }

    return result;
  }

  bool
  contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    return to_lower(std::string(haystack)).find(to_lower(std::string(needle))) != std::string::npos;
  }

  void
  sleep_ms(DWORD duration_ms) {
    ::Sleep(duration_ms);
  }

  class raw_input_probe_t {
  public:
    raw_input_probe_t(std::string match_substring, bool quiet):
        _match_substring(std::move(match_substring)),
        _quiet(quiet) {}

    bool
    initialize() {
      _devices = enumerate_raw_mouse_devices();
      for (const auto &device: _devices) {
        if (contains_case_insensitive(device.name, _match_substring)) {
          _matched_devices.insert(device.handle);
        }
      }

      WNDCLASSW wc {};
      wc.lpfnWndProc = &raw_input_probe_t::wnd_proc_setup;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"SunshineVMouseProbeWindow";

      if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
      }

      _window = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Sunshine VMouse Probe",
        WS_OVERLAPPED,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        wc.hInstance,
        this);

      if (_window == nullptr) {
        return false;
      }

      RAWINPUTDEVICE rid {};
      rid.usUsagePage = 0x01;
      rid.usUsage = 0x02;
      rid.dwFlags = RIDEV_INPUTSINK;
      rid.hwndTarget = _window;

      return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
    }

    ~raw_input_probe_t() {
      if (_window != nullptr) {
        DestroyWindow(_window);
      }
    }

    probe_result_t
    run(DWORD timeout_ms) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
      MSG msg {};

      while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        sleep_ms(10);
      }

      return probe_result_t {
        !_matched_devices.empty(),
        _matched_devices.size(),
        _total_event_count,
        _matched_event_count,
        false,
      };
    }

  private:
    static LRESULT CALLBACK
    wnd_proc_setup(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
      if (msg == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        auto *self = static_cast<raw_input_probe_t *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
      }

      auto *self = reinterpret_cast<raw_input_probe_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
      }

      return self->wnd_proc(hwnd, msg, wparam, lparam);
    }

    LRESULT
    wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
      if (msg != WM_INPUT) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
      }

      UINT size = 0;
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return 0;
      }

      std::vector<std::byte> buffer(size);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return 0;
      }

      const auto *raw = reinterpret_cast<const RAWINPUT *>(buffer.data());
      if (raw->header.dwType != RIM_TYPEMOUSE) {
        return 0;
      }

      ++_total_event_count;

      const bool matched = _matched_devices.contains(raw->header.hDevice);
      if (matched) {
        ++_matched_event_count;
      }

      if (!_quiet) {
        std::cout
          << "EVENT device=" << reinterpret_cast<std::uintptr_t>(raw->header.hDevice)
          << " matched=" << (matched ? 1 : 0)
          << " dx=" << raw->data.mouse.lLastX
          << " dy=" << raw->data.mouse.lLastY
          << " flags=" << raw->data.mouse.usButtonFlags
          << " wheel=" << static_cast<SHORT>(raw->data.mouse.usButtonData)
          << '\n';
      }

      return 0;
    }

    std::string _match_substring;
    bool _quiet;
    HWND _window = nullptr;
    std::vector<raw_device_t> _devices;
    std::unordered_set<HANDLE> _matched_devices;
    std::size_t _total_event_count = 0;
    std::size_t _matched_event_count = 0;
  };

  void
  print_usage() {
    std::cout
      << "Usage: vmouse_probe [--list-only] [--timeout-ms N] [--match-substring TEXT]\n"
      << "                    [--require-device] [--require-events] [--send-test-sequence] [--quiet]\n";
  }

  std::optional<options_t>
  parse_args(int argc, char **argv) {
    options_t options;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--list-only") {
        options.list_only = true;
      }
      else if (arg == "--require-device") {
        options.require_device = true;
      }
      else if (arg == "--require-events") {
        options.require_events = true;
      }
      else if (arg == "--send-test-sequence") {
        options.send_test_sequence = true;
      }
      else if (arg == "--quiet") {
        options.quiet = true;
      }
      else if (arg == "--timeout-ms" && i + 1 < argc) {
        options.timeout_ms = static_cast<DWORD>(std::stoul(argv[++i]));
      }
      else if (arg == "--match-substring" && i + 1 < argc) {
        options.match_substring = argv[++i];
      }
      else if (arg == "--help" || arg == "-h") {
        print_usage();
        return std::nullopt;
      }
      else {
        std::cerr << "Unknown argument: " << arg << '\n';
        print_usage();
        return std::nullopt;
      }
    }

    return options;
  }

  bool
  send_test_sequence() {
    auto device = platf::vmouse::create();
    if (!device.is_available()) {
      return false;
    }

    bool ok = true;
    ok = ok && device.move(48, 24);
    sleep_ms(30);
    ok = ok && device.button(platf::vmouse::BTN_LEFT, false);
    sleep_ms(30);
    ok = ok && device.button(platf::vmouse::BTN_LEFT, true);
    sleep_ms(30);
    ok = ok && device.scroll(120);
    return ok;
  }

  void
  print_enumeration(const std::string &match_substring) {
    const auto devices = enumerate_raw_mouse_devices();
    std::size_t matched = 0;

    for (const auto &device: devices) {
      const bool is_match = contains_case_insensitive(device.name, match_substring);
      matched += is_match ? 1U : 0U;
      std::cout << "DEVICE matched=" << (is_match ? 1 : 0)
                << " handle=" << reinterpret_cast<std::uintptr_t>(device.handle)
                << " name=" << device.name << '\n';
    }

    std::cout << "MATCHED_DEVICE_PRESENT=" << (matched > 0 ? 1 : 0) << '\n';
    std::cout << "MATCHED_DEVICE_COUNT=" << matched << '\n';
  }
}  // namespace

int
main(int argc, char **argv) {
  const auto parsed = parse_args(argc, argv);
  if (!parsed.has_value()) {
    return 1;
  }

  const auto &options = *parsed;

  if (options.list_only) {
    print_enumeration(options.match_substring);
    return 0;
  }

  raw_input_probe_t probe(options.match_substring, options.quiet);
  if (!probe.initialize()) {
    std::cerr << "Failed to initialize raw input probe\n";
    return 2;
  }

  std::atomic_bool send_sequence_ok = false;
  if (options.send_test_sequence) {
    sleep_ms(250);
    send_sequence_ok = send_test_sequence();
  }

  auto result = probe.run(options.timeout_ms);
  result.send_sequence_ok = send_sequence_ok.load();

  std::cout << "MATCHED_DEVICE_PRESENT=" << (result.matched_device_present ? 1 : 0) << '\n';
  std::cout << "MATCHED_DEVICE_COUNT=" << result.matched_device_count << '\n';
  std::cout << "TOTAL_EVENT_COUNT=" << result.total_event_count << '\n';
  std::cout << "MATCHED_EVENT_COUNT=" << result.matched_event_count << '\n';
  std::cout << "SEND_SEQUENCE_OK=" << (result.send_sequence_ok ? 1 : 0) << '\n';

  if (options.require_device && !result.matched_device_present) {
    return 3;
  }

  if (options.require_events && result.matched_event_count == 0) {
    return 4;
  }

  if (options.send_test_sequence && !result.send_sequence_ok) {
    return 5;
  }

  return 0;
}
