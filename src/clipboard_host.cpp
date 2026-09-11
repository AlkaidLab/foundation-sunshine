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

#include <nlohmann/json.hpp>
#include <systemd/sd-bus.h>

#include "clipboard_blob_store.h"
#include "clipboard_bridge.h"
#include "clipboard_echo.h"
#include "clipboard_wire.h"
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
    using clipboard_bridge::kInlineThresholdBytes;

    constexpr auto kPollInterval = std::chrono::milliseconds { 1000 };
    constexpr auto kKlipperRetryInterval = std::chrono::milliseconds { 10'000 };
    /// How long the poll thread waits before checking the bus for klipper's
    /// change signal again; a queued client write wakes it immediately.
    constexpr auto kBusWaitInterval = std::chrono::milliseconds { 200 };

    /// Bound on queued client-side writes; the newest clipboard content wins,
    /// so the oldest entry is dropped when a burst overflows.
    constexpr std::size_t kMaxPendingWrites = 8;

    constexpr const char *kKlipperService = "org.kde.klipper";
    constexpr const char *kKlipperPath = "/klipper";
    constexpr const char *kKlipperInterface = "org.kde.klipper.klipper";
    /// klipper's change notification; the 1 s poll stays as a fallback for
    /// builds without it.
    constexpr const char *kKlipperChangeMatch =
      "type='signal',interface='org.kde.klipper.klipper',member='clipboardHistoryUpdated'";

    std::atomic<bool> running { false };
    std::thread poll_thread;
    std::atomic<std::uint32_t> next_token { 1 };

    // What this host last wrote towards klipper, in the agent's 16-entry ring
    // form (a single slot missed the older of two consecutive client copies).
    std::mutex echo_mu;
    clipboard_echo::ring_t echo_ring;

    /// Set by klipper's change signal, cleared when the clipboard is read.
    std::atomic<bool> clipboard_dirty { false };

    int
    on_clipboard_changed(sd_bus_message *, void *, sd_bus_error *) {
      clipboard_dirty.store(true, std::memory_order_release);
      return 1;
    }

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
      return clipboard_wire::encode_text(text);
    }

    /**
     * @brief Record and hand a peer-side text change to the klipper writer.
     */
    void
    apply_inbound_text(const std::string &text) {
      const auto bytes = std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size() };
      {
        std::lock_guard<std::mutex> lk(echo_mu);
        echo_ring.record(clipboard_bridge::kKindText, bytes);
      }

      // Recorded before the write (as before) so the echo of our own change is
      // suppressed even though the poll thread applies it slightly later.
      queue_klipper_write(text);
    }

    /**
     * @brief Resolve a kKindRef frame against the local blob store.
     * @details Only text is applied on this provider (images and file offers
     *          still belong to the GUI agent); the mime decides, exactly as the
     *          agent's inbound handler does.
     */
    void
    on_inbound_ref(const payload_t &bytes) {
      const auto header = clipboard_wire::parse_header(bytes);
      if (!header) {
        return;
      }

      const auto descriptor = clipboard_wire::parse_ref_descriptor(
        std::string_view {
          reinterpret_cast<const char *>(clipboard_wire::payload_of(bytes, *header).data()),
          header->length });
      if (!descriptor) {
        BOOST_LOG(warning) << "Inbound clipboard REF: unusable descriptor"sv;
        return;
      }

      if (!clipboard_wire::is_text_mime(descriptor->mime)) {
        BOOST_LOG(info) << "Inbound clipboard REF: mime '"sv << descriptor->mime
                        << "' is not handled by the host provider; ignoring"sv;
        return;
      }

      const auto blob = clipboard_blob_store::get(descriptor->id);
      if (!blob.found) {
        BOOST_LOG(warning) << "Inbound clipboard REF: blob "sv << descriptor->id
                           << " is missing or expired; ignoring"sv;
        return;
      }

      apply_inbound_text(std::string { blob.bytes.begin(), blob.bytes.end() });
    }

    void
    on_inbound(clipboard_bridge::session_id, const payload_t &bytes) {
      if (!config::input.clipboard_sync) {
        return;
      }

      const auto header = clipboard_wire::parse_header(bytes);
      if (!header) {
        return;
      }

      if (header->kind == clipboard_bridge::kKindRef) {
        on_inbound_ref(bytes);
        return;
      }

      if (header->kind != clipboard_bridge::kKindText) {
        return;
      }

      const auto payload = clipboard_wire::payload_of(bytes, *header);
      apply_inbound_text(std::string { payload.begin(), payload.end() });
    }

    /**
     * @brief Desktop clipboard -> clients poll loop.
     */
    void
    poll_loop() {
      sd_bus *bus = open_bus();
      std::string last_seen;
      bool klipper_available = bus != nullptr;
      bool change_signal_subscribed = false;
      bool had_sessions = false;
      bool logged_unavailable = false;
      auto last_read = std::chrono::steady_clock::now() - kPollInterval;  // read immediately

      while (running.load(std::memory_order_acquire)) {
        if (!klipper_available) {
          bus = open_bus();
          klipper_available = bus != nullptr;
          change_signal_subscribed = false;
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

        // Ask klipper to tell us when the clipboard changes; the periodic read
        // below stays as a fallback for builds without the signal.
        if (!change_signal_subscribed) {
          change_signal_subscribed =
            sd_bus_add_match(bus, nullptr, kKlipperChangeMatch, on_clipboard_changed, nullptr) >= 0;
          if (!change_signal_subscribed) {
            BOOST_LOG(debug) << "Host clipboard: klipper change signal unavailable; polling only"sv;
          }
        }

        // Observe queued bus traffic first, so a change signal that arrived
        // while we waited is acted on now instead of up to a second later.
        int processed = 0;
        while ((processed = sd_bus_process(bus, nullptr)) > 0) {
        }
        if (processed < 0) {
          BOOST_LOG(debug) << "Host clipboard: session bus lost; reconnecting"sv;
          sd_bus_unref(bus);
          bus = nullptr;
          klipper_available = false;
          change_signal_subscribed = false;
          std::this_thread::sleep_for(kKlipperRetryInterval);
          continue;
        }

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

        const auto now = std::chrono::steady_clock::now();
        const bool due = clipboard_dirty.exchange(false, std::memory_order_acq_rel) ||
                         (now - last_read) >= kPollInterval;
        if (!due) {
          // Nothing to do: wait for a queued write or the next bus check.
          std::unique_lock<std::mutex> lk { write_mu };
          write_cv.wait_for(lk, kBusWaitInterval, [&] {
            return !pending_writes.empty() || !running.load(std::memory_order_acquire);
          });
          continue;
        }
        last_read = now;

        std::string content;
        if (!klipper_get(bus, content)) {
          // klipper may have gone away (session teardown); reopen on the next pass.
          sd_bus_unref(bus);
          bus = nullptr;
          klipper_available = false;
          change_signal_subscribed = false;
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
            const auto bytes = std::span<const std::uint8_t> {
              reinterpret_cast<const std::uint8_t *>(content.data()), content.size() };
            is_echo = echo_ring.is_echo(clipboard_bridge::kKindText, bytes);
          }

          if (is_echo) {
            BOOST_LOG(debug) << "Clipboard change matches content written from a client; skipping"sv;
          }
          else if (content.size() > kInlineThresholdBytes) {
            // Too big for one wire frame: store it out of band and post the
            // descriptor, the same route the GUI agent takes. The peer fetches
            // the bytes from this host's blob endpoint.
            auto stored = clipboard_blob_store::put(
              payload_t { content.begin(), content.end() }, clipboard_bridge::kMimeText);
            if (!stored.ok) {
              BOOST_LOG(warning) << "Host clipboard content could not be stored for out-of-band sync ("sv
                                 << content.size() << " bytes): "sv << stored.err;
            }
            else {
              BOOST_LOG(debug) << "Posting host clipboard as an out-of-band blob ("sv << content.size()
                               << " bytes, id "sv << stored.id << ")"sv;
              clipboard_bridge::bridge_t::instance().enqueue_outbound(
                clipboard_bridge::kBroadcast,
                clipboard_wire::encode_ref(stored.id, clipboard_bridge::kMimeText, content.size()));
            }
          }
          else {
            BOOST_LOG(debug) << "Posting host clipboard to clients ("sv << content.size() << " bytes)"sv;
            clipboard_bridge::bridge_t::instance().enqueue_outbound(
              clipboard_bridge::kBroadcast, encode_text_frame(content));
          }
        }

        // Wait briefly: a queued client write wakes this immediately, and the
        // short timeout keeps the bus check (and therefore klipper's change
        // signal) responsive while the 1 s fallback read stays the safety net.
        {
          std::unique_lock<std::mutex> lk { write_mu };
          write_cv.wait_for(lk, kBusWaitInterval, [&] {
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
