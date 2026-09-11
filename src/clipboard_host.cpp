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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <systemd/sd-bus.h>

#include "clipboard_bridge.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/sdbus_session.h"

namespace clipboard_host {
  using namespace std::string_view_literals;
  namespace {
    using payload_t = clipboard_bridge::payload_t;

    // Wire format constants live in the shared bridge header (canonical C++
    // mirror of the GUI agent's clipboard.rs).
    using clipboard_bridge::kEchoTtl;
    using clipboard_bridge::kFrameHeaderBytes;
    using clipboard_bridge::kInlineThresholdBytes;
    using clipboard_bridge::kKindText;
    using clipboard_bridge::kWireVersion;

    constexpr auto kPollInterval = std::chrono::milliseconds { 1000 };
    constexpr auto kKlipperRetryInterval = std::chrono::milliseconds { 10'000 };

    /// Bound on queued client-side writes; the newest clipboard content wins,
    /// so the oldest entry is dropped when a burst overflows.
    constexpr std::size_t kMaxPendingWrites = 8;

    constexpr const char *kKlipperService = "org.kde.klipper";
    constexpr const char *kKlipperPath = "/klipper";
    constexpr const char *kKlipperInterface = "org.kde.klipper.klipper";

    std::atomic<bool> running { false };
    std::thread poll_thread;
    std::atomic<std::uint32_t> next_token { 1 };

    std::mutex echo_mu;
    std::string last_client_written;
    std::chrono::steady_clock::time_point last_client_written_at {};

    // Client-side changes waiting to be written to klipper. The enet control
    // thread only enqueues; the poll thread owns the provider's bus and applies
    // them, so a wedged klipper cannot stall control-packet processing.
    std::mutex write_mu;
    std::condition_variable write_cv;
    std::deque<std::string> pending_writes;

    /**
     * @brief Hand a client-side clipboard change to the poll thread.
     */
    void
    queue_klipper_write(std::string text) {
      {
        std::lock_guard<std::mutex> lk { write_mu };
        if (pending_writes.size() >= kMaxPendingWrites) {
          BOOST_LOG(warning) << "Host clipboard write queue is full; dropping the oldest entry"sv;
          pending_writes.pop_front();
        }
        pending_writes.push_back(std::move(text));
      }
      write_cv.notify_one();
    }

    /**
     * @brief Open a dedicated user-session bus. Each thread that talks to
     *        klipper owns its own connection; sd_bus objects are not
     *        thread-safe.
     */
    sd_bus *
    open_bus() {
      return platf::sdbus::open_user_bus();
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
      // Single-flavor changes carry token 0, matching the GUI agent's wire
      // contract: a non-zero token marks a compound burst that the peer may
      // coalesce, which must not apply to a standalone text copy.
      constexpr std::uint32_t token = 0;
      const auto len = static_cast<std::uint32_t>(text.size());

      payload_t frame;
      frame.reserve(kFrameHeaderBytes + text.size());
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
      if (!config::input.clipboard_sync || bytes.size() < kFrameHeaderBytes || bytes[0] != kWireVersion || bytes[1] != kKindText) {
        return;
      }

      const auto len = static_cast<std::uint32_t>(bytes[6]) |
                       (static_cast<std::uint32_t>(bytes[7]) << 8) |
                       (static_cast<std::uint32_t>(bytes[8]) << 16) |
                       (static_cast<std::uint32_t>(bytes[9]) << 24);
      if (bytes.size() < kFrameHeaderBytes + static_cast<std::size_t>(len)) {
        return;
      }

      const std::string text { bytes.begin() + kFrameHeaderBytes, bytes.begin() + kFrameHeaderBytes + len };

      {
        std::lock_guard<std::mutex> lk(echo_mu);
        last_client_written = text;
        last_client_written_at = std::chrono::steady_clock::now();
      }

      // Recorded before the write (as before) so the echo of our own change is
      // suppressed even though the poll thread applies it slightly later.
      queue_klipper_write(text);
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

        // Apply client-side changes here rather than on the enet control
        // thread, which only enqueues them.
        {
          std::deque<std::string> writes;
          {
            std::lock_guard<std::mutex> lk { write_mu };
            writes.swap(pending_writes);
          }
          for (const auto &text : writes) {
            if (!klipper_set(bus, text)) {
              // The bus may be gone; remaining entries are dropped (the queue
              // is bounded and the content is best-effort anyway).
              break;
            }
            // Do not re-post what we just wrote as a host-side change.
            last_seen = text;
          }
        }

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
          else if (content.size() > kInlineThresholdBytes) {
            BOOST_LOG(debug) << "Host clipboard content too large for inline sync ("sv << content.size()
                             << " bytes); skipping"sv;
          }
          else {
            BOOST_LOG(debug) << "Posting host clipboard to clients ("sv << content.size() << " bytes)"sv;
            clipboard_bridge::bridge_t::instance().enqueue_outbound(
              clipboard_bridge::kBroadcast, encode_text_frame(content));
          }
        }

        // Sleep, but wake immediately when a client-side write arrives so the
        // clipboard hand-off stays responsive.
        {
          std::unique_lock<std::mutex> lk { write_mu };
          write_cv.wait_for(lk, kPollInterval, [&] {
            return !pending_writes.empty() || !running.load(std::memory_order_acquire);
          });
        }
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

    // Wake the poll thread out of its wait instead of letting it time out.
    write_cv.notify_all();

    if (poll_thread.joinable()) {
      poll_thread.join();
    }
    BOOST_LOG(info) << "Host clipboard provider stopped"sv;
  }

}  // namespace clipboard_host
