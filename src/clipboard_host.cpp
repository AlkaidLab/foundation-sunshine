/**
 * @file src/clipboard_host.cpp
 * @brief Linux host-side clipboard provider.
 *
 * In the fork, host clipboard synchronization lives in the Windows GUI agent
 * (src_assets/common/sunshine-control-panel/src-tauri/src/clipboard.rs). On
 * Linux the agent is not part of the runtime, so this module fills that role
 * in-process: the desktop clipboard (KDE klipper over D-Bus) is synced with
 * streaming clients through clipboard_bridge.
 *
 * Wire format is the same frame the GUI agent and clients speak:
 *   [0] wire version (1)
 *   [1] kind (1 = text)
 *   [2:6] token, u32 LE
 *   [6:10] payload length, u32 LE
 *   [10:]  payload (raw UTF-8 for text)
 *
 * Only KIND_TEXT is handled; image / blob-ref frames are left to the GUI
 * agent and WebUI paths.
 */
#include "clipboard_host.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>

#include "clipboard_bridge.h"
#include "src/config.h"
#include "src/logging.h"

namespace clipboard_host {
  using namespace std::string_view_literals;
  namespace {
    using payload_t = clipboard_bridge::payload_t;

    constexpr std::uint8_t kWireVersion = 1;
    constexpr std::uint8_t kKindText = 1;
    // Matches the GUI agent's inline threshold: larger payloads travel via
    // the blob store, which this provider does not implement.
    constexpr std::size_t kMaxInlineTextBytes = 60'000;
    constexpr auto kPollInterval = std::chrono::milliseconds { 1000 };
    constexpr auto kKlipperRetryInterval = std::chrono::milliseconds { 10'000 };
    constexpr auto kEchoTtl = std::chrono::seconds { 5 };

    constexpr const char *kKlipperService = "org.kde.klipper";
    constexpr const char *kKlipperPath = "/klipper";
    constexpr const char *kKlipperInterface = "org.kde.klipper.klipper";

    std::atomic<bool> running { false };
    std::thread poll_thread;
    std::atomic<std::uint32_t> next_token { 1 };

    std::mutex echo_mu;
    std::string last_client_written;
    std::chrono::steady_clock::time_point last_client_written_at {};

    /**
     * @brief Open a dedicated user-session bus. Each thread that talks to
     *        klipper owns its own connection; sd_bus objects are not
     *        thread-safe.
     * @details The address is built explicitly: Sunshine runs with file
     *          capabilities, which sets AT_SECURE and makes sd-bus's
     *          secure_getenv() hide XDG_RUNTIME_DIR from it, so
     *          sd_bus_open_user() would always fail in this process.
     */
    sd_bus *
    open_bus() {
      const char *runtime_dir = ::getenv("XDG_RUNTIME_DIR");
      if (!runtime_dir || !*runtime_dir) {
        return nullptr;
      }

      sd_bus *bus = nullptr;
      if (sd_bus_new(&bus) < 0) {
        return nullptr;
      }

      const std::string address = std::string { "unix:path=" } + runtime_dir + "/bus";
      if (sd_bus_set_address(bus, address.c_str()) < 0 || sd_bus_start(bus) < 0) {
        sd_bus_unref(bus);
        return nullptr;
      }
      return bus;
    }

    bool
    klipper_get(sd_bus *bus, std::string &out) {
      sd_bus_error err = SD_BUS_ERROR_NULL;
      sd_bus_message *reply = nullptr;
      const int rc = sd_bus_call_method(bus, kKlipperService, kKlipperPath, kKlipperInterface,
        "getClipboardContents", &err, &reply, NULL);
      if (rc < 0) {
        BOOST_LOG(debug) << "klipper getClipboardContents failed: "sv << (err.message ? err.message : strerror(-rc));
        sd_bus_error_free(&err);
        return false;
      }

      const char *content = nullptr;
      const int read_rc = sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &content);
      if (read_rc >= 0 && content) {
        out = content;
      }
      sd_bus_message_unref(reply);
      if (read_rc < 0) {
        return false;
      }
      return true;
    }

    bool
    klipper_set(sd_bus *bus, const std::string &text) {
      sd_bus_error err = SD_BUS_ERROR_NULL;
      const int rc = sd_bus_call_method(bus, kKlipperService, kKlipperPath, kKlipperInterface,
        "setClipboardContents", &err, nullptr, "s", text.c_str());
      if (rc < 0) {
        BOOST_LOG(warning) << "klipper setClipboardContents failed: "sv << (err.message ? err.message : strerror(-rc));
        sd_bus_error_free(&err);
        return false;
      }
      return true;
    }

