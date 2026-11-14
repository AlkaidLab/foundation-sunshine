/**
 * @file tests/tools/ds5_native_usbip_smoke.cpp
 * @brief No-stream native USB/IP DualSense lifetime probe.
 *
 * The process owns one native client lease, keeps the USB/IP device attached
 * for a bounded interval, and releases it on exit.  A separate SSH command
 * can use usbip-win2, SetupAPI, HidD and the Windows audio class driver while
 * this process is holding the lease.
 */
#ifdef _WIN32

  #include <audioclient.h>
  #include <functiondiscoverykeys_devpkey.h>
  #include <hidsdi.h>
  #include <mmdeviceapi.h>
  #include <setupapi.h>

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cmath>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <functional>
  #include <memory>
  #include <string>
#include <string_view>
  #include <thread>
  #include <mutex>
  #include <vector>

#include <boost/log/sources/severity_logger.hpp>

  #include "src/ds5/tuning_profile.h"
  #include "src/ds5/feature_reports.h"
  #include "src/ds5/tuning_runtime.h"
  #include "src/platform/windows/ds5/ds5_native_client.h"
  #include "src/thread_safe.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> debug;

namespace {
  bool
  probe_hid() {
    GUID guid {};
    HidD_GetHidGuid(&guid);
    auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA entry { sizeof(entry) };
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<std::uint8_t> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
      const auto path = std::wstring(detail->DevicePath);
      if (path.find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
      auto file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (file == INVALID_HANDLE_VALUE) continue;
      found = true;
      {
        HIDD_ATTRIBUTES attributes { sizeof(attributes) };
        std::array<wchar_t, 128> product {}, manufacturer {};
        HidD_GetProductString(file, product.data(), static_cast<ULONG>(product.size() * sizeof(wchar_t)));
        HidD_GetManufacturerString(file, manufacturer.data(), static_cast<ULONG>(manufacturer.size() * sizeof(wchar_t)));
        HidD_GetAttributes(file, &attributes);
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps {};
        if (HidD_GetPreparsedData(file, &preparsed)) {
          HidP_GetCaps(preparsed, &caps);
          HidD_FreePreparsedData(preparsed);
        }
        std::wprintf(L"HID_IDENTITY product=\"%ls\" manufacturer=\"%ls\" vid=%04x pid=%04x usage_page=%04x usage=%04x input_len=%u output_len=%u\n",
          product.data(), manufacturer.data(), attributes.VendorID, attributes.ProductID, caps.UsagePage, caps.Usage,
          caps.InputReportByteLength, caps.OutputReportByteLength);
      }
      for (const auto report_id : { 0x05u, 0x09u, 0x20u }) {
        std::array<std::uint8_t, 64> feature {};
        feature[0] = static_cast<std::uint8_t>(report_id);
        const auto ok = HidD_GetFeature(file, feature.data(), static_cast<ULONG>(feature.size()));
        std::printf("FEATURE 0x%02x %s\n", report_id, ok ? "PASS" : "FAIL");
        if (report_id == 0x05u) {
          /* What SDL and Sony libraries do with it: the calibration has to leave the motion values usable. */
          const auto check = ::ds5::feature::check_calibration(feature.data(), 41);
          std::printf("CALIBRATION crc=%d usable=%d gyro_sensitivity=%.2f accel_sensitivity=%.3f %s\n", check.crc_ok ? 1 : 0,
            check.usable ? 1 : 0, check.gyro_sensitivity[0], check.accel_sensitivity[0], check.usable && check.crc_ok ? "PASS" : "FAIL");
        }
      }
      std::array<std::uint8_t, 64> input {};
      input[0] = 1;
      const auto input_report_ok = HidD_GetInputReport(file, input.data(), static_cast<ULONG>(input.size()));
      std::printf("INPUT_REPORT %s buttons=%u\n", input_report_ok ? "PASS" : "FAIL",
        static_cast<unsigned>(input[8]));
      const auto burst_ok = input_report_ok && input[8] == 0x28u;
      std::printf("INPUT_BURST %s\n", burst_ok ? "PASS" : "FAIL");
      DWORD input_bytes = 0;
      const auto read_ok = ReadFile(file, input.data(), static_cast<DWORD>(input.size()), &input_bytes, nullptr);
      std::printf("INPUT_READ %s bytes=%lu id=%u buttons=%u\n", read_ok ? "PASS" : "FAIL",
        static_cast<unsigned long>(input_bytes), static_cast<unsigned>(input[0]), static_cast<unsigned>(input[8]));
      const auto read_i16 = [&input](std::size_t offset) {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(input[offset]) |
                                         (static_cast<std::uint16_t>(input[offset + 1]) << 8));
      };
      /* 1400 degrees per second at 16.4 counts each is 22960; the saved gyro deadzone removes its own width from it. */
      const auto deadzone_dps = ds5::tuning::runtime::state().gyro_deadzone.load() * 2048.0f / 16.4f;
      const auto expected_gyro = static_cast<int>(std::lround((1400.0f - deadzone_dps) * 16.4f));
      const auto motion_ok = read_ok && std::abs(static_cast<int>(read_i16(16u)) - expected_gyro) <= 1 && read_i16(24u) == 8192;
      std::printf("MOTION_EXPECTED gyro_x=%d deadzone=%.4f\n", expected_gyro, ds5::tuning::runtime::state().gyro_deadzone.load());
      std::printf("MOTION_COUNTS %s gyro_x=%d accel_y=%d\n", motion_ok ? "PASS" : "FAIL",
        static_cast<int>(read_i16(16u)), static_cast<int>(read_i16(24u)));
      std::array<std::uint8_t, 48> output {};
      output[0] = 2;
      output[1] = 0x02;
      output[11] = 1;
      output[12] = 10;
      output[13] = 100;
      output[14] = 80;
      output[15] = 5;
      output[22] = 1;
      output[23] = 10;
      output[24] = 100;
      output[25] = 80;
      output[26] = 5;
      DWORD output_bytes = 0;
      const auto write_ok = WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &output_bytes, nullptr);
      std::printf("OUTPUT_WRITE %s bytes=%lu\n", write_ok ? "PASS" : "FAIL",
        static_cast<unsigned long>(output_bytes));
      CloseHandle(file);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
  }

  void
  probe_audio() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    IMMDeviceEnumerator *enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
      for (const auto flow : { eRender, eCapture }) {
        IMMDeviceCollection *collection = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) continue;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count; ++index) {
          IMMDevice *device = nullptr;
          if (FAILED(collection->Item(index, &device))) continue;
          IPropertyStore *store = nullptr;
          PROPVARIANT value;
          PropVariantInit(&value);
          if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
              SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
              value.vt == VT_LPWSTR && value.pwszVal) {
            std::wprintf(L"AUDIO_%s %ls\n", flow == eRender ? L"RENDER" : L"CAPTURE", value.pwszVal);
          }
          PropVariantClear(&value);
          if (store) store->Release();
          device->Release();
        }
        collection->Release();
      }
      enumerator->Release();
    }
    CoUninitialize();
  }

  /* Non-PCM feedback seen by the reader thread, for the scripted checks. */
  std::mutex g_log_mutex;
  std::vector<platf::gamepad_feedback_msg_t> g_log;

  /* Channel 3/4 content of the haptics PCM: zero crossings of the non-silent
   * stretches give the frequency the audio tool put on each channel. */
  struct pcm_analysis_t {
    unsigned long long active_frames { 0 };
    unsigned long long crossings[2] { 0, 0 };
    int last_sign[2] { 0, 0 };
    double energy[2] { 0.0, 0.0 };
    void
    add(const std::uint8_t *pcm, unsigned frames) {
      double chunk_energy = 0.0;
      for (unsigned frame = 0; frame != frames; ++frame)
        for (int channel = 0; channel != 2; ++channel) {
          const auto *sample = pcm + frame * 4 + channel * 2;
          const double value = static_cast<std::int16_t>(sample[0] | (sample[1] << 8));
          chunk_energy += value * value;
        }
      if (chunk_energy / (2.0 * frames) < 500.0 * 500.0) {
        last_sign[0] = last_sign[1] = 0;
        return;
      }
      for (unsigned frame = 0; frame != frames; ++frame)
        for (int channel = 0; channel != 2; ++channel) {
          const auto *sample = pcm + frame * 4 + channel * 2;
          const int value = static_cast<std::int16_t>(sample[0] | (sample[1] << 8));
          energy[channel] += static_cast<double>(value) * value;
          const int sign = value > 0 ? 1 : (value < 0 ? -1 : 0);
          if (sign != 0 && last_sign[channel] != 0 && sign != last_sign[channel]) ++crossings[channel];
          if (sign != 0) last_sign[channel] = sign;
        }
      active_frames += frames;
    }
  };
  pcm_analysis_t g_pcm;
  std::mutex g_pcm_mutex;

  double
  process_cpu_ms() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    const auto to_ms = [](const FILETIME &value) {
      return static_cast<double>((static_cast<unsigned long long>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10000.0;
    };
    return to_ms(kernel) + to_ms(user);
  }

  /* Opens the virtual HID for reading; null handle when it is not present. */
  HANDLE
  open_virtual_hid() {
    GUID guid {};
    HidD_GetHidGuid(&guid);
    auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD index = 0; result == INVALID_HANDLE_VALUE; ++index) {
      SP_DEVICE_INTERFACE_DATA entry { sizeof(entry) };
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<std::uint8_t> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
      if (std::wstring(detail->DevicePath).find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
      result = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
  }

  unsigned g_step_pass = 0;
  unsigned g_step_fail = 0;

  /* Write one output report to the virtual HID and wait for the matching
   * feedback message.  `match` inspects the messages logged after the write. */
  template <typename Match>
  void
  output_step(HANDLE file, const char *name, std::array<std::uint8_t, 48> report, Match match) {
    std::size_t before;
    {
      std::lock_guard lock(g_log_mutex);
      before = g_log.size();
    }
    DWORD written = 0;
    const auto ok = WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
    bool matched = false;
    for (int wait = 0; wait != 40 && !matched; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::lock_guard lock(g_log_mutex);
      for (auto index = before; index < g_log.size() && !matched; ++index) matched = match(g_log[index]);
    }
    std::printf("STEP %-34s write=%s %s\n", name, ok ? "ok" : "FAIL", matched ? "PASS" : "FAIL");
    (matched && ok ? g_step_pass : g_step_fail) += 1;
  }

  /* Write one report and require every predicate to match some message logged after
   * it (expect = true) or none of them (expect = false). */
  void
  output_step_all(HANDLE file, const char *name, std::array<std::uint8_t, 48> report,
    const std::vector<std::function<bool(const platf::gamepad_feedback_msg_t &)>> &predicates, bool expect) {
    std::size_t before;
    {
      std::lock_guard lock(g_log_mutex);
      before = g_log.size();
    }
    DWORD written = 0;
    const auto ok = WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool all = true, any = false;
    {
      std::lock_guard lock(g_log_mutex);
      for (const auto &predicate : predicates) {
        bool hit = false;
        for (auto index = before; index < g_log.size() && !hit; ++index) hit = predicate(g_log[index]);
        all = all && hit;
        any = any || hit;
      }
    }
    const bool passed = ok && (expect ? all : !any);
    std::printf("STEP %-34s write=%s %s\n", name, ok ? "ok" : "FAIL", passed ? "PASS" : "FAIL");
    (passed ? g_step_pass : g_step_fail) += 1;
  }

  /* Run an action (a tuning save, for example) and require every predicate to match some message
   * logged after it (expect = true) or none of them (expect = false). */
  void
  action_step(const char *name, const std::function<void()> &action,
    const std::vector<std::function<bool(const platf::gamepad_feedback_msg_t &)>> &predicates, bool expect) {
    std::size_t before;
    {
      std::lock_guard lock(g_log_mutex);
      before = g_log.size();
    }
    action();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool all = true, any = false;
    {
      std::lock_guard lock(g_log_mutex);
      for (const auto &predicate : predicates) {
        bool hit = false;
        for (auto index = before; index < g_log.size() && !hit; ++index) hit = predicate(g_log[index]);
        all = all && hit;
        any = any || hit;
      }
    }
    const bool passed = expect ? all : !any;
    std::printf("STEP %-34s %s\n", name, passed ? "PASS" : "FAIL");
    (passed ? g_step_pass : g_step_fail) += 1;
  }

  void
  feedback_steps() {
    using platf::gamepad_feedback_e;
    auto file = open_virtual_hid();
    if (file == INVALID_HANDLE_VALUE) {
      std::puts("STEP open FAIL no virtual HID");
      ++g_step_fail;
      return;
    }
    const auto report = [](std::uint8_t flag0, std::uint8_t flag1) {
      std::array<std::uint8_t, 48> value {};
      value[0] = 2;
      value[1] = flag0;
      value[2] = flag1;
      return value;
    };
    /* Rumble over the whole range: byte 3 is the right (high frequency) motor,
     * byte 4 the left (low frequency) motor; the host scales 8 to 16 bits. */
    for (int index = 0; index != 16; ++index) {
      const auto left = static_cast<std::uint8_t>(index * 17);
      const auto right = static_cast<std::uint8_t>(255 - index * 17);
      auto value = report(0x01, 0);
      value[3] = right;
      value[4] = left;
      char name[48];
      std::snprintf(name, sizeof name, "rumble left=%u right=%u", left, right);
      output_step(file, name, value, [&](const platf::gamepad_feedback_msg_t &message) {
        return message.type == gamepad_feedback_e::rumble && message.data.rumble.lowfreq == left * 257u &&
               message.data.rumble.highfreq == right * 257u;
      });
    }
    /* Adaptive triggers: every mode on each trigger, then both together. */
    const std::uint8_t modes[] = { 0x00, 0x01, 0x02, 0x06, 0x21, 0x25, 0x26 };
    std::uint8_t salt = 1;
    for (const auto mode : modes) {
      for (const auto which : { 0x04, 0x08, 0x0c }) {
        auto value = report(static_cast<std::uint8_t>(which), 0);
        std::array<std::uint8_t, 10> params {};
        for (std::size_t i = 0; i != params.size(); ++i) params[i] = static_cast<std::uint8_t>(salt * 3 + i);
        ++salt;
        if (which & 0x04) {
          value[11] = mode;
          std::copy(params.begin(), params.end(), value.begin() + 12);
        }
        if (which & 0x08) {
          value[22] = static_cast<std::uint8_t>(mode ^ 0x01);
          std::copy(params.begin(), params.end(), value.begin() + 23);
        }
        char name[48];
        std::snprintf(name, sizeof name, "trigger mode=0x%02x sides=0x%02x", mode, which);
        output_step(file, name, value, [&](const platf::gamepad_feedback_msg_t &message) {
          if (message.type != gamepad_feedback_e::set_adaptive_triggers) return false;
          const auto &data = message.data.adaptive_triggers;
          if (data.event_flags != which) return false;
          if ((which & 0x04) && (data.type_right != mode || data.right != params)) return false;
          if ((which & 0x08) && (data.type_left != static_cast<std::uint8_t>(mode ^ 0x01) || data.left != params)) return false;
          return true;
        });
      }
    }
    /* Haptics state (volume, source select, flags). */
    {
      auto value = report(0x03, 0x02);
      value[10] = 0x30;
      output_step(file, "haptics state byte5=0x30", value, [](const platf::gamepad_feedback_msg_t &message) {
        return message.type == gamepad_feedback_e::ds5_haptics_state && message.data.ds5_haptics_state[5] == 0x30 &&
               (message.data.ds5_haptics_state[2] & 0x02) != 0;
      });
      value = report(0x03, 0x40);
      value[37] = 0x55;
      output_step(file, "haptics state byte6=0x55", value, [](const platf::gamepad_feedback_msg_t &message) {
        return message.type == gamepad_feedback_e::ds5_haptics_state && message.data.ds5_haptics_state[6] == 0x55;
      });
      value = report(0x03, 0x20);
      value[40] = 0x01;
      output_step(file, "haptics state byte7=0x01", value, [](const platf::gamepad_feedback_msg_t &message) {
        return message.type == gamepad_feedback_e::ds5_haptics_state && message.data.ds5_haptics_state[7] == 0x01;
      });
    }
    /* Lightbar. */
    {
      auto value = report(0, 0x04);
      value[45] = 10;
      value[46] = 20;
      value[47] = 30;
      output_step(file, "lightbar rgb=10,20,30", value, [](const platf::gamepad_feedback_msg_t &message) {
        return message.type == gamepad_feedback_e::set_rgb_led && message.data.rgb_led.r == 10 &&
               message.data.rgb_led.g == 20 && message.data.rgb_led.b == 30;
      });
    }
    /* Game-style reports, as the Windows HID stack and titles send them (enable bits of the
     * corrected core: flag0 0x01 compat vibration, 0x02 haptics select, 0x04/0x08 triggers;
     * flag1 0x04 lightbar; flag2 0x04 improved rumble emulation). */
    {
      using msg_t = platf::gamepad_feedback_msg_t;
      const auto rumble_is = [](unsigned left, unsigned right) {
        return [=](const msg_t &m) { return m.type == gamepad_feedback_e::rumble && m.data.rumble.lowfreq == left * 257u && m.data.rumble.highfreq == right * 257u; };
      };
      const auto any_rumble = [](const msg_t &m) { return m.type == gamepad_feedback_e::rumble; };
      const auto motor_report = [](std::uint8_t flag0, std::uint8_t flag1, std::uint8_t right, std::uint8_t left) {
        std::array<std::uint8_t, 48> report {};
        report[0] = 2;
        report[1] = flag0;
        report[2] = flag1;
        report[3] = right;
        report[4] = left;
        return report;
      };
      /* Rumble that games end the way SDL does: a report with no enable bit, and one with haptics
       * select and zero values. */
      output_step_all(file, "game: rumble running", motor_report(0x01, 0, 40, 40), { rumble_is(40, 40) }, true);
      output_step_all(file, "game: empty report ends the rumble", motor_report(0, 0, 0, 0), { rumble_is(0, 0) }, true);
      output_step_all(file, "game: empty report while idle", motor_report(0, 0, 0, 0), { any_rumble }, false);
      output_step_all(file, "game: haptics select, no values, idle", motor_report(0x02, 0, 0, 0), { any_rumble }, false);
      /* A report with haptics select alone and motor bytes: nothing proves that it must not move the
       * motors, and the earlier guard (motor bits only) left such a game without rumble. */
      output_step_all(file, "game: haptics select with values", motor_report(0x02, 0, 90, 30), { rumble_is(30, 90) }, true);
      {
        auto value = motor_report(0, 0x04, 0, 0);
        value[45] = 4;
        value[46] = 5;
        value[47] = 6;
        output_step_all(file, "game: lightbar report keeps rumble", value, { any_rumble }, false);
      }
      output_step_all(file, "game: haptics select zero ends rumble", motor_report(0x02, 0, 0, 0), { rumble_is(0, 0) }, true);
      std::array<std::uint8_t, 48> value {};
      value[0] = 2;
      value[1] = 0x02;
      value[39] = 0x04;  // flag2: improved rumble emulation
      value[3] = 91;
      value[4] = 31;
      output_step_all(file, "game: improved rumble emulation", value,
        { [](const msg_t &m) { return m.type == gamepad_feedback_e::rumble && m.data.rumble.lowfreq == 31 * 257u && m.data.rumble.highfreq == 91 * 257u; } }, true);
      value = {};
      value[0] = 2;
      value[1] = 0xff;
      value[2] = 0xf7;
      value[39] = 0x07;
      value[3] = 61;  // right motor
      value[4] = 21;  // left motor
      value[11] = 0x26;
      for (int i = 0; i != 10; ++i) value[12 + i] = static_cast<std::uint8_t>(40 + i);
      value[22] = 0x25;
      for (int i = 0; i != 10; ++i) value[23 + i] = static_cast<std::uint8_t>(70 + i);
      value[45] = 1;
      value[46] = 2;
      value[47] = 3;
      output_step_all(file, "game: full 0xff/0xf7 report", value,
        { [](const msg_t &m) { return m.type == gamepad_feedback_e::rumble && m.data.rumble.lowfreq == 21 * 257u && m.data.rumble.highfreq == 61 * 257u; },
          [](const msg_t &m) { return m.type == gamepad_feedback_e::set_adaptive_triggers && m.data.adaptive_triggers.event_flags == 0x0c &&
                                      m.data.adaptive_triggers.type_right == 0x26 && m.data.adaptive_triggers.type_left == 0x25 &&
                                      m.data.adaptive_triggers.right[0] == 40 && m.data.adaptive_triggers.left[0] == 70; },
          [](const msg_t &m) { return m.type == gamepad_feedback_e::set_rgb_led && m.data.rgb_led.r == 1 && m.data.rgb_led.g == 2 && m.data.rgb_led.b == 3; } },
        true);
    }
    /* Player LEDs: a mask the game never set is not forwarded, a real one is, and then a zero is the
     * game's own clear. */
    {
      using msg_t = platf::gamepad_feedback_msg_t;
      const auto leds = [](std::uint8_t mask) {
        std::array<std::uint8_t, 48> report {};
        report[0] = 2;
        report[2] = 0x10;  // flag1: player LEDs
        report[44] = mask;
        return report;
      };
      const auto player_message = [](unsigned mask) {
        return [=](const msg_t &m) {
          return m.type == gamepad_feedback_e::ds5_output_extension && (m.data.ds5_output_extension.valid_mask & 0x01u) != 0u &&
                 m.data.ds5_output_extension.player_led_mask == mask;
        };
      };
      const auto any_player = [](const msg_t &m) {
        return m.type == gamepad_feedback_e::ds5_output_extension && (m.data.ds5_output_extension.valid_mask & 0x01u) != 0u;
      };
      output_step_all(file, "player LEDs: default zero mask", leds(0), { any_player }, false);
      output_step_all(file, "player LEDs: game sets mask 0x04", leds(0x04), { player_message(0x04) }, true);
      output_step_all(file, "player LEDs: game clears them", leds(0x00), { player_message(0x00) }, true);
    }
    /* A tuning save reaches the pad at once: the host calls the same hook the settings API reaches. */
    {
      using msg_t = platf::gamepad_feedback_msg_t;
      auto &state = ds5::tuning::runtime::state();
      const auto rgb_is = [](unsigned r, unsigned g, unsigned b) {
        return [=](const msg_t &m) { return m.type == gamepad_feedback_e::set_rgb_led && m.data.rgb_led.r == r && m.data.rgb_led.g == g && m.data.rgb_led.b == b; };
      };
      const auto any_rgb = [](const msg_t &m) { return m.type == gamepad_feedback_e::set_rgb_led; };
      /* The game's last colour is 1,2,3 (full report above). */
      action_step("lightbar push: fixed colour", [&] {
        state.lightbar_mode.store(1);
        state.lightbar_r.store(52);
        state.lightbar_g.store(199);
        state.lightbar_b.store(89);
        ds5::tuning::runtime::notify_published();
      }, { rgb_is(52, 199, 89) }, true);
      action_step("lightbar push: saved twice, sent once", [&] { ds5::tuning::runtime::notify_published(); }, { any_rgb }, false);
      action_step("lightbar push: off", [&] {
        state.lightbar_mode.store(2);
        ds5::tuning::runtime::notify_published();
      }, { rgb_is(0, 0, 0) }, true);
      action_step("lightbar push: follow game again", [&] {
        state.lightbar_mode.store(0);
        ds5::tuning::runtime::notify_published();
      }, { rgb_is(1, 2, 3) }, true);
    }
    CloseHandle(file);
    std::printf("STEPS pass=%u fail=%u\n", g_step_pass, g_step_fail);
  }

  /* Buttons, hat and triggers as the virtual HID reports them: every case is
   * submitted through the native client and the raw input report read back
   * with ReadFile.  Expected bytes follow the DualSense layout. */
  void
  buttons_probe(platf::ds5::native_client_t &client) {
    auto file = open_virtual_hid();
    if (file == INVALID_HANDLE_VALUE) {
      std::puts("BUTTONS FAIL no virtual HID");
      return;
    }
    std::mutex report_mutex;
    std::array<std::uint8_t, 64> last {};
    std::atomic_bool stop { false };
    std::thread reader([&] {
      std::array<std::uint8_t, 64> buffer {};
      while (!stop.load(std::memory_order_acquire)) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
        std::lock_guard lock(report_mutex);
        last = buffer;
      }
    });
    struct test_case_t {
      const char *name;
      std::uint32_t flags;
      std::uint8_t lt, rt;
      std::uint8_t byte8, byte9, byte10;  // expected report bytes 8, 9, 10
      std::uint8_t l2, r2;                // expected analog bytes 5 and 6
    };
    using namespace platf;
    const test_case_t cases[] = {
      { "none", 0, 0, 0, 0x08, 0x00, 0x00, 0, 0 },
      { "A=cross", A, 0, 0, 0x28, 0x00, 0x00, 0, 0 },
      { "B=circle", B, 0, 0, 0x48, 0x00, 0x00, 0, 0 },
      { "X=square", X, 0, 0, 0x18, 0x00, 0x00, 0, 0 },
      { "Y=triangle", Y, 0, 0, 0x88, 0x00, 0x00, 0, 0 },
      { "dpad up", DPAD_UP, 0, 0, 0x00, 0x00, 0x00, 0, 0 },
      { "dpad up+right", DPAD_UP | DPAD_RIGHT, 0, 0, 0x01, 0x00, 0x00, 0, 0 },
      { "dpad right", DPAD_RIGHT, 0, 0, 0x02, 0x00, 0x00, 0, 0 },
      { "dpad down+right", DPAD_DOWN | DPAD_RIGHT, 0, 0, 0x03, 0x00, 0x00, 0, 0 },
      { "dpad down", DPAD_DOWN, 0, 0, 0x04, 0x00, 0x00, 0, 0 },
      { "dpad down+left", DPAD_DOWN | DPAD_LEFT, 0, 0, 0x05, 0x00, 0x00, 0, 0 },
      { "dpad left", DPAD_LEFT, 0, 0, 0x06, 0x00, 0x00, 0, 0 },
      { "dpad up+left", DPAD_UP | DPAD_LEFT, 0, 0, 0x07, 0x00, 0x00, 0, 0 },
      { "L1", LEFT_BUTTON, 0, 0, 0x08, 0x01, 0x00, 0, 0 },
      { "R1", RIGHT_BUTTON, 0, 0, 0x08, 0x02, 0x00, 0, 0 },
      { "Back=Create", BACK, 0, 0, 0x08, 0x10, 0x00, 0, 0 },
      { "Start=Options", START, 0, 0, 0x08, 0x20, 0x00, 0, 0 },
      { "L3", LEFT_STICK, 0, 0, 0x08, 0x40, 0x00, 0, 0 },
      { "R3", RIGHT_STICK, 0, 0, 0x08, 0x80, 0x00, 0, 0 },
      { "Home=PS", HOME, 0, 0, 0x08, 0x00, 0x01, 0, 0 },
      { "touchpad click", TOUCHPAD_BUTTON, 0, 0, 0x08, 0x00, 0x02, 0, 0 },
      { "misc=mute", MISC_BUTTON, 0, 0, 0x08, 0x00, 0x04, 0, 0 },
      { "L2 below threshold", 0, 100, 0, 0x08, 0x00, 0x00, 100, 0 },
      { "L2 at threshold", 0, 128, 0, 0x08, 0x04, 0x00, 128, 0 },
      { "R2 full", 0, 0, 255, 0x08, 0x08, 0x00, 0, 255 },
      { "L2+R2+L1", LEFT_BUTTON, 255, 255, 0x08, 0x0d, 0x00, 255, 255 },
    };
    unsigned pass = 0, fail = 0;
    for (const auto &test : cases) {
      gamepad_state_t state {};
      state.buttonFlags = test.flags;
      state.lt = test.lt;
      state.rt = test.rt;
      client.submit_input(0, state);
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      std::array<std::uint8_t, 64> report;
      {
        std::lock_guard lock(report_mutex);
        report = last;
      }
      const bool ok = report[8] == test.byte8 && report[9] == test.byte9 && report[10] == test.byte10 &&
                      report[5] == test.l2 && report[6] == test.r2;
      std::printf("BUTTON %-22s got b8=%02x b9=%02x b10=%02x l2=%u r2=%u expect b8=%02x b9=%02x b10=%02x l2=%u r2=%u %s\n",
        test.name, report[8], report[9], report[10], report[5], report[6], test.byte8, test.byte9, test.byte10,
        test.l2, test.r2, ok ? "PASS" : "FAIL");
      (ok ? pass : fail) += 1;
    }
    stop.store(true, std::memory_order_release);
    client.submit_input(0, gamepad_state_t {});
    CancelIoEx(file, nullptr);
    reader.join();
    CloseHandle(file);
    std::printf("BUTTONS pass=%u fail=%u\n", pass, fail);
  }

  /* Two virtual pads at once: independent input, feedback and pairing identity. */
  std::mutex g_log1_mutex;
  std::vector<platf::gamepad_feedback_msg_t> g_log1;
  pcm_analysis_t g_pcm1;
  std::mutex g_pcm1_mutex;

  struct pad_handle_t {
    HANDLE file { INVALID_HANDLE_VALUE };
    int index { -1 };
    std::mutex mutex;
    std::array<std::uint8_t, 64> last {};
    std::atomic_bool stop { false };
    std::thread reader;
  };

  std::vector<HANDLE>
  open_all_virtual_hids() {
    std::vector<HANDLE> result;
    GUID guid {};
    HidD_GetHidGuid(&guid);
    auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return result;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA entry { sizeof(entry) };
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<std::uint8_t> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
      if (std::wstring(detail->DevicePath).find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
      auto file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (file != INVALID_HANDLE_VALUE) result.push_back(file);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
  }

  void
  multi_probe(platf::ds5::native_client_t &client) {
    auto handles = open_all_virtual_hids();
    std::printf("MULTI hid_devices=%zu\n", handles.size());
    if (handles.size() != 2) {
      std::puts("MULTI FAIL expected two virtual HID devices");
      for (auto handle : handles) CloseHandle(handle);
      return;
    }
    std::array<pad_handle_t, 2> pads;
    unsigned pass = 0, fail = 0;
    const auto check = [&](const char *name, bool ok) {
      std::printf("MULTI %-52s %s\n", name, ok ? "PASS" : "FAIL");
      (ok ? pass : fail) += 1;
    };
    for (std::size_t slot = 0; slot != 2; ++slot) {
      pads[slot].file = handles[slot];
      std::array<std::uint8_t, 64> feature {};
      feature[0] = 0x09;
      if (HidD_GetFeature(handles[slot], feature.data(), static_cast<ULONG>(feature.size()))) {
        pads[slot].index = static_cast<int>(feature[6]) - 1;  // pairing MAC ends in index + 1
      }
      std::printf("MULTI hid[%zu] pairing mac=%02x:%02x:%02x:%02x:%02x:%02x -> pad %d\n", slot, feature[1], feature[2],
        feature[3], feature[4], feature[5], feature[6], pads[slot].index);
    }
    check("two distinct pairing identities", pads[0].index != pads[1].index && pads[0].index >= 0 && pads[1].index >= 0);
    for (auto &pad : pads) {
      pad.reader = std::thread([&pad] {
        std::array<std::uint8_t, 64> buffer {};
        while (!pad.stop.load(std::memory_order_acquire)) {
          DWORD bytes = 0;
          if (!ReadFile(pad.file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
          std::lock_guard lock(pad.mutex);
          pad.last = buffer;
        }
      });
    }
    const auto pad_for = [&](int index) -> pad_handle_t & { return pads[0].index == index ? pads[0] : pads[1]; };
    const auto latest = [&](int index) {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      auto &pad = pad_for(index);
      std::lock_guard lock(pad.mutex);
      return pad.last;
    };
    // Independent input.
    platf::gamepad_state_t first {}, second {};
    first.buttonFlags = platf::A;
    second.buttonFlags = platf::B | platf::LEFT_BUTTON;
    second.lt = 255;
    client.submit_input(0, first);
    client.submit_input(1, second);
    auto report0 = latest(0), report1 = latest(1);
    check("pad 0 input: A only", report0[8] == 0x28 && report0[9] == 0x00 && report0[5] == 0);
    check("pad 1 input: B + L1 + L2", report1[8] == 0x48 && report1[9] == 0x05 && report1[5] == 255);
    first.buttonFlags = platf::Y;
    first.rt = 200;
    client.submit_input(0, first);
    second = {};
    client.submit_input(1, second);
    report0 = latest(0);
    report1 = latest(1);
    check("pad 0 changed alone: Y + R2", report0[8] == 0x88 && report0[9] == 0x08 && report0[6] == 200);
    check("pad 1 cleared alone", report1[8] == 0x08 && report1[9] == 0x00 && report1[5] == 0);
    // Independent feedback: rumble written to one pad reaches only that pad's queue.
    const auto rumble_to = [&](int index, std::uint8_t left, std::uint8_t right) {
      std::array<std::uint8_t, 48> output {};
      output[0] = 2;
      output[1] = 0x01;
      output[3] = right;
      output[4] = left;
      DWORD written = 0;
      WriteFile(pad_for(index).file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    };
    const auto count_rumble = [](std::mutex &mutex, std::vector<platf::gamepad_feedback_msg_t> &log, unsigned low, unsigned high) {
      std::lock_guard lock(mutex);
      unsigned hits = 0;
      for (const auto &message : log)
        if (message.type == platf::gamepad_feedback_e::rumble && message.data.rumble.lowfreq == low && message.data.rumble.highfreq == high) ++hits;
      return hits;
    };
    rumble_to(0, 100, 50);
    rumble_to(1, 200, 10);
    check("rumble 100/50 written to pad 0 reaches queue 0 only",
      count_rumble(g_log_mutex, g_log, 100 * 257, 50 * 257) >= 1 && count_rumble(g_log1_mutex, g_log1, 100 * 257, 50 * 257) == 0);
    check("rumble 200/10 written to pad 1 reaches queue 1 only",
      count_rumble(g_log1_mutex, g_log1, 200 * 257, 10 * 257) >= 1 && count_rumble(g_log_mutex, g_log, 200 * 257, 10 * 257) == 0);
    // Independent adaptive trigger and lightbar.
    {
      std::array<std::uint8_t, 48> output {};
      output[0] = 2;
      output[1] = 0x04;
      output[11] = 0x01;
      output[12] = 77;
      DWORD written = 0;
      WriteFile(pad_for(1).file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      unsigned on0 = 0, on1 = 0;
      {
        std::lock_guard lock(g_log_mutex);
        for (const auto &m : g_log) on0 += m.type == platf::gamepad_feedback_e::set_adaptive_triggers && m.data.adaptive_triggers.right[0] == 77;
      }
      {
        std::lock_guard lock(g_log1_mutex);
        for (const auto &m : g_log1) on1 += m.type == platf::gamepad_feedback_e::set_adaptive_triggers && m.data.adaptive_triggers.right[0] == 77;
      }
      check("adaptive trigger written to pad 1 reaches queue 1 only", on1 >= 1 && on0 == 0);
    }
    // Hold so an external tone can be played to each audio endpoint; print the haptics PCM energy per pad.
    for (int tick = 0; tick != 20; ++tick) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::scoped_lock lock(g_pcm_mutex, g_pcm1_mutex);
      std::printf("MULTI_PCM t=%ds pad0_active_frames=%llu pad1_active_frames=%llu\n", tick + 1, g_pcm.active_frames, g_pcm1.active_frames);
      std::fflush(stdout);
    }
    for (auto &pad : pads) pad.stop.store(true, std::memory_order_release);
    for (auto &pad : pads) CancelIoEx(pad.file, nullptr);
    for (auto &pad : pads) pad.reader.join();
    for (auto &pad : pads) CloseHandle(pad.file);
    std::printf("MULTI pass=%u fail=%u\n", pass, fail);
  }

  /* Touch points, motion, sensor timestamp, battery and headset status as the virtual HID reports them. */
  void
  sensors_probe(platf::ds5::native_client_t &client) {
    auto file = open_virtual_hid();
    if (file == INVALID_HANDLE_VALUE) {
      std::puts("SENSORS FAIL no virtual HID");
      return;
    }
    std::mutex report_mutex;
    std::array<std::uint8_t, 64> last {};
    std::atomic_bool stop { false };
    std::thread reader([&] {
      std::array<std::uint8_t, 64> buffer {};
      while (!stop.load(std::memory_order_acquire)) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
        std::lock_guard lock(report_mutex);
        last = buffer;
      }
    });
    const auto latest = [&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      std::lock_guard lock(report_mutex);
      return last;
    };
    unsigned pass = 0, fail = 0;
    const auto check = [&](const char *name, bool ok, const std::string &detail) {
      std::printf("SENSOR %-40s %s %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
      (ok ? pass : fail) += 1;
    };
    const auto hex = [](const std::array<std::uint8_t, 64> &report, std::size_t from, std::size_t count) {
      std::string text;
      char buffer[4];
      for (std::size_t index = from; index != from + count; ++index) {
        std::snprintf(buffer, sizeof buffer, "%02x", report[index]);
        text += buffer;
      }
      return text;
    };
    using namespace platf;
    // Two fingers: x/y are normalised, the report carries 1920 x 943 (12 bit each).
    client.submit_touch({ { 0, 0 }, LI_TOUCH_EVENT_DOWN, 0, 0.5f, 0.5f, 1.0f });
    client.submit_touch({ { 0, 0 }, LI_TOUCH_EVENT_DOWN, 1, 0.25f, 0.75f, 1.0f });
    auto report = latest();
    // x=960 (0x3c0), y=471 (0x1d7): bytes c0, 73, 1d. x=480 (0x1e0), y=707 (0x2c3): bytes e0, 31, 2c.
    check("touch finger 0 down", hex(report, 33, 4) == "00c0731d", "got " + hex(report, 33, 4) + " want 00c0731d");
    check("touch finger 1 down", hex(report, 37, 4) == "01e0312c", "got " + hex(report, 37, 4) + " want 01e0312c");
    client.submit_touch({ { 0, 0 }, LI_TOUCH_EVENT_UP, 0, 0.5f, 0.5f, 0.0f });
    report = latest();
    check("touch finger 0 up sets the inactive bit", (report[33] & 0x80) != 0, "byte33=" + hex(report, 33, 1));
    // Gyro 100 deg/s on x, accel 1 g on y (9.80665 m/s^2).
    // The default gyro path is a 30 Hz low pass: feed a steady rate until it has settled.
    for (int sample = 0; sample != 120; ++sample) {
      client.submit_motion({ { 0, 0 }, LI_MOTION_TYPE_GYRO, 100.0f, -50.0f, 25.0f });
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    client.submit_motion({ { 0, 0 }, LI_MOTION_TYPE_ACCEL, 0.0f, 9.80665f, 0.0f });
    report = latest();
    const auto word = [&](std::size_t offset) { return static_cast<std::int16_t>(report[offset] | (report[offset + 1] << 8)); };
    check("gyro x 100 deg/s is about 1640 counts", std::abs(word(16) - 1640) < 90, "got " + std::to_string(word(16)));
    check("gyro y -50 deg/s is about -820 counts", std::abs(word(18) + 820) < 60, "got " + std::to_string(word(18)));
    check("gyro z 25 deg/s is about 410 counts", std::abs(word(20) - 410) < 60, "got " + std::to_string(word(20)));
    check("accel y 1 g is 8192 counts", word(24) == 8192, "got " + std::to_string(word(24)));
    const auto timestamp = [&] { return static_cast<std::uint32_t>(report[28] | (report[29] << 8) | (report[30] << 16) | (static_cast<std::uint32_t>(report[31]) << 24)); };
    const auto first_stamp = timestamp();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.submit_motion({ { 0, 0 }, LI_MOTION_TYPE_GYRO, 10.0f, 0.0f, 0.0f });
    report = latest();
    const auto second_stamp = timestamp();
    // 100 ms are 300000 ticks of 1/3 microsecond.
    check("sensor timestamp advances by 1/3 us ticks", second_stamp > first_stamp && (second_stamp - first_stamp) > 250000u && (second_stamp - first_stamp) < 600000u,
      "delta " + std::to_string(second_stamp - first_stamp));
    // Sticks and buttons keep the sensor block (the core zeroes bytes 16 and up on each build).
    gamepad_state_t state {};
    state.buttonFlags = A;
    state.lsX = 32767;
    state.lsY = 32767;  // up
    client.submit_input(0, state);
    report = latest();
    check("stick/button update keeps gyro and finger 1 bytes", word(16) != 0 && report[38] == 0xe0 && (report[33] & 0x80) != 0, "gyro x " + std::to_string(word(16)));
    check("left stick full right and up", report[1] == 255 && report[2] == 0, "x=" + std::to_string(report[1]) + " y=" + std::to_string(report[2]));
    // Battery: percentage 80, charging -> low nibble 8, high nibble 1.
    client.submit_battery({ { 0, 0 }, LI_BATTERY_STATE_CHARGING, 80 });
    report = latest();
    check("battery 80 percent charging", report[53] == 0x18, "byte53=" + hex(report, 53, 1));
    client.submit_battery({ { 0, 0 }, LI_BATTERY_STATE_FULL, 100 });
    report = latest();
    check("battery full", report[53] == 0x2a, "byte53=" + hex(report, 53, 1));
    gamepad_ds5_status_t status {};
    status.id = { 0, 0 };
    status.valid_mask = 0x7;
    status.headset_present = 1;
    status.microphone_present = 1;
    status.microphone_muted = 0;
    client.submit_status(status);
    report = latest();
    check("headset and microphone present", (report[54] & 0x03) == 0x03 && (report[54] & 0x04) == 0, "byte54=" + hex(report, 54, 1));
    status.microphone_muted = 1;
    client.submit_status(status);
    report = latest();
    check("microphone muted bit", (report[54] & 0x04) != 0, "byte54=" + hex(report, 54, 1));
    stop.store(true, std::memory_order_release);
    client.submit_input(0, gamepad_state_t {});
    CancelIoEx(file, nullptr);
    reader.join();
    CloseHandle(file);
    std::printf("SENSORS pass=%u fail=%u\n", pass, fail);
  }

  /* Rumble as it leaves the feedback path: the raw bytes of an output report
   * next to the 16-bit values that make_rumble carries, with whatever tuning
   * profile the process published. */
  void
  rumble_probe() {
    auto file = open_virtual_hid();
    if (file == INVALID_HANDLE_VALUE) {
      std::puts("RUMBLE_PROBE FAIL no virtual HID");
      return;
    }
    for (const auto value : { 16, 64, 128, 200, 255 }) {
      std::array<std::uint8_t, 48> report {};
      report[0] = 2;
      report[1] = 0x01;
      report[3] = static_cast<std::uint8_t>(value);
      report[4] = static_cast<std::uint8_t>(value);
      std::size_t before;
      {
        std::lock_guard lock(g_log_mutex);
        before = g_log.size();
      }
      DWORD written = 0;
      WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::lock_guard lock(g_log_mutex);
      bool seen = false;
      for (auto index = before; index < g_log.size(); ++index) {
        if (g_log[index].type != platf::gamepad_feedback_e::rumble) continue;
        std::printf("RUMBLE_PROBE raw=%d unscaled=%d low=%u high=%u\n", value, value * 257,
          g_log[index].data.rumble.lowfreq, g_log[index].data.rumble.highfreq);
        seen = true;
      }
      if (!seen) std::printf("RUMBLE_PROBE raw=%d no rumble message\n", value);
    }
    CloseHandle(file);
  }

  /* Submit a marked gamepad state at 250 Hz and time its arrival at ReadFile.
   * The marker is the left stick X byte (report byte 1). */
  void
  measure_latency(platf::ds5::native_client_t &client, unsigned duration_ms, unsigned period_us) {
    using clock = std::chrono::steady_clock;
    auto file = open_virtual_hid();
    if (file == INVALID_HANDLE_VALUE) {
      std::puts("LATENCY FAIL no virtual HID");
      return;
    }
    struct read_event {
      clock::time_point at;
      std::uint8_t marker;
    };
    std::vector<read_event> reads;
    reads.reserve(duration_ms / 2 + 1000);
    std::atomic_bool stop { false };
    std::thread reader([&] {
      std::array<std::uint8_t, 64> buffer {};
      while (!stop.load(std::memory_order_acquire)) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
        reads.push_back({ clock::now(), buffer[1] });
      }
    });
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const auto period = std::chrono::microseconds(period_us);
    std::vector<clock::time_point> submitted;
    submitted.reserve(duration_ms / 4 + 100);
    platf::gamepad_state_t state {};
    const auto begin = clock::now();
    auto next = begin;
    for (unsigned count = 0; clock::now() - begin < std::chrono::milliseconds(duration_ms); ++count) {
      next += period;
      while (clock::now() < next) std::this_thread::yield();
      /* Markers 1..255 so a stale 128 (neutral) can never match. */
      const auto marker = static_cast<std::uint8_t>(1u + count % 255u);
      state.lsX = static_cast<std::int16_t>(static_cast<int>(marker) * 257 - 32768);
      submitted.push_back(clock::now());
      client.submit_input(0, state);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    state.lsX = 0;
    client.submit_input(0, state);
    CancelIoEx(file, nullptr);
    reader.join();
    CloseHandle(file);
    /* Latency of submission n: the first read, not before the submission,
     * that shows marker n or any later one. */
    std::vector<double> latencies;
    std::size_t cursor = 0;
    for (std::size_t n = 0; n != submitted.size(); ++n) {
      const auto marker = static_cast<std::uint8_t>(1u + n % 255u);
      while (cursor != reads.size() && reads[cursor].at < submitted[n]) ++cursor;
      for (auto probe = cursor; probe != reads.size() && probe < cursor + 8; ++probe) {
        const auto delta = static_cast<int>(reads[probe].marker) - static_cast<int>(marker);
        if (delta == 0 || (delta > 0 && delta < 64)) {
          latencies.push_back(std::chrono::duration<double, std::milli>(reads[probe].at - submitted[n]).count());
          break;
        }
      }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto pick = [&](double fraction) {
      return latencies.empty() ? -1.0 : latencies[std::min(latencies.size() - 1, static_cast<std::size_t>(fraction * latencies.size()))];
    };
    std::printf("LATENCY period_us=%u submitted=%zu matched=%zu reads=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f\n",
      period_us, submitted.size(), latencies.size(), reads.size(), pick(0.50), pick(0.95), pick(0.99),
      latencies.empty() ? -1.0 : latencies.back());
  }

  /* The attach path starts usbip.exe: found on PATH, or in the machine-wide directory usbip-win2's installer uses
   * (the same two places the host controller probes). */
  bool
  usbip_client_installed() {
    char found[MAX_PATH] {};
    if (SearchPathA(nullptr, "usbip.exe", nullptr, MAX_PATH, found, nullptr) != 0) return true;
    for (const char *variable: { "ProgramW6432", "ProgramFiles" }) {
      const char *program_files = std::getenv(variable);
      if (!program_files || !*program_files) continue;
      std::error_code error;
      if (std::filesystem::is_regular_file(std::filesystem::path(program_files) / "USBip" / "usbip.exe", error)) return true;
    }
    return false;
  }
}  // namespace

int
main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--sweep-on-start") {
    platf::ds5::native_client_t client;
    std::puts("SWEEP_ON_START_DONE");
    return 0;
  }
  if (!usbip_client_installed()) {
    /* ctest reads 77 as "skipped": a host without usbip-win2 cannot attach a virtual DualSense. */
    std::puts("SKIP usbip.exe (usbip-win2) is not installed on this host");
    return 77;
  }
  const auto hold_ms = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 30000u;
  /* ds5_native_usbip_smoke <hold_ms> [latency <duration_ms>] */
  const bool feedback_mode = argc > 2 && std::string(argv[2]) == "feedback";
  const bool buttons_mode = argc > 2 && std::string(argv[2]) == "buttons";
  const bool sensors_mode = argc > 2 && std::string(argv[2]) == "sensors";
  const bool rumble_mode = argc > 2 && std::string(argv[2]) == "rumble";
  /* ALK_DS5_TUNING_FILE: publish a tuning profile written by the settings API,
   * as the web server does when it starts. */
  static std::unique_ptr<ds5::tuning::store_t> tuning_store;
  if (const char *tuning_file = std::getenv("ALK_DS5_TUNING_FILE"); tuning_file && *tuning_file) {
    tuning_store = std::make_unique<ds5::tuning::store_t>(std::filesystem::path(tuning_file));
    std::string load_error;
    const auto loaded = tuning_store->load(&load_error);
    tuning_store->publish();
    std::printf("TUNING_PROFILE file=%s loaded=%d %s\n", tuning_file, loaded ? 1 : 0, load_error.c_str());
  }
  const auto latency_ms = argc > 3 && std::string(argv[2]) == "latency" ? std::strtoul(argv[3], nullptr, 10) : 0ul;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-native-usbip-smoke");
  auto feedback_observer = feedback;
  /* ALK_DS5_SMOKE_QUIET=1 counts PCM frames without printing one line each. */
  const bool quiet_pcm = std::getenv("ALK_DS5_SMOKE_QUIET") != nullptr;
  std::atomic_bool feedback_done { false };
  std::atomic_uint feedback_pcm_frames { 0 };
  std::atomic_uint feedback_adaptive { 0 };
  std::atomic_uint feedback_state { 0 };
  std::thread feedback_reader([&] {
    while (!feedback_done.load(std::memory_order_acquire)) {
      if (auto message = feedback_observer->pop(std::chrono::milliseconds(100))) {
        if (message->type == platf::gamepad_feedback_e::ds5_haptics_pcm) {
          {
            std::lock_guard lock(g_pcm_mutex);
            g_pcm.add(message->data.ds5_haptics.pcm.data(), message->data.ds5_haptics.frame_count);
          }
          feedback_pcm_frames.fetch_add(message->data.ds5_haptics.frame_count, std::memory_order_relaxed);
          if (!quiet_pcm) {
            std::printf("FEEDBACK PCM frames=%u sequence=%u\n",
              static_cast<unsigned>(message->data.ds5_haptics.frame_count),
              static_cast<unsigned>(message->data.ds5_haptics.sequence));
          }
        }
        else if (message->type == platf::gamepad_feedback_e::rumble) {
          std::lock_guard lock(g_log_mutex);
          g_log.push_back(*message);
          std::printf("FEEDBACK RUMBLE low=%u high=%u\n", message->data.rumble.lowfreq, message->data.rumble.highfreq);
        }
        else if (message->type == platf::gamepad_feedback_e::set_rgb_led) {
          std::lock_guard lock(g_log_mutex);
          g_log.push_back(*message);
          std::printf("FEEDBACK RGB %u %u %u\n", message->data.rgb_led.r, message->data.rgb_led.g, message->data.rgb_led.b);
        }
        else if (message->type == platf::gamepad_feedback_e::set_adaptive_triggers) {
          {
            std::lock_guard lock(g_log_mutex);
            g_log.push_back(*message);
          }
          feedback_adaptive.fetch_add(1, std::memory_order_relaxed);
          std::printf("FEEDBACK ADAPTIVE flags=0x%02x left_type=0x%02x right_type=0x%02x\n",
            message->data.adaptive_triggers.event_flags,
            message->data.adaptive_triggers.type_left,
            message->data.adaptive_triggers.type_right);
        }
        else if (message->type == platf::gamepad_feedback_e::ds5_output_extension) {
          std::lock_guard lock(g_log_mutex);
          g_log.push_back(*message);
          std::printf("FEEDBACK EXTENSION valid=0x%02x player=0x%02x\n", message->data.ds5_output_extension.valid_mask,
            message->data.ds5_output_extension.player_led_mask);
        }
        else if (message->type == platf::gamepad_feedback_e::ds5_haptics_state) {
          {
            std::lock_guard lock(g_log_mutex);
            g_log.push_back(*message);
          }
          feedback_state.fetch_add(1, std::memory_order_relaxed);
          std::printf("FEEDBACK STATE bytes=%02x%02x%02x%02x%02x%02x%02x%02x\n",
            message->data.ds5_haptics_state[0], message->data.ds5_haptics_state[1],
            message->data.ds5_haptics_state[2], message->data.ds5_haptics_state[3],
            message->data.ds5_haptics_state[4], message->data.ds5_haptics_state[5],
            message->data.ds5_haptics_state[6], message->data.ds5_haptics_state[7]);
        }
      }
    }
  });
  platf::ds5::native_client_t client;
  if (client.alloc({ 0, 0 }, std::move(feedback), true) != 0) {
    feedback_done.store(true, std::memory_order_release);
    feedback_reader.join();
    std::fprintf(stderr, "FAIL native DS5 allocation or usbip-win2 attach\n");
    return 1;
  }
  const bool multi_mode = argc > 2 && std::string(argv[2]) == "multi";
  /* "pads <n>": n virtual pads attached for the hold time, for enumeration checks only. */
  const unsigned extra_pads = argc > 3 && std::string(argv[2]) == "pads" ? std::strtoul(argv[3], nullptr, 10) : 0u;
  std::vector<platf::feedback_queue_t> extra_queues;
  for (unsigned index = 1; index < extra_pads; ++index) {
    auto queue = mail->queue<platf::gamepad_feedback_msg_t>("ds5-native-usbip-smoke-pad-" + std::to_string(index));
    extra_queues.push_back(queue);
    const auto result = client.alloc({ static_cast<int>(index), static_cast<int>(index) }, std::move(queue), true);
    std::printf("PADS alloc index=%u %s\n", index, result == 0 ? "ok" : "FAIL");
  }
  std::atomic_bool second_done { false };
  std::thread second_reader;
  if (multi_mode) {
    auto second_feedback = mail->queue<platf::gamepad_feedback_msg_t>("ds5-native-usbip-smoke-1");
    auto second_observer = second_feedback;
    if (client.alloc({ 1, 1 }, std::move(second_feedback), true) != 0) {
      std::puts("MULTI FAIL second native DS5 allocation");
    }
    second_reader = std::thread([&, second_observer] {
      while (!second_done.load(std::memory_order_acquire)) {
        if (auto message = second_observer->pop(std::chrono::milliseconds(100))) {
          if (message->type == platf::gamepad_feedback_e::ds5_haptics_pcm) {
            std::lock_guard lock(g_pcm1_mutex);
            g_pcm1.add(message->data.ds5_haptics.pcm.data(), message->data.ds5_haptics.frame_count);
          }
          else {
            std::lock_guard lock(g_log1_mutex);
            g_log1.push_back(*message);
          }
        }
      }
    });
  }
  platf::gamepad_state_t neutral {};
  client.submit_input(0, neutral);
  platf::gamepad_state_t burst {};
  for (int index = 0; index != 8; ++index) {
    burst.buttonFlags = index == 7 ? platf::A : 0;
    client.submit_input(0, burst);
  }
  platf::gamepad_motion_t gyro { { 0, 0 }, LI_MOTION_TYPE_GYRO, 1400.0f, 0.0f, 0.0f };
  client.submit_motion(gyro);
  platf::gamepad_motion_t accel { { 0, 0 }, LI_MOTION_TYPE_ACCEL, 0.0f, 9.80665f, 0.0f };
  client.submit_motion(accel);
  std::printf("READY native DS5 USB/IP lease hold_ms=%lu\n", static_cast<unsigned long>(hold_ms));
  /* usbip-win2 reports attach before UDE publishes all child interfaces. */
  bool hid_found = false;
  for (int attempt = 0; attempt != 20 && !hid_found; ++attempt) {
    hid_found = probe_hid();
    if (!hid_found) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  std::printf("HID_ENUMERATION %s\n", hid_found ? "PASS" : "FAIL");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  probe_audio();
  std::fflush(stdout);
  if (multi_mode) {
    std::this_thread::sleep_for(std::chrono::seconds(8));
    multi_probe(client);
  }
  if (sensors_mode) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sensors_probe(client);
  }
  if (buttons_mode) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    buttons_probe(client);
  }
  if (rumble_mode) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    rumble_probe();
  }
  if (feedback_mode) {
    /* Let an external audio tool start rendering to channels 3 and 4 first. */
    std::this_thread::sleep_for(std::chrono::seconds(8));
    feedback_steps();
  }
  if (latency_ms != 0) measure_latency(client, static_cast<unsigned>(latency_ms), argc > 4 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)) : 4000u);
  /* Host CPU of this process every 5 s: idle versus streaming is read from the
   * phase an external audio tool runs in. */
  {
    const auto hold_begin = std::chrono::steady_clock::now();
    auto last_wall = hold_begin;
    auto last_cpu = process_cpu_ms();
    while (std::chrono::steady_clock::now() - hold_begin < std::chrono::milliseconds(hold_ms)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(std::min<unsigned long>(5000, hold_ms)));
      const auto now = std::chrono::steady_clock::now();
      const auto cpu = process_cpu_ms();
      const auto wall_ms = std::chrono::duration<double, std::milli>(now - last_wall).count();
      std::printf("HOST_CPU t=%.0fs cpu_ms=%.1f wall_ms=%.0f percent_of_one_core=%.2f\n",
        std::chrono::duration<double>(now - hold_begin).count(), cpu - last_cpu, wall_ms,
        100.0 * (cpu - last_cpu) / wall_ms);
      std::fflush(stdout);
      last_wall = now;
      last_cpu = cpu;
    }
  }
  client.free(0);
  if (multi_mode) client.free(1);
  for (unsigned index = 1; index < extra_pads; ++index) client.free(static_cast<int>(index));
  second_done.store(true, std::memory_order_release);
  if (second_reader.joinable()) second_reader.join();
  feedback_done.store(true, std::memory_order_release);
  feedback_reader.join();
  std::printf("FEEDBACK_SUMMARY pcm_frames=%u adaptive=%u state=%u\n",
    feedback_pcm_frames.load(std::memory_order_relaxed),
    feedback_adaptive.load(std::memory_order_relaxed),
    feedback_state.load(std::memory_order_relaxed));
  {
    std::lock_guard lock(g_pcm_mutex);
    const auto seconds = static_cast<double>(g_pcm.active_frames) / 48000.0;
    std::printf("PCM_CHANNELS active_frames=%llu ch3_hz=%.1f ch4_hz=%.1f ch3_rms=%.0f ch4_rms=%.0f\n",
      g_pcm.active_frames, seconds > 0 ? g_pcm.crossings[0] / 2.0 / seconds : 0.0,
      seconds > 0 ? g_pcm.crossings[1] / 2.0 / seconds : 0.0,
      g_pcm.active_frames ? std::sqrt(g_pcm.energy[0] / g_pcm.active_frames) : 0.0,
      g_pcm.active_frames ? std::sqrt(g_pcm.energy[1] / g_pcm.active_frames) : 0.0);
  }
  std::puts("DETACHED native DS5 USB/IP lease");
  return 0;
}

#else
int
main() { return 77; }
#endif
