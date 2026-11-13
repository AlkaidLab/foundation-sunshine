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
    struct native_impl {
      loopback_usbip_bridge bridge;
      remote_usb::usbip_host_controller host;
      AlkDs5Core *core { nullptr };
      feedback_queue_t feedback;
      std::mutex mutex;
      std::condition_variable condition;
      std::optional<remote_usb::endpoint> live_endpoint;
      usbip_trace trace;
      std::vector<std::uint8_t> report = std::vector<std::uint8_t>(64, 0);
      ::ds5::control_extension::output_t sent_extension {};
      std::uint8_t jack_bits { 0 };
      std::uint8_t battery_status { 0 };
      std::chrono::steady_clock::time_point sensor_epoch { std::chrono::steady_clock::now() };
      AlkUsbIpUacFeatureUnit speaker { sizeof(speaker), ALK_USBIP_CORE_ABI_VERSION, 2, 0, 0, 0, -25600, 0, 256 };
      AlkUsbIpUacFeatureUnit microphone { sizeof(microphone), ALK_USBIP_CORE_ABI_VERSION, 5, 0, 0, 3809, 0, 12288, 122 };
      bool attached { false };
      bool attach_completed { false };
      bool imported { false };
      bool stopping { false };
      bool audio_haptics { false };
      bool pcm_stream_started { false };
      bool speaker_recently_audible { false };
      bool active_counted { false };
      std::uint32_t pcm_sequence { 0 };
      bool haptics_state_initialized { false };
      std::array<std::uint8_t, 3> lightbar_out {};
      std::array<std::uint8_t, 3> game_lightbar {};
      bool have_game_lightbar { false };
      bool lightbar_sent { false };
      ::ds5::rumble::tracker_t rumble_tracker;
      bool first_report_checked { false };
      struct session_stats_t {
        std::map<std::uint32_t, unsigned> combinations;  ///< flag0 | flag1 << 8 | flag2 << 16 -> count
        unsigned reports {};
        unsigned motors_nonzero {};
        std::uint8_t peak_left {};
        std::uint8_t peak_right {};
        std::uint8_t left_mode {};  ///< trigger effect type byte of the last report, left side
        std::uint8_t right_mode {};
        unsigned triggers_forwarded {};
        unsigned lightbar_forwarded {};
        std::array<std::uint8_t, 3> last_rgb {};
        ::ds5::output_fields::snapshot_t last_fields;
        bool have_feedback_fields { false };
        unsigned input_reports {};
        unsigned firmware_refused {};
        unsigned pulses_started {};  ///< rumble starts the game made (zero to nonzero)
        double pulse_shortest_ms {};
        double pulse_longest_ms {};
        bool rumble_on { false };
        std::chrono::steady_clock::time_point rumble_started {};
        std::map<std::uint16_t, std::array<unsigned, 3>> feature;
        std::set<std::uint32_t> feature_logged;
        std::chrono::steady_clock::time_point last_log {};
        std::chrono::steady_clock::time_point last_set_80_log {};
        unsigned set_80_suppressed {};
      } stats;
      void
      note_feature_locked(std::uint8_t report_id, unsigned kind, std::size_t length, const char *outcome,
        std::span<const std::uint8_t> bytes = {}) {
        auto &counts = stats.feature[report_id];
        ++counts[kind];
        if (kind < 2) {
          BOOST_LOG(info) << "Native DS5 feature "sv << (kind == 0 ? "GET"sv : "GET (stalled)"sv) << ": pad "sv << global_index
                          << " id 0x"sv << std::hex << static_cast<unsigned>(report_id) << std::dec << " length "sv << length << " -> "sv << outcome;
          return;
        }
        if (report_id == 0x80u) {
          const auto now = std::chrono::steady_clock::now();
          if (stats.last_set_80_log == std::chrono::steady_clock::time_point {} || now - stats.last_set_80_log >= 100ms) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string encoded;
            encoded.reserve(bytes.size() * 3);
            for (const auto byte : bytes) {
              if (!encoded.empty()) encoded += ' ';
              encoded += digits[byte >> 4];
              encoded += digits[byte & 0x0fu];
            }
            BOOST_LOG(info) << "Native DS5 feature SET 0x80: pad "sv << global_index << " length "sv << length
                            << " bytes ["sv << encoded << "] ignored, nothing forwarded"sv;
            if (stats.set_80_suppressed != 0) BOOST_LOG(info) << "Native DS5 feature SET 0x80: suppressed "sv << stats.set_80_suppressed << " similar request(s)"sv;
            stats.last_set_80_log = now;
            stats.set_80_suppressed = 0;
          }
          else {
            ++stats.set_80_suppressed;
          }
          return;
        }
        const std::uint32_t key = (static_cast<std::uint32_t>(kind) << 8) | report_id;
        if (stats.feature_logged.size() < 64 && stats.feature_logged.insert(key).second) {
          static constexpr const char *names[] = { "GET", "GET (stalled)", "SET" };
          BOOST_LOG(info) << "Native DS5 feature request: pad "sv << global_index << ' ' << names[kind] << " id 0x"sv << std::hex
                          << static_cast<unsigned>(report_id) << std::dec << " length "sv << length << " -> "sv << outcome;
        }
      }
      void
      maybe_log_summary_locked() {
        const auto now = std::chrono::steady_clock::now();
        if (stats.last_log == std::chrono::steady_clock::time_point {}) {
          stats.last_log = now;
          return;
        }
        const auto elapsed = std::chrono::duration<double>(now - stats.last_log).count();
        if (elapsed < 5.0) return;
        std::string combos;
        for (const auto &[key, count] : stats.combinations) {
          char text[48];
          std::snprintf(text, sizeof text, "%s%02x/%02x/%02x x%u", combos.empty() ? "" : ", ", key & 0xffu, (key >> 8) & 0xffu, (key >> 16) & 0xffu, count);
          combos += text;
        }
        std::string features;
        for (const auto &[id, counts] : stats.feature) {
          char text[64];
          std::snprintf(text, sizeof text, "%s0x%02x get %u stall %u set %u", features.empty() ? "" : ", ", id, counts[0], counts[1], counts[2]);
          features += text;
        }
        BOOST_LOG(info) << "Native DS5 session summary: pad "sv << global_index << " window "sv << static_cast<int>(elapsed * 10) / 10.0 << " s, input reports/s "sv
                        << static_cast<int>(stats.input_reports / elapsed) << ", output reports "sv << stats.reports
                        << " flag0/flag1/flag2: ["sv << combos << "], motors nonzero "sv << stats.motors_nonzero << " peak left "sv
                        << static_cast<unsigned>(stats.peak_left) << " right "sv << static_cast<unsigned>(stats.peak_right)
                        << ", trigger mode left 0x"sv << std::hex << static_cast<unsigned>(stats.left_mode) << " right 0x"sv << static_cast<unsigned>(stats.right_mode) << std::dec
                        << " forwarded "sv << stats.triggers_forwarded << ", lightbar forwarded "sv << stats.lightbar_forwarded << " last "sv
                        << static_cast<unsigned>(stats.last_rgb[0]) << ',' << static_cast<unsigned>(stats.last_rgb[1]) << ',' << static_cast<unsigned>(stats.last_rgb[2])
                        << ", feature requests ["sv << features << "], firmware-update requests refused "sv << stats.firmware_refused
                        << ", rumble pulses started "sv << stats.pulses_started << " (rate "sv << static_cast<int>(stats.pulses_started / elapsed * 10) / 10.0
                        << "/s) shortest "sv << static_cast<int>(stats.pulse_shortest_ms * 10) / 10.0 << " ms longest "sv << static_cast<int>(stats.pulse_longest_ms * 10) / 10.0 << " ms";
        stats.combinations.clear();
        stats.reports = stats.motors_nonzero = stats.triggers_forwarded = stats.lightbar_forwarded = stats.input_reports = 0;
        stats.peak_left = stats.peak_right = 0;
        stats.pulses_started = 0;
        stats.pulse_shortest_ms = stats.pulse_longest_ms = 0.0;
        stats.last_log = now;
      }
      std::array<std::uint8_t, 2> held_rumble {};  ///< left, right
      ::ds5::rumble::combination_log_t output_log;
      std::set<std::uint32_t> rejected_reports;
      std::array<std::uint8_t, 8> haptics_state {};
      ::ds5::tuning::runtime::motion_filter_state_t motion_filter;
      ::ds5::sensor_clock_t gyro_clock;
      ::ds5::sensor_clock_t accel_clock;
      std::int64_t last_gyro_stamp_us { 0 };
      std::int64_t last_gyro_arrival_us { 0 };
      ::ds5::tuning::trigger::tracker_t trigger_tracker;
      int global_index { -1 };
      int client_index { 0 };
      std::wstring leased_serial;
      bool mute_button_down { false };
      std::optional<bool> published_mute;
      std::atomic<int> usb_capture_mute { -1 };
      std::atomic_bool mute_watch_stop { false };
      std::thread mute_watch;
      ::ds5::control_extension::output_t game_extension {};
      std::uint64_t session_id { 0 };
      std::uint64_t sequence { 1 };
      paced_reply_queue replies { [this](std::vector<std::uint8_t> wire) { return bridge.send_reply(std::move(wire)); } };
      struct pcm_counters_t {
        std::uint64_t packets {};
        std::uint64_t frames {};
        std::uint64_t haptic_frames_nonzero {};
        std::uint64_t speaker_frames_nonzero {};
        std::uint64_t forwarded_frames {};
        std::uint64_t gated_packets {};
        std::uint64_t last_logged_packets {};
        std::chrono::steady_clock::time_point last_log {};
      } pcm_counters;
      void
      note_pcm(const std::uint8_t *payload, std::size_t length, bool forwarded, std::size_t forwarded_frames) {
        auto &c = pcm_counters;
        ++c.packets;
        if (length >= 8 && (length % 8) == 0) {
          const auto frames = length / 8;
          c.frames += frames;
          for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto *f = payload + frame * 8;
            if ((f[0] | f[1] | f[2] | f[3]) != 0u) ++c.speaker_frames_nonzero;
            if ((f[4] | f[5] | f[6] | f[7]) != 0u) ++c.haptic_frames_nonzero;
          }
        }
        if (forwarded)
          c.forwarded_frames += forwarded_frames;
        else
          ++c.gated_packets;
        const auto now = std::chrono::steady_clock::now();
        if (c.last_log == std::chrono::steady_clock::time_point {}) c.last_log = now;
        if (now - c.last_log >= std::chrono::seconds(5) && c.packets != c.last_logged_packets) {
          c.last_log = now;
          c.last_logged_packets = c.packets;
          BOOST_LOG(info) << "Native DS5 audio endpoint: pad "sv << global_index << " packets="sv << c.packets << " frames="sv << c.frames
                          << " speaker(ch1-2)-nonzero="sv << c.speaker_frames_nonzero << " haptic(ch3-4)-nonzero="sv << c.haptic_frames_nonzero
                          << " forwarded="sv << c.forwarded_frames << " held-back-packets="sv << c.gated_packets;
        }
      }
      /* USB serial leased for this attach. One serial is one Windows container. */
      std::wstring
      pad_serial() const {
        return leased_serial.empty() ? std::wstring(L"02DA55000001") : leased_serial;
      }
      void
      dispatch_audio_pcm(const std::uint8_t *payload, std::size_t length) {
        if (!feedback || !audio_haptics || length < 8 || (length % 8) != 0) {
          note_pcm(payload, length, false, 0);
          return;
        }
        const bool haptics_on = ::ds5::tuning::runtime::allow_authored_pcm() &&
                                ::ds5::tuning::runtime::pcm_source_enabled(audio_haptics);
        const auto total_frames = length / 8;
        bool forwarded_any = false;
        for (std::size_t offset = 0; offset < total_frames;) {
          const auto frames = std::min<std::size_t>(240, total_frames - offset);
          std::vector<std::uint8_t> pcm(frames * 4, 0);
          std::array<std::uint8_t, 240 * 4> speaker {};
          bool speaker_audible = false;
          for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto *source = payload + (offset + frame) * 8;
            std::copy_n(source, 4, speaker.data() + frame * 4);
            speaker_audible = speaker_audible || (source[0] | source[1] | source[2] | source[3]) != 0u;
            if (haptics_on) std::copy_n(source + 4, 4, pcm.data() + frame * 4);
          }
          if (haptics_on) ::ds5::tuning::runtime::apply_pcm_gain(pcm.data(), pcm.size());
          const bool speaker_flow = speaker_audible || speaker_recently_audible;
          speaker_recently_audible = speaker_audible;
          if (!haptics_on && !speaker_flow) {
            offset += frames;
            continue;
          }
          const auto flags = pcm_stream_started ? 0u : 0x01u;
          pcm_stream_started = true;
          const auto timestamp = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
          feedback->raise(gamepad_feedback_msg_t::make_ds5_haptics_pcm(
            static_cast<std::uint16_t>(client_index), static_cast<std::uint8_t>(flags),
            static_cast<std::uint16_t>(frames), ++pcm_sequence, timestamp,
            pcm.data(), pcm.size(), speaker.data(), haptics_on));
          trace.feedback("ds5_haptics_pcm", static_cast<std::uint16_t>(client_index), pcm.size());
          forwarded_any = true;
          offset += frames;
        }
        note_pcm(payload, length, forwarded_any, forwarded_any ? total_frames : 0);
      }
      static std::string
      foreground_process_name() {
        const HWND window = GetForegroundWindow();
        if (window == nullptr) return "none";
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == 0) return "unknown";
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) return "pid " + std::to_string(pid);
        wchar_t path[MAX_PATH] {};
        DWORD size = MAX_PATH;
        std::string result = "pid " + std::to_string(pid);
        if (QueryFullProcessImageNameW(process, 0, path, &size) && size > 0) {
          const std::wstring full(path, size);
          const auto slash = full.find_last_of(L"\\/");
          const auto name = slash == std::wstring::npos ? full : full.substr(slash + 1);
          int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
          std::string utf8(bytes > 0 ? static_cast<std::size_t>(bytes) : 0u, '\0');
          if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), utf8.data(), bytes, nullptr, nullptr);
          result += " " + utf8;
        }
        CloseHandle(process);
        return result;
      }
      void
      raise_trigger_plan(::ds5::tuning::trigger::plan_t trigger_plan) {
        if (!feedback || trigger_plan.flags == 0u) return;
        std::array<std::uint8_t, 10> left_parameters {};
        std::array<std::uint8_t, 10> right_parameters {};
        std::copy_n(trigger_plan.left.data() + 1, 10, left_parameters.begin());
        std::copy_n(trigger_plan.right.data() + 1, 10, right_parameters.begin());
        ::ds5::tuning::runtime::apply_compat_triggers(trigger_plan.left[0], left_parameters.data(), trigger_plan.right[0], right_parameters.data());
        feedback->raise(gamepad_feedback_msg_t::make_adaptive_triggers(static_cast<std::uint16_t>(client_index), trigger_plan.flags, trigger_plan.left[0], trigger_plan.right[0], left_parameters, right_parameters));
        trace.feedback("adaptive_triggers", static_cast<std::uint16_t>(client_index), 22);
        if (trigger_plan.clear) {
          auto clear_left = trigger_plan.left;
          auto clear_right = trigger_plan.right;
          if (trigger_plan.clear_left) clear_left = {};
          if (trigger_plan.clear_right) clear_right = {};
          std::array<std::uint8_t, 10> clear_left_parameters {};
          std::array<std::uint8_t, 10> clear_right_parameters {};
          std::copy_n(clear_left.data() + 1, 10, clear_left_parameters.begin());
          std::copy_n(clear_right.data() + 1, 10, clear_right_parameters.begin());
          feedback->raise(gamepad_feedback_msg_t::make_adaptive_triggers(static_cast<std::uint16_t>(client_index),
            trigger_plan.flags, clear_left[0], clear_right[0], clear_left_parameters, clear_right_parameters));
        }
      }
      void
      dispatch_output_report(const std::uint8_t *payload, std::size_t length) {
        if (!feedback) return;
        AlkDs5Feedback value { sizeof(value), ALK_DS5_CORE_ABI_VERSION };
        const auto parsed = length >= 48 ? alk_ds5_parse_output_report_ex(payload, length, &value) : ALK_DS5_ERROR_MALFORMED_REPORT;
        if (parsed != ALK_DS5_OK) {
          bool first = false;
          {
            std::lock_guard lock(mutex);
            first = rejected_reports.size() < 8 &&
                    rejected_reports.insert(static_cast<std::uint32_t>(length << 8) | (length != 0 ? payload[0] : 0u)).second;
          }
          if (first) {
            BOOST_LOG(info) << "Native DS5 output report rejected: pad "sv << global_index << " length "sv << length
                            << " id 0x"sv << std::hex << (length != 0 ? static_cast<unsigned>(payload[0]) : 0u);
          }
          return;
        }
        /* Ignore the driver's first all-zero default report so it cannot blank client lights. */
        if (!first_report_checked) {
          first_report_checked = true;
          if (length >= 4 && payload[1] == 0xffu && payload[2] == 0xf7u && payload[39] == 0x00u) {
            BOOST_LOG(info) << "Native DS5 output report: pad "sv << global_index
                            << " Windows default report (flag0=0xff flag1=0xf7 flag2=0x00) ignored: not forwarded to the client"sv;
            return;
          }
        }
        std::array<std::uint8_t, 8> state_to_send {};
        bool send_state = false;
        bool fields_changed = false;
        ::ds5::output_fields::snapshot_t changed_fields;
        ::ds5::rumble::decision_t rumble_decision;
        ::ds5::rumble::report_t rumble_report;
        bool log_report = false;
        const auto trigger_config = ::ds5::tuning::trigger::read_config();
        ::ds5::tuning::trigger::plan_t trigger_plan;
        std::uint16_t rumble_left = static_cast<std::uint16_t>(value.motor_left) * 257u;
        std::uint16_t rumble_right = static_cast<std::uint16_t>(value.motor_right) * 257u;
        {
          std::lock_guard lock(mutex);
          if (length >= 48) {
            const auto *effect = payload + 1;  // output report body, 47 bytes
            const auto previous_state = haptics_state;
            haptics_state[0] = static_cast<std::uint8_t>(effect[0] & 3u);
            haptics_state[1] = static_cast<std::uint8_t>(effect[38] & 4u);
            if ((haptics_state[0] & 1u) != 0u || (haptics_state[1] & 4u) != 0u) {
              haptics_state[3] = effect[2];
              haptics_state[4] = effect[3];
            }
            const auto merge = [&](std::uint8_t valid, std::size_t source, std::size_t target, std::uint8_t mask) {
              if ((effect[1] & valid) != 0u) {
                haptics_state[2] = static_cast<std::uint8_t>(haptics_state[2] | valid);
                haptics_state[target] = static_cast<std::uint8_t>(effect[source] & mask);
              }
            };
            merge(0x02u, 9u, 5u, 0xffu);
            merge(0x40u, 36u, 6u, 0xffu);
            merge(0x20u, 39u, 7u, 0x01u);
            if (!haptics_state_initialized || previous_state != haptics_state) {
              haptics_state_initialized = true;
              state_to_send = haptics_state;
              send_state = true;
            }
            trigger_plan = trigger_tracker.plan(payload, value.valid_flag0, trigger_config);
            rumble_report = ::ds5::rumble::from_body(effect);
            rumble_decision = rumble_tracker.decide(rumble_report);
            log_report = output_log.should_log(rumble_report, std::chrono::steady_clock::now());
            ++stats.reports;
            ++stats.combinations[static_cast<std::uint32_t>(rumble_report.flag0) | (static_cast<std::uint32_t>(rumble_report.flag1) << 8) |
                                 (static_cast<std::uint32_t>(rumble_report.flag2) << 16)];
            if ((rumble_report.left | rumble_report.right) != 0u) ++stats.motors_nonzero;
            stats.peak_left = std::max(stats.peak_left, rumble_report.left);
            stats.peak_right = std::max(stats.peak_right, rumble_report.right);
            stats.right_mode = payload[11];
            stats.left_mode = payload[22];
            changed_fields = ::ds5::output_fields::from_feedback(value);
            fields_changed = !stats.have_feedback_fields || stats.last_fields != changed_fields;
            if (fields_changed) {
              stats.have_feedback_fields = true;
              stats.last_fields = changed_fields;
            }
          }
        }
        if (fields_changed) {
          BOOST_LOG(info) << "Native DS5 output fields: pad "sv << global_index << " rgb="sv
                          << static_cast<unsigned>(changed_fields.rgb[0]) << ',' << static_cast<unsigned>(changed_fields.rgb[1]) << ','
                          << static_cast<unsigned>(changed_fields.rgb[2]) << " left-trigger="sv << std::hex
                          << static_cast<unsigned>(changed_fields.left_trigger[0]) << std::dec << '/' << static_cast<unsigned>(changed_fields.left_trigger[1]) << '/'
                          << static_cast<unsigned>(changed_fields.left_trigger[2]) << '/' << static_cast<unsigned>(changed_fields.left_trigger[3]) << '/'
                          << static_cast<unsigned>(changed_fields.left_trigger[4]) << " right-trigger="sv << std::hex
                          << static_cast<unsigned>(changed_fields.right_trigger[0]) << std::dec << '/' << static_cast<unsigned>(changed_fields.right_trigger[1]) << '/'
                          << static_cast<unsigned>(changed_fields.right_trigger[2]) << '/' << static_cast<unsigned>(changed_fields.right_trigger[3]) << '/'
                          << static_cast<unsigned>(changed_fields.right_trigger[4]) << " player=0x"sv << std::hex
                          << static_cast<unsigned>(changed_fields.player_led) << std::dec;
        }
        if (log_report) {
          char line[200];
          std::snprintf(line, sizeof line,
            "Native DS5 output report: pad %d flag0=0x%02x flag1=0x%02x flag2=0x%02x motors=%s right=%u left=%u player=0x%02x -> %s",
            global_index, rumble_report.flag0, rumble_report.flag1, rumble_report.flag2,
            (rumble_report.left | rumble_report.right) != 0u ? "nonzero" : "zero", rumble_report.right, rumble_report.left,
            payload[1 + 43] & 0x1fu, ::ds5::rumble::to_string(rumble_decision.reason));
          BOOST_LOG(info) << line << " (foreground process: "sv << foreground_process_name() << ')';
        }
        if (send_state) {
          feedback->raise(gamepad_feedback_msg_t::make_ds5_haptics_state(static_cast<std::uint16_t>(client_index), state_to_send.data()));
          trace.feedback("ds5_haptics_state", static_cast<std::uint16_t>(client_index), state_to_send.size());
        }
        if (rumble_decision.raise) {
          {
            std::lock_guard lock(mutex);
            held_rumble = { rumble_decision.left, rumble_decision.right };
            const bool nonzero = (rumble_decision.left | rumble_decision.right) != 0u;
            const auto at = std::chrono::steady_clock::now();
            if (nonzero && !stats.rumble_on) {
              stats.rumble_on = true;
              stats.rumble_started = at;
              ++stats.pulses_started;
            }
            else if (!nonzero && stats.rumble_on) {
              stats.rumble_on = false;
              const double ms = std::chrono::duration<double, std::milli>(at - stats.rumble_started).count();
              if (stats.pulse_shortest_ms == 0.0 || ms < stats.pulse_shortest_ms) stats.pulse_shortest_ms = ms;
              if (ms > stats.pulse_longest_ms) stats.pulse_longest_ms = ms;
            }
          }
          rumble_left = static_cast<std::uint16_t>(rumble_decision.left) * 257u;
          rumble_right = static_cast<std::uint16_t>(rumble_decision.right) * 257u;
          if ((rumble_decision.left | rumble_decision.right) != 0u) {
            ::ds5::tuning::runtime::apply_rumble(rumble_left, rumble_right);
          }
          feedback->raise(gamepad_feedback_msg_t::make_rumble(static_cast<std::uint16_t>(client_index), rumble_left, rumble_right));
          trace.feedback("rumble", static_cast<std::uint16_t>(client_index), sizeof(rumble_left) + sizeof(rumble_right));
        }
        raise_trigger_plan(trigger_plan);
        if (trigger_plan.flags != 0u) {
          std::lock_guard lock(mutex);
          ++stats.triggers_forwarded;
        }
        if ((value.valid_flag1 & 4u) != 0u) {
          std::uint8_t red {};
          std::uint8_t green {};
          std::uint8_t blue {};
          ::ds5::tuning::runtime::apply_lightbar(red, green, blue,
            value.lightbar_rgb[0], value.lightbar_rgb[1], value.lightbar_rgb[2]);
          feedback->raise(gamepad_feedback_msg_t::make_rgb_led(static_cast<std::uint16_t>(client_index), red, green, blue));
          {
            std::lock_guard lock(mutex);
            lightbar_out = { red, green, blue };
            ++stats.lightbar_forwarded;
            stats.last_rgb = { red, green, blue };
            game_lightbar = { value.lightbar_rgb[0], value.lightbar_rgb[1], value.lightbar_rgb[2] };
            have_game_lightbar = true;
            lightbar_sent = true;
          }
          trace.feedback("rgb_led", static_cast<std::uint16_t>(client_index), 3);
        }
        dispatch_output_extension(payload + 1);
      }
      void
      dispatch_output_extension(const std::uint8_t *effect) {
        auto current = ::ds5::control_extension::from_output_report(
          effect, static_cast<std::uint16_t>(client_index));
        std::lock_guard lock(mutex);
        ::ds5::control_extension::drop_unset_player_leds(current, sent_extension);
        ::ds5::control_extension::merge_fields(game_extension, current);
        send_extension_changes_locked();
      }
      void
      send_extension_changes_locked() {
        if (!feedback || client_index < 0) return;
        const auto delta = ::ds5::tuning::runtime::plan_extension_update(
          game_extension, sent_extension, static_cast<std::uint32_t>(client_index), (jack_bits & 0x01u) != 0u);
        if (delta.valid_mask == 0u) return;
        feedback->raise(gamepad_feedback_msg_t::make_ds5_output_extension(
          static_cast<std::uint16_t>(client_index), delta.valid_mask, delta.player_led_mask, delta.mute_led,
          delta.speaker_volume, delta.headphone_volume, delta.mic_volume, delta.mic_mute, delta.audio_routing,
          delta.lightbar_brightness));
        trace.feedback("ds5_output_extension", static_cast<std::uint16_t>(client_index), 16);
      }
      /* Tell the client the mute LED that matches the pad capture endpoint. */
      void
      publish_capture_mute_locked(bool muted) {
        published_mute = muted;
        jack_bits = static_cast<std::uint8_t>((jack_bits & ~0x04u) | (muted ? 0x04u : 0u));
        report[54] = jack_bits;
        microphone.mute = muted ? 1u : 0u;
        namespace ext = ::ds5::control_extension;
        game_extension.valid_mask |= ext::out_mic_mute | ext::out_mute_led;
        game_extension.mic_mute = muted ? 1u : 0u;
        game_extension.mute_led = muted ? 1u : 0u;
        send_extension_changes_locked();
      }
      void
      watch_capture_mute() {
        while (!mute_watch_stop.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(200ms);
          if (mute_watch_stop.load(std::memory_order_acquire)) return;
          const int from_usb = usb_capture_mute.exchange(-1, std::memory_order_acq_rel);
          auto muted = pad_audio::capture_muted();
          if (from_usb == 0 || from_usb == 1) {
            const bool wanted = from_usb == 1;
            if (!muted.has_value() || *muted != wanted)
              muted = pad_audio::set_capture_mute(wanted);
            else
              muted = wanted;
          }
          if (!muted.has_value()) continue;
          std::lock_guard lock(mutex);
          if (!stopping && (!published_mute.has_value() || *published_mute != *muted)) publish_capture_mute_locked(*muted);
        }
      }
      std::vector<platf::audio::default_endpoint> original_audio_defaults;
      struct shared_defaults_t {
        std::mutex mutex;
        std::vector<platf::audio::default_endpoint> list;
        std::set<std::wstring> existing;  ///< active endpoints that were there before the first pad
        int users { 0 };
      };
      static shared_defaults_t &
      shared_defaults() {
        static shared_defaults_t value;
        return value;
      }
      static std::set<std::wstring>
      existing_endpoints() {
        std::lock_guard lock(shared_defaults().mutex);
        return shared_defaults().existing;
      }
      static void
      restore_defaults(const std::vector<platf::audio::default_endpoint> &defaults) {
        const auto existing = existing_endpoints();
        (void) platf::audio::restore_default_endpoints(defaults, { L"Wireless Controller", L"VID_054C" }, &existing);
      }
      bool defaults_acquired { false };
      std::atomic_bool defaults_guard_stop { false };
      std::thread defaults_guard;
      std::thread endpoint_setup;
      std::atomic<bool> endpoint_setup_stop { false };
      native_impl() {
        report[0] = 1;
        report[1] = report[2] = report[3] = report[4] = 128;
        report[8] = 8;
        sweep_stale_ports();
      }
      void
      sweep_stale_ports() {
        const auto result = host.sweep_stale_ds5_ports();
        if (!result.detached_ports.empty()) {
          BOOST_LOG(info) << "Native DS5 released stale USB/IP hub port(s)";
          for (const auto port : result.detached_ports) {
            BOOST_LOG(info) << "  detached port " << port;
          }
        }
        if (!result.ok() && result.status != remote_usb::usbip_host_status::unsupported) {
          BOOST_LOG(debug) << "Native DS5 stale USB/IP sweep: " << result.detail;
        }
      }

      AlkDs5Error
      drain_core_events_locked() {
        if (core == nullptr) return ALK_DS5_ERROR_SESSION;
        for (;;) {
          AlkDs5Event event { sizeof(event), ALK_DS5_CORE_ABI_VERSION };
          const auto result = alk_ds5_core_poll_event(core, &event);
          if (result == ALK_DS5_ERROR_NOT_FOUND) return ALK_DS5_OK;
          if (result != ALK_DS5_OK) return result;
        }
      }

      ~native_impl() { stop(); }

      void
      push_tuning_state() {
        std::lock_guard lock(mutex);
        if (!feedback || global_index < 0 || stopping || core == nullptr) return;
        const auto extension_before = sent_extension;
        send_extension_changes_locked();
        const bool extension_sent = !(extension_before == sent_extension);
        const auto trigger_plan = trigger_tracker.plan(nullptr, 0u, ::ds5::tuning::trigger::read_config());
        raise_trigger_plan(trigger_plan);
        bool rumble_sent = false;
        if (rumble_tracker.running() && (held_rumble[0] | held_rumble[1]) != 0u) {
          auto left = static_cast<std::uint16_t>(held_rumble[0] * 257u);
          auto right = static_cast<std::uint16_t>(held_rumble[1] * 257u);
          ::ds5::tuning::runtime::apply_rumble(left, right);
          feedback->raise(gamepad_feedback_msg_t::make_rumble(static_cast<std::uint16_t>(client_index), left, right));
          trace.feedback("rumble", static_cast<std::uint16_t>(client_index), sizeof(left) + sizeof(right));
          rumble_sent = true;
        }
        const auto push = ::ds5::tuning::runtime::plan_lightbar_push(have_game_lightbar, game_lightbar, lightbar_out, lightbar_sent);
        if (push.emit) {
          lightbar_out = { push.red, push.green, push.blue };
          lightbar_sent = true;
          feedback->raise(gamepad_feedback_msg_t::make_rgb_led(static_cast<std::uint16_t>(client_index), push.red, push.green, push.blue));
          trace.feedback("rgb_led", static_cast<std::uint16_t>(client_index), 3);
        }
        BOOST_LOG(info) << "Native DS5 live tuning push: pad "sv << global_index << " client number "sv << client_index
                        << " extension="sv << (extension_sent ? "sent"sv : "unchanged"sv)
                        << " triggers=0x"sv << std::hex << static_cast<unsigned>(trigger_plan.flags) << std::dec
                        << " rumble="sv << (rumble_sent ? "re-shaped"sv : "none"sv)
                        << " lightbar="sv << (push.emit ? "sent"sv : "unchanged"sv);
      }

      bool
      test_rumble(std::uint8_t left_in, std::uint8_t right_in, bool restore) {
        std::lock_guard lock(mutex);
        if (!feedback || global_index < 0 || stopping || core == nullptr) return false;
        std::array<std::uint8_t, 2> values { left_in, right_in };
        if (restore) values = ::ds5::rumble_test::restore_values(rumble_tracker.running(), held_rumble);
        auto left = static_cast<std::uint16_t>(values[0] * 257u);
        auto right = static_cast<std::uint16_t>(values[1] * 257u);
        if ((values[0] | values[1]) != 0u) ::ds5::tuning::runtime::apply_rumble(left, right);
        feedback->raise(gamepad_feedback_msg_t::make_rumble(static_cast<std::uint16_t>(client_index), left, right));
        trace.feedback("rumble", static_cast<std::uint16_t>(client_index), sizeof(left) + sizeof(right));
        BOOST_LOG(info) << "Native DS5 rumble test: pad "sv << global_index << " client number "sv << client_index
                        << (restore ? " end"sv : " start"sv) << " left "sv << left << " right "sv << right << " (16-bit, after the saved shaping)"sv;
        return true;
      }

      void
      run_endpoint_setup() {
        static std::atomic<bool> pruned { false };
        if (!pruned.exchange(true)) {
          const auto removed = platf::audio::prune_phantom_pad_devices(L"VID_054C&PID_0CE6", L"02DA55");
          for (const auto &id : removed) {
            BOOST_LOG(info) << "Native DS5 audio setup: removed a leftover virtual pad device from an earlier attach: "sv << narrow_log_text(id);
          }
          for (const auto &name : platf::audio::list_stale_pad_endpoints(L"Wireless Controller")) {
            BOOST_LOG(info) << "Native DS5 audio setup: endpoint of an earlier attach is not active: "sv << narrow_log_text(name);
          }
        }
        const char *program_data = std::getenv("ALK_DS5_STATE_DIR");
        if (program_data == nullptr) program_data = std::getenv("ProgramData");
        const auto known_file = std::filesystem::path(program_data != nullptr ? program_data : "C:\\ProgramData") / "Sunshine" / "ds5_audio_known.txt";
        std::set<std::wstring> logged;
        int settled = 0;
        for (int step = 0; step < 60 && !endpoint_setup_stop.load(std::memory_order_acquire); ++step) {
          std::this_thread::sleep_for(500ms);
          const auto states = platf::audio::configure_pad_endpoints(L"Wireless Controller", known_file);
          bool render = false, capture = false;
          for (const auto &state : states) {
            (state.flow == eRender ? render : capture) = state.verified || (render && state.flow == eRender);
            const auto key = state.id + (state.configured_now ? L"!" : L"") + (state.warning.empty() ? L"" : L"?");
            if (!logged.insert(key).second) continue;
            BOOST_LOG(info) << "Native DS5 audio setup: "sv << (state.flow == eRender ? "render"sv : "capture"sv) << " endpoint "sv
                            << narrow_log_text(state.name) << " ("sv << narrow_log_text(state.id) << ") "sv << (state.first_time ? "first time"sv : "known"sv)
                            << ": speakers "sv << std::hex << state.speakers_before << "->"sv << state.speakers_after << " full-range "sv
                            << state.full_range_before << "->"sv << state.full_range_after << std::dec << " format-ok "sv << state.format_before_ok << "->"sv
                            << state.format_after_ok << ", volume "sv << static_cast<int>(state.volume_before * 100) << "%->"sv << static_cast<int>(state.volume_after * 100)
                            << "% mute "sv << state.mute_before << "->"sv << state.mute_after << ", read back "sv << (state.verified ? "ok"sv : "NOT ok"sv)
                            << (state.warning.empty() ? ""s : " WARNING: "s + state.warning);
          }
          if (render && capture) {
            if (++settled >= 3) break;
          }
          else {
            settled = 0;
          }
        }
      }

      static std::string
      narrow_log_text(const std::wstring &text) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(bytes > 0 ? static_cast<std::size_t>(bytes) : 0u, '\0');
        if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
        return out;
      }

      bool
      start(const gamepad_id_t &id, feedback_queue_t queue, bool use_audio) {
        std::unique_lock lock(mutex);
        if (attached || core != nullptr) return false;
        sweep_stale_ports();
        feedback = std::move(queue);
        global_index = id.globalIndex;
        client_index = id.clientRelativeIndex;
        leased_serial = pad_audio::lease_serial(global_index);
        audio_haptics = use_audio;
        {
          auto &shared = shared_defaults();
          std::lock_guard shared_lock(shared.mutex);
          if (shared.users++ == 0) {
            shared.existing = platf::audio::capture_active_endpoint_ids();
            if (auto defaults = platf::audio::capture_default_endpoints()) {
              shared.list = std::move(*defaults);
            }
            else {
              shared.list.clear();
            }
          }
          original_audio_defaults = shared.list;
          defaults_acquired = true;
        }
        const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
        session_id = (pid << 32) | static_cast<std::uint32_t>(id.globalIndex + 1);
        AlkDs5BackendEvidence evidence { sizeof(evidence), ALK_DS5_CORE_ABI_VERSION };
        evidence.proven_capabilities = 0xffffffffu;
        evidence.transport_mask = 1u;
        evidence.audio.struct_size = sizeof(evidence.audio);
        evidence.audio.abi = ALK_DS5_CORE_ABI_VERSION;
        evidence.audio.sample_rate_hz = 48000;
        evidence.audio.bits_per_sample = 16;
        evidence.audio.channel_count = 4;
        evidence.audio.supports_render = use_audio;
        evidence.audio.supports_capture = use_audio;
        AlkDs5Profile profile { sizeof(profile), ALK_DS5_CORE_ABI_VERSION, kVendor, kProduct,
          ALK_DS5_TRANSPORT_USB, 1, kProductStringNarrow,
          "Sony Interactive Entertainment", nullptr, &evidence };
        AlkDs5Event arrival { sizeof(arrival), ALK_DS5_CORE_ABI_VERSION };
        if (alk_ds5_core_create(&profile, &core) != ALK_DS5_OK ||
            alk_ds5_core_begin_session(core, session_id) != ALK_DS5_OK ||
            alk_ds5_core_arrive(core, 0, &arrival) != ALK_DS5_OK) {
          lock.unlock();
          stop();
          return false;
        }
        if (drain_core_events_locked() != ALK_DS5_OK) {
          lock.unlock();
          stop();
          return false;
        }
        auto open_bridge = [this]() -> std::optional<remote_usb::endpoint> {
          remote_usb::device_info device;
          device.busid = "1-1";
          device.path = "/sys/devices/platform/ds5spike/usb1/1-1";
          device.busnum = 1;
          device.devnum = 1;
          device.speed = 3;
          device.vendor_id = kVendor;
          device.product_id = kProduct;
          device.device_bcd = 0x0100;
          device.configuration_value = 1;
          device.interfaces = { { 1, 1, 0 }, { 1, 2, 0 }, { 1, 2, 0 }, { 3, 0, 0 } };
          remote_usb::callbacks callbacks;
          callbacks.on_request = [this](std::vector<std::uint8_t> wire) { return request(std::move(wire)); };
          callbacks.on_imported = [this] {
            std::lock_guard lock(mutex);
            imported = true;
            condition.notify_all();
          };
          callbacks.on_closed = [this](remote_usb::close_reason) {
            /* Never call the queue with the mutex held: its worker takes the
             * mutex while building an interrupt reply. */
            replies.clear();
            std::lock_guard lock(mutex);
            imported = false;
            if (core) alk_ds5_core_clear(core);
            condition.notify_all();
          };
          boost::system::error_code error;
          const auto opened = bridge.start(std::move(device), std::move(callbacks), error);
          if (!opened) return std::nullopt;
          live_endpoint = *opened;
          remote_usb::remember_protected_endpoint(*opened);
          if (trace.enabled()) {
            remote_usb::device_info imported_device;
            imported_device.busid = "1-1";
            imported_device.path = "/sys/devices/platform/ds5spike/usb1/1-1";
            imported_device.speed = 3;
            imported_device.device_bcd = 0x0100;
            imported_device.configuration_value = 1;
            imported_device.interfaces = { { 1, 1, 0 }, { 1, 2, 0 }, { 1, 2, 0 }, { 3, 0, 0 } };
            trace.record_import(imported_device, *opened);
          }
          return opened;
        };
        auto endpoint = open_bridge();
        if (!endpoint) {
          lock.unlock();
          stop();
          return false;
        }
        for (int attempt = 0; attempt != 4 && !attached && !stopping; ++attempt) {
          if (attempt != 0) {
            lock.unlock();
            bridge.stop();
            if (live_endpoint) {
              remote_usb::forget_protected_endpoint(*live_endpoint);
              live_endpoint.reset();
            }
            sweep_stale_ports();  // the stalled import may still hold a hub port
            std::this_thread::sleep_for(1500ms * attempt);
            lock.lock();
            if (stopping) break;
            endpoint = open_bridge();
            if (!endpoint) break;
          }
          attach_completed = false;
          remote_usb::usbip_host_request attempt_request { *endpoint };
          host.attach(std::move(attempt_request), [this](remote_usb::usbip_host_result result) {
            std::lock_guard attach_lock(mutex);
            attached = result.ok();
            attach_completed = true;
            if (!result.ok()) trace.attach_failure(result.detail);
            condition.notify_all();
          });
          condition.wait_for(lock, 15s, [this] { return attach_completed || stopping; });
        }
        if (!attached) {
          lock.unlock();
          stop();
          return false;
        }
        lock.unlock();
        std::vector<platf::audio::default_endpoint> defaults;
        {
          std::lock_guard guard(mutex);
          defaults = original_audio_defaults;
        }
        for (const auto delay : { 0ms, 250ms, 1000ms }) {
          std::this_thread::sleep_for(delay);
          restore_defaults(defaults);
        }
        defaults_guard_stop.store(false, std::memory_order_release);
        defaults_guard = std::thread([this] {
          for (int step = 0; step != 20 && !defaults_guard_stop.load(std::memory_order_acquire); ++step) {
            std::this_thread::sleep_for(500ms);
            std::vector<platf::audio::default_endpoint> defaults;
            {
              std::lock_guard guard(mutex);
              defaults = original_audio_defaults;
            }
            restore_defaults(defaults);
          }
        });
        endpoint_setup_stop.store(false, std::memory_order_release);
        endpoint_setup = std::thread([this] { run_endpoint_setup(); });
        mute_watch_stop.store(false, std::memory_order_release);
        mute_watch = std::thread([this] { watch_capture_mute(); });
        if (!active_counted) {
          active_counted = true;
          ::ds5::tuning::runtime::state().active_controllers.fetch_add(1, std::memory_order_acq_rel);
        }
        return true;
      }

      void
      stop() {
        std::unique_lock lock(mutex);
        if (stopping) return;
        stopping = true;
        lock.unlock();
        defaults_guard_stop.store(true, std::memory_order_release);
        endpoint_setup_stop.store(true, std::memory_order_release);
        mute_watch_stop.store(true, std::memory_order_release);
        if (defaults_guard.joinable()) defaults_guard.join();
        if (endpoint_setup.joinable()) endpoint_setup.join();
        if (mute_watch.joinable()) mute_watch.join();
        bridge.stop();
        host.stop();
        replies.clear();
        lock.lock();
        if (feedback && global_index >= 0 && rumble_tracker.take_stop()) {
          feedback->raise(gamepad_feedback_msg_t::make_rumble(static_cast<std::uint16_t>(client_index), 0, 0));
        }
        if (feedback && global_index >= 0) {
          while (::ds5::tuning::runtime::state().release_on_end.load(std::memory_order_acquire) &&
                 (lightbar_out[0] | lightbar_out[1] | lightbar_out[2]) != 0) {
            for (auto &channel : lightbar_out) channel = ::ds5::tuning::runtime::fade_channel(channel);
            feedback->raise(gamepad_feedback_msg_t::make_rgb_led(static_cast<std::uint16_t>(client_index),
              lightbar_out[0], lightbar_out[1], lightbar_out[2]));
          }
        }
        if (live_endpoint) {
          remote_usb::forget_protected_endpoint(*live_endpoint);
          live_endpoint.reset();
        }
        attached = false;
        imported = false;
        if (core) {
          alk_ds5_core_end_session(core, session_id);
          alk_ds5_core_destroy(core);
          core = nullptr;
        }
        original_audio_defaults.clear();
        if (defaults_acquired) {
          defaults_acquired = false;
          auto &shared = shared_defaults();
          std::lock_guard shared_lock(shared.mutex);
          if (--shared.users == 0) {
            shared.list.clear();
            shared.existing.clear();
          }
        }
        if (global_index >= 0) pad_audio::release_serial(global_index);
        leased_serial.clear();
        global_index = -1;
        if (active_counted) {
          active_counted = false;
          ::ds5::tuning::runtime::state().active_controllers.fetch_sub(1, std::memory_order_acq_rel);
        }
        stopping = false;
      }

      bool
      owns(int index) const noexcept { return global_index == index && core != nullptr; }

      bool
      request(std::vector<std::uint8_t> wire) {
        if (wire.size() < kUsbipHeader) return false;
        const auto command = get_be(wire.data());
        if (command != 1u && command != 2u) return false;
        const auto sequence_number = get_be(wire.data() + 4);
        const auto direction = get_be(wire.data() + 12);
        const auto endpoint = get_be(wire.data() + 16);
        const auto length = get_be(wire.data() + 24);
        const auto packet_count = get_be(wire.data() + 32);
        if (command == 2) {
          const auto cancelled = replies.cancel(get_be(wire.data() + 20));
          return bridge.send_reply(ret_unlink(sequence_number, cancelled ? -104 : 0));
        }
        if (length > 1024u * 1024u || wire.size() < kUsbipHeader) return false;
        if (endpoint == 0) return control(sequence_number, direction, wire.data() + 40,
          wire.data() + kUsbipHeader, length);
        const auto payload_size = direction == 0 ? length : 0u;
        if (wire.size() < kUsbipHeader + payload_size) return false;
        const auto payload = wire.data() + kUsbipHeader;
        if (endpoint == 4 && direction == 1 && packet_count == 0xffffffffu) {
          std::array<std::uint8_t, 8> setup {};
          std::copy_n(wire.data() + 40, setup.size(), setup.begin());
          replies.schedule_interrupt(paced_reply_queue::lane::interrupt_in, sequence_number,
            [this, sequence_number, endpoint, direction, length, setup] {
              std::vector<std::uint8_t> current_report;
              {
                std::lock_guard lock(mutex);
                current_report = report;
                ++stats.input_reports;
                maybe_log_summary_locked();
              }
              trace.urb(sequence_number, endpoint, direction, length, 0, current_report.size(),
                setup, current_report);
              return ret_submit(sequence_number, 0, current_report);
            });
          return true;
        }
        if (endpoint == 3 && direction == 0 && packet_count == 0xffffffffu) {
          AlkDs5Feedback feedback_value { sizeof(feedback_value), ALK_DS5_CORE_ABI_VERSION };
          dispatch_output_report(payload, length);
          trace.urb(sequence_number, endpoint, direction, length, 0, 0,
            std::span<const std::uint8_t>(wire.data() + 40, 8),
            std::span<const std::uint8_t>(payload, length));
          return bridge.send_reply(ret_submit(sequence_number, 0, {}, length));
        }
        if (packet_count > 0 && packet_count <= ALK_USBIP_MAX_ISO_PACKETS) {
          const auto descriptor_offset = kUsbipHeader + payload_size;
          const auto descriptor_bytes = static_cast<std::size_t>(packet_count) * 16u;
          if (wire.size() < descriptor_offset + descriptor_bytes) return false;
          if (endpoint == 1 && direction == 0) {
            if (core && audio_haptics && length != 0) {
              AlkDs5AudioFrame frame { sizeof(frame), ALK_DS5_CORE_ABI_VERSION, 0, 48000, 16, 4, 0,
                length / 8u, 0, reinterpret_cast<const std::int16_t *>(payload), nullptr };
              std::lock_guard lock(mutex);
              (void) alk_ds5_core_audio_write(core, &frame);
            }
            dispatch_audio_pcm(payload, length);
            return iso_reply(sequence_number, wire, false);
          }
          if (endpoint == 2 && direction == 1) return iso_reply(sequence_number, wire, true);
        }
        trace.urb(sequence_number, endpoint, direction, length, kPipeError, 0,
          std::span<const std::uint8_t>(wire.data() + 40, 8), {});
        return bridge.send_reply(ret_submit(sequence_number, kPipeError, {}));
      }

      bool
      iso_reply(std::uint32_t sequence_number, const std::vector<std::uint8_t> &wire, bool capture) {
        const auto direction = get_be(wire.data() + 12);
        const auto length = get_be(wire.data() + 24);
        const auto count = get_be(wire.data() + 32);
        const auto payload_offset = kUsbipHeader + (direction == 0 ? length : 0u);
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> descriptors(static_cast<std::size_t>(count) * 16u, 0);
        std::uint32_t actual = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
          const auto *descriptor = wire.data() + payload_offset + index * 16u;
          const auto offset = get_be(descriptor);
          const auto packet_length = get_be(descriptor + 4);
          const auto available = std::min(packet_length, 192u);
          put_be(descriptors.data() + index * 16u, offset);
          put_be(descriptors.data() + index * 16u + 4, packet_length);
          put_be(descriptors.data() + index * 16u + 8, capture ? available : packet_length);
          if (capture) payload.insert(payload.end(), available, 0);
          actual += capture ? available : packet_length;
        }
        trace.urb(sequence_number, get_be(wire.data() + 16), direction, length, 0, actual,
          std::span<const std::uint8_t>(wire.data() + 40, 8), payload);
        auto reply = ret_submit(sequence_number, 0, payload, actual, count, descriptors);
        std::copy_n(wire.data() + 28, 4, reply.data() + 28);  // start_frame echoes the request
        replies.schedule_iso(capture ? paced_reply_queue::lane::iso_in : paced_reply_queue::lane::iso_out,
          sequence_number, count, std::move(reply));
        return true;
      }

      bool
      control(std::uint32_t sequence_number, std::uint32_t direction, const std::uint8_t *setup,
        const std::uint8_t *data, std::size_t data_length) {
        const auto request = setup[1];
        const auto value = get_le16(setup + 2);
        const auto index = get_le16(setup + 4);
        const auto requested = get_le16(setup + 6);
        const bool in = (setup[0] & 0x80u) != 0;
        std::vector<std::uint8_t> reply;
        if (request == 6 && in) {
          const auto type = static_cast<std::uint8_t>(value >> 8);
          const auto number = static_cast<std::uint8_t>(value);
          if (type == 1)
            reply = device_descriptor();
          else if (type == 2)
            reply = configuration_descriptor();
          else if (type == 3)
            reply = string_descriptor(number, pad_serial());
          else if (type == 0x21 && (index & 0xffu) == 3)
            reply = hid_descriptor();
          else if (type == 0x22 && (index & 0xffu) == 3)
            reply = hid_report_descriptor();
          else
            return stall(sequence_number);
        }
        else if ((setup[0] & 0x60u) == 0x20u && (index & 0xffu) == 3 && request == 9 && !in) {
          const auto set_type = static_cast<std::uint8_t>(value >> 8);
          if (set_type == 2 && data_length >= 48) {
            dispatch_output_report(data, data_length);
          }
          else if (set_type == 3) {
            const auto set_id = static_cast<std::uint8_t>(value);
            if (::ds5::feature::feature_set_action_for(set_id) == ::ds5::feature::feature_set_action::refuse) {
              /* A firmware update or manufacturing command: the virtual pad has no firmware to change and nothing is
               * ever forwarded to a real pad. The request fails, and the attempt is logged. */
              {
                std::lock_guard lock(mutex);
                note_feature_locked(set_id, 2, data_length, "REFUSED (firmware update class), nothing forwarded");
                ++stats.firmware_refused;
              }
              BOOST_LOG(info) << "Native DS5 WARNING: refused a firmware-update class feature report 0x"sv << std::hex
                              << static_cast<unsigned>(set_id) << std::dec << " ("sv << data_length << " bytes) for pad "sv << global_index;
              return stall(sequence_number);
            }
            std::lock_guard lock(mutex);
            note_feature_locked(set_id, 2, data_length,
              ::ds5::feature::is_public_get_only_feature_report(set_id) ? "ignored (public GET-only report)" : "ignored (not on the public read-only list)",
              std::span<const std::uint8_t>(data, data_length));
          }
          reply = {};
        }
        else if ((setup[0] & 0x60u) == 0x20u && (index & 0xffu) == 3 && request == 1 && in) {
          const auto report_type = static_cast<std::uint8_t>(value >> 8);
          const auto report_id = static_cast<std::uint8_t>(value);
          if (report_type == 3) {
            std::size_t feature_length = 0;
            bool known = alk_ds5_feature_report_length(report_id, &feature_length) == ALK_DS5_OK;
            if (!known) {
              feature_length = declared_feature_length(report_id);
              known = feature_length != 0;
            }
            if (!known) {
              std::lock_guard lock(mutex);
              note_feature_locked(report_id, 1, requested, "stall (not declared)");
              return stall(sequence_number);
            }
            reply.assign(feature_length, 0);
            reply[0] = report_id;
            const char *outcome = "zeros of the declared size";
            if (report_id == ALK_DS5_FEATURE_CALIBRATION_ID && feature_length == ::ds5::feature::calibration_bytes) {
              /* A game divides by these ranges: zeros turn every motion value into NaN or infinity. */
              const auto calibration = ::ds5::feature::calibration_report();
              std::copy(calibration.begin(), calibration.end(), reply.begin());
              outcome = "calibration";
            }
            if (report_id == ALK_DS5_FEATURE_PAIRING_ID) {
              const auto mac = pad_audio::mac_from_serial(pad_serial());
              std::copy(mac.begin(), mac.end(), reply.begin() + 1);
              reply[7] = 0x08;
              reply[8] = 0x25;
              {
                const auto crc = ::ds5::feature::feature_crc(reply.data(), reply.size() - 4);
                for (std::size_t i = 0; i < 4; ++i) reply[reply.size() - 4 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
              }
              outcome = "pairing info with the pad's own address";
            }
            if (report_id == ALK_DS5_FEATURE_FIRMWARE_ID && feature_length == ::ds5::feature::firmware_bytes) {
              const auto firmware = ::ds5::feature::firmware_report();
              std::copy(firmware.begin(), firmware.end(), reply.begin());
              outcome = "firmware info";
            }
            std::lock_guard lock(mutex);
            note_feature_locked(report_id, 0, requested, outcome);
          }
          else if (report_type == 1) {
            std::lock_guard lock(mutex);
            if (report_id != 0u && report_id != report[0]) return stall(sequence_number);
            reply = report;
          }
          else
            return stall(sequence_number);
        }
        else if ((setup[0] & 0x60u) == 0x20u && (index & 0xffu) == 0 && (value >> 8) != 0 &&
                 (request == 1 || request == 9 || (request >= 0x81 && request <= 0x84))) {
          auto &unit = (index >> 8) == 5 ? microphone : speaker;
          const auto control = static_cast<std::uint8_t>(value >> 8);
          int32_t feature_value = 0;
          if (in) {
            if (alk_usbip_uac_feature_get(&unit, control, request, &feature_value) != ALK_USBIP_OK) {
              return stall(sequence_number);
            }
            if (control == 1)
              reply = { static_cast<std::uint8_t>(feature_value) };
            else {
              reply.resize(2);
              reply[0] = static_cast<std::uint8_t>(feature_value);
              reply[1] = static_cast<std::uint8_t>(feature_value >> 8);
            }
          }
          else {
            const auto input_value = data_length >= 2 ? static_cast<int32_t>(static_cast<int16_t>(get_le16(data))) :
                                                        (data_length == 1 ? data[0] : 0);
            if (alk_usbip_uac_feature_set(&unit, control, input_value) != ALK_USBIP_OK) return stall(sequence_number);
            if (&unit == &microphone && control == 1) usb_capture_mute.store(input_value != 0 ? 1 : 0, std::memory_order_release);
            reply = {};
          }
        }
        else if (request == 9 && !in) {
          reply = {};
        }
        else if (request == 8 && in) {
          reply = { 1 };
        }
        else if (request == 11 && !in) {
          reply = {};
        }
        else if (request == 10 && in) {
          reply = { 0 };
        }
        else {
          return stall(sequence_number);
        }
        if (reply.size() > requested) reply.resize(requested);
        trace.urb(sequence_number, 0, direction, data_length, 0, reply.size(),
          std::span<const std::uint8_t>(setup, 8), reply);
        return bridge.send_reply(ret_submit(sequence_number, 0, reply, static_cast<std::uint32_t>(reply.size())));
      }

      bool
      stall(std::uint32_t sequence_number) {
        trace.urb(sequence_number, 0, 0, 0, kPipeError, 0, {}, {});
        return bridge.send_reply(ret_submit(sequence_number, kPipeError, {}));
      }
    };
  }  // namespace

  struct native_client_t::impl_t {
    std::shared_mutex registry;
    std::vector<std::unique_ptr<native_impl>> controllers;

    native_impl *
    find(int index) const {
      for (const auto &controller : controllers) {
        if (controller->owns(index)) return controller.get();
      }
      return nullptr;
    }
  };

  namespace {
    std::atomic<std::int64_t (*)()> motion_clock { nullptr };

    std::mutex live_clients_mutex;
    std::vector<native_client_t *> live_clients;

    unsigned
    test_rumble_all(std::uint8_t left, std::uint8_t right, bool restore) {
      std::lock_guard lock(live_clients_mutex);
      unsigned pads = 0;
      for (auto *client : live_clients) pads += client->test_rumble(left, right, restore);
      return pads;
    }

    void
    push_saved_lightbar() {
      std::lock_guard lock(live_clients_mutex);
      BOOST_LOG(info) << "DualSense tuning saved: pushing to "sv << live_clients.size() << " native client(s), "sv
                      << ::ds5::tuning::runtime::state().active_controllers.load(std::memory_order_acquire) << " active pad(s)"sv;
      for (auto *client : live_clients) client->push_tuning_state();
    }
  }  // namespace

  native_client_t::native_client_t(): _impl(std::make_unique<impl_t>()) {
    {
      std::lock_guard lock(live_clients_mutex);
      live_clients.push_back(this);
    }
    ::ds5::tuning::runtime::published_handler().store(&push_saved_lightbar, std::memory_order_release);
    ::ds5::rumble_test::handler().store(&test_rumble_all, std::memory_order_release);
  }
  native_client_t::~native_client_t() {
    std::lock_guard lock(live_clients_mutex);
    live_clients.erase(std::remove(live_clients.begin(), live_clients.end(), this), live_clients.end());
  }
  void
  native_client_t::push_tuning_state() {
    std::shared_lock registry(_impl->registry);
    for (const auto &controller : _impl->controllers) controller->push_tuning_state();
  }
  unsigned
  native_client_t::test_rumble(std::uint8_t left, std::uint8_t right, bool restore) {
    std::shared_lock registry(_impl->registry);
    unsigned pads = 0;
    for (const auto &controller : _impl->controllers) pads += controller->test_rumble(left, right, restore) ? 1u : 0u;
    return pads;
  }
  bool
  native_client_t::owns(int index) const {
    std::shared_lock registry(_impl->registry);
    return _impl->find(index) != nullptr;
  }
  bool
  native_client_t::audio_haptics_active() const noexcept { return published_audio_haptics.load(boost::memory_order_relaxed); }

  int
  native_client_t::alloc(const gamepad_id_t &id, feedback_queue_t queue, bool audio_haptics) {
    {
      std::shared_lock registry(_impl->registry);
      if (_impl->find(id.globalIndex) != nullptr || _impl->controllers.size() >= kMaxControllers) return -1;
    }
    auto controller = std::make_unique<native_impl>();
    if (!controller->start(id, std::move(queue), audio_haptics)) return -1;
    std::unique_lock registry(_impl->registry);
    _impl->controllers.push_back(std::move(controller));
    published_audio_haptics.store(audio_haptics || published_audio_haptics.load(boost::memory_order_relaxed),
      boost::memory_order_release);
    return 0;
  }

  void
  native_client_t::free(int index) {
    std::unique_ptr<native_impl> released;
    {
      std::unique_lock registry(_impl->registry);
      auto &list = _impl->controllers;
      const auto found = std::find_if(list.begin(), list.end(), [index](const auto &c) { return c->owns(index); });
      if (found == list.end()) return;
      released = std::move(*found);
      list.erase(found);
      bool any_audio = false;
      for (const auto &c : list) any_audio = any_audio || c->audio_haptics;
      published_audio_haptics.store(any_audio, boost::memory_order_release);
    }
    released->stop();
  }

  void
  native_client_t::submit_input(int index, const gamepad_state_t &state) {
    std::shared_lock registry(_impl->registry);
    auto *const pad = _impl->find(index);
    if (pad == nullptr) return;
    std::unique_lock lock(pad->mutex);
    auto input_state = state;
    const auto triggers = ::ds5::tuning::runtime::shape_triggers(input_state.lt, input_state.rt);
    const bool l2_pressed = triggers.l2_pressed;
    const bool r2_pressed = triggers.r2_pressed;
    input_state.lt = triggers.left;
    input_state.rt = triggers.right;
    ::ds5::tuning::runtime::apply_stick(input_state.lsX, input_state.lsY, true);
    ::ds5::tuning::runtime::apply_stick(input_state.rsX, input_state.rsY, false);
    AlkDs5State value { sizeof(value), ALK_DS5_CORE_ABI_VERSION, 0, ++pad->sequence, 0, 8,
      input_state.lt, input_state.rt, 0,
      ::ds5::button_map::stick_x(input_state.lsX), ::ds5::button_map::stick_y(input_state.lsY),
      ::ds5::button_map::stick_x(input_state.rsX), ::ds5::button_map::stick_y(input_state.rsY), 0 };
    value.hat = ::ds5::button_map::hat(input_state.buttonFlags);
    value.buttons = ::ds5::button_map::buttons(input_state.buttonFlags, 0, 0) |
                    (l2_pressed ? ::ds5::button_map::bit::l2 : 0u) | (r2_pressed ? ::ds5::button_map::bit::r2 : 0u);
    AlkDs5Event event { sizeof(event), ALK_DS5_CORE_ABI_VERSION };
    auto result = alk_ds5_core_submit_state(pad->core, &value, &event);
    if (result == ALK_DS5_ERROR_QUEUE_FULL) {
      (void) pad->drain_core_events_locked();
      result = alk_ds5_core_submit_state(pad->core, &value, &event);
    }
    (void) result;
    (void) pad->drain_core_events_locked();
    std::size_t length = 0;
    std::array<std::uint8_t, 48> sensors_and_status {};
    std::copy_n(pad->report.begin() + 16, sensors_and_status.size(), sensors_and_status.begin());
    if (alk_ds5_build_input_report(&value, pad->report.data(), pad->report.size(), &length) == ALK_DS5_OK) {
      std::copy(sensors_and_status.begin(), sensors_and_status.end(), pad->report.begin() + 16);
      pad->report[7] = static_cast<std::uint8_t>(value.sequence);
    }
    const bool mute_down = (input_state.buttonFlags & MISC_BUTTON) != 0;
    const bool mute_edge = mute_down && !pad->mute_button_down;
    pad->mute_button_down = mute_down;
    if (mute_edge) {
      lock.unlock();
      const auto muted = pad_audio::toggle_capture_mute();
      lock.lock();
      if (muted.has_value()) {
        pad->publish_capture_mute_locked(*muted);
        BOOST_LOG(info) << "Native DS5 capture mute: pad "sv << pad->global_index << (*muted ? " muted"sv : " open"sv) << " on its own endpoint"sv;
      }
      else {
        BOOST_LOG(info) << "Native DS5 capture mute: pad "sv << pad->global_index
                        << " button pressed, but no active Wireless Controller capture endpoint"sv;
      }
    }
  }

  void
  native_client_t::submit_touch(const gamepad_touch_t &touch) {
    std::shared_lock registry(_impl->registry);
    auto *const pad = _impl->find(touch.id.globalIndex);
    if (pad == nullptr) return;
    std::lock_guard lock(pad->mutex);
    const auto offset = touch.pointerId & 1u ? 37u : 33u;
    const auto x = static_cast<std::uint16_t>(touch.x * 1920.f);
    const auto y = static_cast<std::uint16_t>(touch.y * 943.f);
    pad->report[offset] = static_cast<std::uint8_t>((touch.eventType == LI_TOUCH_EVENT_UP ? 0x80u : 0) | (touch.pointerId & 0x7fu));
    pad->report[offset + 1] = static_cast<std::uint8_t>(x);
    pad->report[offset + 2] = static_cast<std::uint8_t>((x >> 8) | (y << 4));
    pad->report[offset + 3] = static_cast<std::uint8_t>(y >> 4);
  }

  void
  native_client_t::set_motion_clock(std::int64_t (*clock)()) {
    motion_clock.store(clock, std::memory_order_release);
  }

  void
  native_client_t::submit_motion(const gamepad_motion_t &motion) {
    std::shared_lock registry(_impl->registry);
    auto *const pad = _impl->find(motion.id.globalIndex);
    if (pad == nullptr) return;
    std::lock_guard lock(pad->mutex);
    float counts_per_unit = 0.0f;
    std::size_t offset = 0;
    if (motion.motionType == LI_MOTION_TYPE_GYRO) {
      counts_per_unit = kGyroCountsPerDegreePerSecond;
      offset = 16u;
    }
    else if (motion.motionType == LI_MOTION_TYPE_ACCEL) {
      counts_per_unit = kAccelCountsPerG / kStandardGravity;
      offset = 22u;
    }
    else {
      return;
    }
    auto x = motion.x;
    auto y = motion.y;
    auto z = motion.z;
    const auto clock = motion_clock.load(std::memory_order_acquire);
    const auto arrival_us = clock != nullptr ? clock() : static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pad->sensor_epoch).count());
    float dt_seconds = 1.0f / 250.0f;
    std::int64_t stamp_us = arrival_us;
    if (motion.motionType == LI_MOTION_TYPE_GYRO) {
      stamp_us = pad->gyro_clock.stamp(arrival_us);
      if (pad->last_gyro_stamp_us != 0) dt_seconds = static_cast<float>(stamp_us - pad->last_gyro_stamp_us) / 1e6f;
      pad->last_gyro_stamp_us = stamp_us;
      pad->last_gyro_arrival_us = arrival_us;
    }
    else {
      if (arrival_us - pad->last_gyro_arrival_us < 100000)
        stamp_us = 0;
      else
        stamp_us = pad->accel_clock.stamp(arrival_us);
    }
    if (motion.motionType == LI_MOTION_TYPE_GYRO &&
        !::ds5::tuning::runtime::apply_gyro(x, y, z, pad->motion_filter, dt_seconds)) {
      put_le16(pad->report.data() + offset, 0);
      put_le16(pad->report.data() + offset + 2, 0);
      put_le16(pad->report.data() + offset + 4, 0);
      return;
    }
    const auto to_counts = [counts_per_unit](float value) {
      const auto rounded = std::lround(value * counts_per_unit);
      return static_cast<std::int16_t>(std::clamp(rounded, -32768L, 32767L));
    };
    put_le16(pad->report.data() + offset, to_counts(x));
    put_le16(pad->report.data() + offset + 2, to_counts(y));
    put_le16(pad->report.data() + offset + 4, to_counts(z));
    if (stamp_us != 0) {
      const auto ticks = static_cast<std::uint32_t>(static_cast<std::uint64_t>(stamp_us) * 3u);
      for (std::size_t byte = 0; byte < 4; ++byte) {
        pad->report[28 + byte] = static_cast<std::uint8_t>(ticks >> (8 * byte));
      }
    }
  }

  void
  native_client_t::submit_battery(const gamepad_battery_t &battery) {
    std::shared_lock registry(_impl->registry);
    auto *const pad = _impl->find(battery.id.globalIndex);
    if (pad == nullptr) return;
    std::lock_guard lock(pad->mutex);
    const auto percentage = battery.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN ? 50u : std::min<unsigned>(battery.percentage, 100u);
    std::uint8_t level = static_cast<std::uint8_t>((percentage + 5u) / 10u);
    std::uint8_t charging = 0;
    if (battery.state == LI_BATTERY_STATE_CHARGING) {
      charging = 1;
    }
    else if (battery.state == LI_BATTERY_STATE_FULL) {
      charging = 2;
      level = 10;
    }
    pad->battery_status = static_cast<std::uint8_t>((charging << 4) | (level & 0x0f));
    pad->report[53] = pad->battery_status;
  }

  void
  native_client_t::submit_status(const gamepad_ds5_status_t &status) {
    std::shared_lock registry(_impl->registry);
    auto *const pad = _impl->find(status.id.globalIndex);
    if (pad == nullptr) return;
    std::lock_guard lock(pad->mutex);
    namespace ext = ::ds5::control_extension;
    if (status.valid_mask & ext::in_headset) {
      pad->jack_bits = static_cast<std::uint8_t>((pad->jack_bits & ~0x01u) | (status.headset_present ? 0x01u : 0u));
    }
    if (status.valid_mask & ext::in_microphone) {
      pad->jack_bits = static_cast<std::uint8_t>((pad->jack_bits & ~0x02u) | (status.microphone_present ? 0x02u : 0u));
    }
    if (status.valid_mask & ext::in_mic_muted) {
      pad->jack_bits = static_cast<std::uint8_t>((pad->jack_bits & ~0x04u) | (status.microphone_muted ? 0x04u : 0u));
    }
    pad->report[54] = pad->jack_bits;
  }
}  // namespace platf::ds5