    payload_t
    encode_text_frame(const std::string &text) {
      const auto token = next_token.fetch_add(1, std::memory_order_relaxed);
      const auto len = static_cast<std::uint32_t>(text.size());

      payload_t frame;
      frame.reserve(10 + text.size());
      frame.push_back(kWireVersion);
      frame.push_back(kKindText);
      for (int i = 0; i < 4; ++i) {
        frame.push_back(static_cast<std::uint8_t>((token >> (8 * i)) & 0xFF));
      }
      for (int i = 0; i < 4; ++i) {
        frame.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFF));
      }
      frame.insert(frame.end(), text.begin(), text.end());
      return frame;
    }

    void
    on_inbound(clipboard_bridge::session_id, const payload_t &bytes) {
      if (!config::input.clipboard_sync || bytes.size() < 10 || bytes[0] != kWireVersion || bytes[1] != kKindText) {
        return;
      }

      const auto len = static_cast<std::uint32_t>(bytes[6]) |
                       (static_cast<std::uint32_t>(bytes[7]) << 8) |
                       (static_cast<std::uint32_t>(bytes[8]) << 16) |
                       (static_cast<std::uint32_t>(bytes[9]) << 24);
      if (bytes.size() < 10 + static_cast<std::size_t>(len)) {
        return;
      }

      const std::string text { bytes.begin() + 10, bytes.begin() + 10 + len };

      {
        std::lock_guard<std::mutex> lk(echo_mu);
        last_client_written = text;
        last_client_written_at = std::chrono::steady_clock::now();
      }

      sd_bus *bus = open_bus();
      if (!bus) {
        return;
      }
      klipper_set(bus, text);
      sd_bus_unref(bus);
    }

    /**
     * @brief Desktop clipboard -> clients poll loop.
     */
    void
    poll_loop() {
      sd_bus *bus = open_bus();
      std::string last_seen;
      bool klipper_available = bus != nullptr;
      bool had_sessions = false;
      bool logged_unavailable = false;

      while (running.load(std::memory_order_acquire)) {
        if (!klipper_available) {
          bus = open_bus();
          klipper_available = bus != nullptr;
          if (!klipper_available) {
            if (!logged_unavailable) {
              BOOST_LOG(info) << "Host clipboard sync unavailable (no session bus); will keep retrying"sv;
              logged_unavailable = true;
            }
            std::this_thread::sleep_for(kKlipperRetryInterval);
            continue;
          }
          if (logged_unavailable) {
            BOOST_LOG(info) << "Host clipboard sync provider online"sv;
            logged_unavailable = false;
          }
        }

        clipboard_bridge::bridge_t::instance().notify_gui_alive();

        std::string content;
        if (!klipper_get(bus, content)) {
          // klipper may have gone away (session teardown); reopen on the next pass.
          sd_bus_unref(bus);
          bus = nullptr;
          klipper_available = false;
          std::this_thread::sleep_for(kKlipperRetryInterval);
          continue;
        }

        const auto session_count = clipboard_bridge::bridge_t::instance().session_count();
        if (!had_sessions && session_count > 0) {
          // Baseline the current clipboard when a session starts so the first
          // change on the host (not the state at connect) is what gets posted.
          last_seen = content;
        }
        had_sessions = session_count > 0;

        if (content != last_seen && config::input.clipboard_sync && session_count > 0) {
          last_seen = content;

          bool is_echo = false;
          {
            std::lock_guard<std::mutex> lk(echo_mu);
            const auto age = std::chrono::steady_clock::now() - last_client_written_at;
            is_echo = content == last_client_written && age < kEchoTtl;
          }

          if (is_echo) {
            BOOST_LOG(debug) << "Clipboard change matches content written from a client; skipping"sv;
          }
          else if (content.size() > kMaxInlineTextBytes) {
            BOOST_LOG(debug) << "Host clipboard content too large for inline sync ("sv << content.size()
                             << " bytes); skipping"sv;
          }
          else {
            BOOST_LOG(debug) << "Posting host clipboard to clients ("sv << content.size() << " bytes)"sv;
            clipboard_bridge::bridge_t::instance().enqueue_outbound(
              clipboard_bridge::kBroadcast, encode_text_frame(content));
          }
        }

        std::this_thread::sleep_for(kPollInterval);
      }

      if (bus) {
        sd_bus_unref(bus);
      }
    }

  }  // namespace

  void
  start() {
    if (running.exchange(true)) {
      return;
    }

    clipboard_bridge::bridge_t::instance().add_inbound_listener(&on_inbound);

    poll_thread = std::thread(poll_loop);
    BOOST_LOG(info) << "Host clipboard provider started"sv;
  }

  void
  stop() {
    if (!running.exchange(false)) {
      return;
    }

    if (poll_thread.joinable()) {
      poll_thread.join();
    }
    BOOST_LOG(info) << "Host clipboard provider stopped"sv;
  }

}  // namespace clipboard_host
