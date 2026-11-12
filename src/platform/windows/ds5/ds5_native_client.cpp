/**
 * @file src/platform/windows/ds5/ds5_native_client.cpp
 * @brief In-process DualSense USB/IP device backed by the standalone cores.
 *
 * The Windows path uses the current usbip-win2 attach/detach contract.  The
 * device server stays in this process, so a session lease is released by the
 * bridge close callback and by normal object destruction.  The virtual audio
 * interface is never selected as a Windows default endpoint; endpoint policy
 * therefore remains owned by the user and is restored by construction.
 * The HID report path is the USB/IP bridge itself; ds5-ms-hid-api-20261107,
 * ds5-linux-hid-playstation-20261107, ds5-sdl-hidapi-ps5-20261107 and
 * ds5-dualsensectl-main-20261107 supply the public report definitions below.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <audioclient.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/log/trivial.hpp>

#include "alkaidlab/ds5_core/ds5_core.h"
#include "alkaidlab/usbip_core/usbip_core.h"
#include "src/ds5/button_map.h"
#include "src/ds5/control_extension.h"
#include "src/ds5/feature_reports.h"
#include "src/ds5/output_fields.h"
#include "src/ds5/rumble_policy.h"
#include "src/ds5/rumble_test.h"
#include "src/ds5/sensor_clock.h"
#include "src/ds5/tuning_runtime.h"
#include "src/ds5/tuning_trigger.h"
#include "src/logging_severity.h"
#include "src/platform/windows/audio_default_guard.h"
#include "src/platform/windows/ds5/ds5_native_client.h"
#include "src/platform/windows/ds5/ds5_paced_reply.h"
#include "src/platform/windows/ds5/ds5_pad_audio.h"
#include "src/remote_usb/loopback_usbip_bridge.h"
#include "src/remote_usb/remote_usb_host_controller.h"

namespace platf::ds5 {
  static_assert(::ds5::button_map::flag::dpad_up == DPAD_UP && ::ds5::button_map::flag::dpad_down == DPAD_DOWN &&
                ::ds5::button_map::flag::dpad_left == DPAD_LEFT && ::ds5::button_map::flag::dpad_right == DPAD_RIGHT &&
                ::ds5::button_map::flag::start == START && ::ds5::button_map::flag::back == BACK &&
                ::ds5::button_map::flag::left_stick == LEFT_STICK && ::ds5::button_map::flag::right_stick == RIGHT_STICK &&
                ::ds5::button_map::flag::left_shoulder == LEFT_BUTTON && ::ds5::button_map::flag::right_shoulder == RIGHT_BUTTON &&
                ::ds5::button_map::flag::home == HOME && ::ds5::button_map::flag::a == A && ::ds5::button_map::flag::b == B &&
                ::ds5::button_map::flag::x == X && ::ds5::button_map::flag::y == Y &&
                ::ds5::button_map::flag::touchpad == TOUCHPAD_BUTTON && ::ds5::button_map::flag::misc == MISC_BUTTON);

  namespace {
    using namespace std::chrono_literals;
    using remote_usb::loopback_usbip_bridge;

    constexpr std::uint32_t kVendor = 0x054c;
    constexpr std::uint32_t kProduct = 0x0ce6;
    constexpr std::size_t kUsbipHeader = 48;
    constexpr const wchar_t *kProductString = L"Wireless Controller";
    constexpr const char *kProductStringNarrow = "Wireless Controller";
    constexpr std::size_t kMaxControllers = 8;
    constexpr std::int32_t kPipeError = -32;
    constexpr float kGyroCountsPerDegreePerSecond = 16.4f;
    constexpr float kAccelCountsPerG = 8192.0f;
    constexpr float kStandardGravity = 9.80665f;

    class usbip_trace final {
    public:
      usbip_trace() {
        if (const auto *path = std::getenv("ALK_DS5_USBIP_TRACE"); path && *path) {
          stream_.open(path, std::ios::out | std::ios::app);
        }
      }
      bool
      enabled() const noexcept { return stream_.is_open(); }
      void
      record_import(const remote_usb::device_info &device, const remote_usb::endpoint &endpoint) {
        std::lock_guard lock(mutex_);
        if (!enabled()) return;
        stream_ << "OP_REP_IMPORT path=" << device.path << " busid=" << device.busid
                << " speed=" << device.speed << " bcdDevice=0x" << std::hex << device.device_bcd
                << std::dec << " class=" << static_cast<unsigned>(device.device_class) << ":"
                << static_cast<unsigned>(device.device_subclass) << ":"
                << static_cast<unsigned>(device.device_protocol)
                << " configuration=" << static_cast<unsigned>(device.configuration_value)
                << " interfaces=" << device.interfaces.size() << " endpoint=" << endpoint.address
                << ":" << endpoint.port << '\n';
        stream_.flush();
      }
      void
      urb(std::uint32_t sequence, std::uint32_t endpoint, std::uint32_t direction,
        std::size_t length, std::int32_t status, std::size_t returned,
        std::span<const std::uint8_t> setup, std::span<const std::uint8_t> bytes) {
        std::lock_guard lock(mutex_);
        if (!enabled()) return;
        stream_ << "URB seq=" << sequence << " endpoint=" << endpoint << " direction=" << direction
                << " length=" << length << " status=" << status << " returned_bytes=" << returned
                << " setup=";
        write_hex(setup);
        stream_ << " returned=";
        write_hex(bytes);
        stream_ << '\n';
        stream_.flush();
      }
      void
      attach_failure(std::string_view detail) {
        std::lock_guard lock(mutex_);
        if (!enabled()) return;
        stream_ << "ATTACH_FAILURE detail=" << detail << '\n';
        stream_.flush();
      }
      void
      feedback(std::string_view type, std::uint16_t id, std::size_t bytes = 0) {
        std::lock_guard lock(mutex_);
        if (!enabled()) return;
        stream_ << "FEEDBACK type=" << type << " id=" << id << " bytes=" << bytes << '\n';
        stream_.flush();
      }

    private:
      void
      write_hex(std::span<const std::uint8_t> bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        for (const auto byte : bytes) {
          stream_ << digits[byte >> 4] << digits[byte & 0x0f];
        }
      }
      std::mutex mutex_;
      std::ofstream stream_;
    };
    std::vector<std::uint8_t>
    hex(std::string_view text) {
      std::vector<std::uint8_t> result;
      result.reserve(text.size() / 2);
      auto digit = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        return static_cast<std::uint8_t>(value - 'A' + 10);
      };
      for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        result.push_back(static_cast<std::uint8_t>((digit(text[index]) << 4) | digit(text[index + 1])));
      }
      return result;
    }
    void
    put_be(std::uint8_t *out, std::uint32_t value) {
      out[0] = static_cast<std::uint8_t>(value >> 24);
      out[1] = static_cast<std::uint8_t>(value >> 16);
      out[2] = static_cast<std::uint8_t>(value >> 8);
      out[3] = static_cast<std::uint8_t>(value);
    }
    std::uint32_t
    get_be(const std::uint8_t *in) {
      return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
             (static_cast<std::uint32_t>(in[2]) << 8) | in[3];
    }
    std::uint16_t
    get_le16(const std::uint8_t *in) {
      return static_cast<std::uint16_t>(in[0] | (static_cast<std::uint16_t>(in[1]) << 8));
    }
    void
    put_le16(std::uint8_t *out, std::int16_t value) {
      const auto raw = static_cast<std::uint16_t>(value);
      out[0] = static_cast<std::uint8_t>(raw);
      out[1] = static_cast<std::uint8_t>(raw >> 8);
    }
    const std::vector<std::uint8_t> &
    device_descriptor() {
      static const auto value = hex("12010002000000404c05e60c000101020301");
      return value;
    }
    const std::vector<std::uint8_t> &
    configuration_descriptor() {
      static const auto value = hex(
        "0902e300040100c0fa0904000000010100000a2401000149000201020c24020101010604330000000c24060201010300000000000924030301030402000c2402040204030203000000092406050401030000092403060101010500"
        "090401000001020000090401010101020000072401010101000b2402010402100180bb0009050109880104000007250100000000090402000001020000090402010101020000072401060101000b2402010202100180bb0009058205c400040000072501000000000904030002030000000921110100012211010705840340000607050303400006");
      return value;
    }
    const std::vector<std::uint8_t> &
    hid_descriptor() {
      static const auto value = hex("092111010001221101");
      return value;
    }
    const std::vector<std::uint8_t> &
    hid_report_descriptor() {
      static const auto value = hex(
        "05010905a1018501093009310932093509330934150026ff007508950681020600ff09209501810205010939150025073500463b016514750495018142650005091901290f150025017501950f81020600ff0921950d81020600ff0922150026ff0075089534810285020923952f9102850509339528b10285080934952fb102850909249513b102850a0925951ab10285200926953fb102852109279504b10285220940953fb10285800928953fb10285810929953fb1028582092a9509b1028583092b953fb1028584092c953fb1028585092d9502b10285a0092e9501b10285e0092f953fb10285f00930953fb10285f10931953fb10285f20932950fb10285f40935953fb10285f509369503b102c0");
      return value;
    }
    std::vector<std::uint8_t>
    ret_submit(std::uint32_t sequence, std::int32_t status,
      std::span<const std::uint8_t> payload,
      std::uint32_t actual_length = 0,
      std::uint32_t packet_count = 0,
      std::span<const std::uint8_t> descriptors = {}) {
      const auto actual = actual_length == 0 ? static_cast<std::uint32_t>(payload.size()) : actual_length;
      std::vector<std::uint8_t> wire(kUsbipHeader + payload.size() + descriptors.size(), 0);
      put_be(wire.data(), 3);
      put_be(wire.data() + 4, sequence);
      put_be(wire.data() + 20, static_cast<std::uint32_t>(status));
      put_be(wire.data() + 24, actual);
      put_be(wire.data() + 32, packet_count);
      put_be(wire.data() + 36, 0);
      std::copy(payload.begin(), payload.end(), wire.begin() + kUsbipHeader);
      std::copy(descriptors.begin(), descriptors.end(), wire.begin() + kUsbipHeader + payload.size());
      return wire;
    }
    std::vector<std::uint8_t>
    ret_unlink(std::uint32_t sequence, std::int32_t status) {
      std::vector<std::uint8_t> wire(kUsbipHeader, 0);
      put_be(wire.data(), 4);
      put_be(wire.data() + 4, sequence);
      put_be(wire.data() + 20, static_cast<std::uint32_t>(status));
      return wire;
    }
    std::size_t
    declared_feature_length(std::uint8_t id) {
      switch (id) {
        case 0x05:
          return 41;
        case 0x08:
          return 48;
        case 0x09:
          return 20;
        case 0x0a:
          return 27;
        case 0x20:
          return 64;
        case 0x21:
          return 5;
        case 0x22:
          return 64;
        case 0x80:
        case 0x81:
        case 0x83:
        case 0x84:
          return 64;
        case 0x82:
          return 10;
        case 0x85:
          return 3;
        case 0xa0:
          return 2;
        case 0xe0:
        case 0xf0:
        case 0xf1:
        case 0xf4:
          return 64;
        case 0xf2:
          return 16;
        case 0xf5:
          return 4;
        default:
          return 0;
      }
    }
    std::vector<std::uint8_t>
    string_descriptor(std::uint8_t index, const std::wstring &serial = L"") {
      if (index == 0) return { 4, 3, 9, 4 };
      const std::wstring value = index == 1 ? L"Sony Interactive Entertainment" : index == 3 ? serial :
                                                                                               kProductString;
      std::vector<std::uint8_t> result(2 + value.size() * 2);
      result[0] = static_cast<std::uint8_t>(result.size());
      result[1] = 3;
      for (std::size_t i = 0; i < value.size(); ++i) {
        result[2 + i * 2] = static_cast<std::uint8_t>(value[i]);
        result[3 + i * 2] = static_cast<std::uint8_t>(value[i] >> 8);
      }
      return result;
    }
  }  // namespace
}  // namespace platf::ds5
