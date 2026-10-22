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
 * Text travels through klipper (strings only). Images cannot: they need the
 * compositor's own clipboard, so the image half lives in
 * platform/linux/clipboard_image.cpp and speaks ext_data_control_v1. Both
 * halves publish and apply the same clipboard_wire frames, so a client cannot
 * tell which half moved the data. KIND_FILE_OFFER frames remain the GUI agent's.
 */
#include "clipboard_host.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
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
#ifdef SUNSHINE_BUILD_WAYLAND
  #include "src/platform/linux/clipboard_data.h"
#endif
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

#ifdef SUNSHINE_BUILD_WAYLAND
    /// Compositor half (desktop <-> clients): always images, and text too when
    /// klipper is absent. Null when the session has no data-control protocol.
    /// Guarded by watcher_mu: the enet control thread, the RTSP thread and the
    /// shutdown path all reach it.
    std::shared_ptr<platf::clipboard_data::watcher_t> data_watcher;
    std::mutex watcher_mu;
    /// True when this desktop has no klipper, so text must go through the
    /// compositor as well (niri, sway, Hyprland, plain weston).
    std::atomic<bool> ext_text { false };
    std::mutex text_mu;
    /// Text we last saw or published, so an unchanged selection is not re-posted.
    std::optional<std::string> last_text;
    std::mutex image_mu;
    /// Fingerprint of the picture we last saw or published, so a session that
    /// starts mid-stream does not replay whatever was already on the clipboard.
    std::optional<std::size_t> last_image;
#endif
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
     * @brief Wait out the klipper retry interval, or wake early on shutdown.
     * @details A plain sleep would not be interruptible: `stop()` only notifies
     *          the condition variable, and the shutdown watchdog in main.cpp
     *          allows ten seconds in total before it skips restoring the
     *          display state. Waiting on the same variable keeps the retry
     *          cadence while letting shutdown finish immediately.
     */
    void
    wait_retry() {
      std::unique_lock<std::mutex> lk { write_mu };
      write_cv.wait_for(lk, kKlipperRetryInterval, [] {
        return !running.load(std::memory_order_acquire);
      });
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    /**
     * @brief Copy the current compositor provider handle.
     * @details Holding the shared_ptr keeps the watcher alive for the duration
     *          of one call even if `stop()` concurrently drops the owner; the
     *          raw unique_ptr it used to be was read and reset from three
     *          threads without synchronisation.
     */
    std::shared_ptr<platf::clipboard_data::watcher_t>
    watcher_snapshot() {
      std::lock_guard<std::mutex> lk { watcher_mu };
      return data_watcher;
    }
#endif

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

    /**
     * @brief Hand a finished item to the bridge.
     * @details One route for both halves: items that fit travel inline, larger
     *          ones go to the blob store as a REF descriptor, exactly the way
     *          the GUI agent does it on Windows.
     */
    void
    enqueue_item(std::uint8_t kind, std::string_view mime, const payload_t &bytes, std::string_view what) {
      if (bytes.size() > kInlineThresholdBytes) {
        auto stored = clipboard_blob_store::put(bytes, std::string { mime });
        if (!stored.ok) {
          BOOST_LOG(warning) << "Host clipboard "sv << what << " could not be stored for out-of-band sync ("sv
                             << bytes.size() << " bytes): "sv << stored.err;
          return;
        }
        BOOST_LOG(debug) << "Posting host clipboard "sv << what << " as an out-of-band blob ("sv << bytes.size()
                         << " bytes, id "sv << stored.id << ")"sv;
        clipboard_bridge::bridge_t::instance().enqueue_outbound(
          clipboard_bridge::kBroadcast,
          clipboard_wire::encode_ref(stored.id, mime, bytes.size()));
        return;
      }

      BOOST_LOG(debug) << "Posting host clipboard "sv << what << " to clients ("sv << bytes.size() << " bytes)"sv;
      clipboard_bridge::bridge_t::instance().enqueue_outbound(
        clipboard_bridge::kBroadcast,
        clipboard_wire::encode(kind, 0, std::span<const std::uint8_t> { bytes }));
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    /// Cheap content identity for the "unchanged picture" test; the bytes are
    /// too large to keep around just to compare.
    std::size_t
    fingerprint(const payload_t &bytes) {
      std::size_t hash = 1469598103934665603ULL;
      for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
      }
      return hash ^ (bytes.size() * 1099511628211ULL);
    }

    /**
     * @brief Desktop clipboard -> clients, image half.
     * @details Called on the image watcher's thread. The echo ring and the
     *          session check mirror the text path so both halves stay silent in
     *          the same situations.
     */
    void
    on_desktop_image(payload_t png) {
      if (png.empty()) {
        return;
      }

      const auto fp = fingerprint(png);
      std::optional<std::size_t> previous;
      {
        std::lock_guard<std::mutex> lk { image_mu };
        previous = last_image;
        last_image = fp;
      }

      if (!config::input.clipboard_sync) {
        return;
      }
      if (clipboard_bridge::bridge_t::instance().session_count() == 0) {
        return;
      }
      if (previous && *previous == fp) {
        BOOST_LOG(debug) << "Clipboard image is unchanged; skipping"sv;
        return;
      }

      bool is_echo = false;
      {
        std::lock_guard<std::mutex> lk { echo_mu };
        is_echo = echo_ring.is_echo(clipboard_bridge::kKindPng, std::span<const std::uint8_t> { png });
      }
      if (is_echo) {
        BOOST_LOG(debug) << "Clipboard image matches what a client wrote; skipping"sv;
        return;
      }

      enqueue_item(clipboard_bridge::kKindPng, clipboard_bridge::kMimePng, png, "image"sv);
    }

    /**
     * @brief Desktop -> clients, text over the compositor's clipboard.
     * @details Only used when klipper is absent; mirrors the klipper poll path
     *          (same echo suppression, same empty/NUL rejection, same
     *          "unchanged is not a change" rule).
     */
    void
    on_desktop_text(std::string text) {
      if (text.empty() || text.find('\0') != std::string::npos) {
        return;
      }

      {
        std::lock_guard<std::mutex> lk { text_mu };
        if (last_text && *last_text == text) {
          return;
        }
        last_text = text;
      }

      if (!config::input.clipboard_sync) {
        return;
      }
      if (clipboard_bridge::bridge_t::instance().session_count() == 0) {
        return;
      }

      bool is_echo = false;
      {
        std::lock_guard<std::mutex> lk { echo_mu };
        const auto bytes = std::span<const std::uint8_t> {
          reinterpret_cast<const std::uint8_t *>(text.data()), text.size() };
        is_echo = echo_ring.is_echo(clipboard_bridge::kKindText, bytes);
      }
      if (is_echo) {
        BOOST_LOG(debug) << "Clipboard text matches what a client wrote; skipping"sv;
        return;
      }

      enqueue_item(clipboard_bridge::kKindText, clipboard_bridge::kMimeText,
        payload_t { text.begin(), text.end() }, "text"sv);
    }

    /**
     * @brief Clients -> desktop, image half.
     * @details Recorded in the echo ring before the compositor sees it, so the
     *          selection event our own write produces is not posted back.
     */
    void
    apply_inbound_image(const payload_t &png) {
      if (png.empty()) {
        return;
      }
      const auto watcher = watcher_snapshot();
      if (!watcher) {
        BOOST_LOG(debug) << "Inbound clipboard image ignored: no image provider on this session"sv;
        return;
      }

      {
        std::lock_guard<std::mutex> lk { echo_mu };
        echo_ring.record(clipboard_bridge::kKindPng, std::span<const std::uint8_t> { png });
      }
      {
        std::lock_guard<std::mutex> lk { image_mu };
        last_image = fingerprint(png);
      }

      if (!watcher->set_image(png)) {
        BOOST_LOG(warning) << "Inbound clipboard image could not be queued for the desktop"sv;
      }
    }
#else
    /**
     * @brief Clients -> desktop, image half; nothing to publish without
     *        Wayland (the desktop clipboard has no compositor provider here).
     */
    void
    apply_inbound_image(const payload_t &) {
      BOOST_LOG(debug) << "Inbound clipboard image ignored: built without Wayland support"sv;
    }
#endif

    /**
     * @brief Record and hand a peer-side text change to the klipper writer.
     */
    void
    apply_inbound_text(const std::string &text) {
      // The GUI agent drops inbound text that is empty or carries embedded
      // NULs (clipboard.rs accepts only `!s.is_empty() && !s.contains('\0')`);
      // klipper cannot take either, and an empty post would blank the desktop
      // clipboard.
      if (text.empty() || text.find('\0') != std::string::npos) {
        BOOST_LOG(debug) << "Ignoring inbound clipboard text that is empty or contains NUL bytes"sv;
        return;
      }

      const auto bytes = std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size() };
      {
        std::lock_guard<std::mutex> lk(echo_mu);
        echo_ring.record(clipboard_bridge::kKindText, bytes);
      }

#ifdef SUNSHINE_BUILD_WAYLAND
      if (ext_text.load(std::memory_order_acquire)) {
        // No klipper here: publish through the compositor and remember the
        // value, so the selection event our own write produces is dropped both
        // by the echo ring and by the unchanged check.
        {
          std::lock_guard<std::mutex> lk { text_mu };
          last_text = text;
        }
        const auto watcher = watcher_snapshot();
        if (!watcher) {
          BOOST_LOG(debug) << "Inbound clipboard text ignored: no data-control provider"sv;
          return;
        }
        watcher->set_text(text);
        return;
      }
#endif

      // Recorded before the write (as before) so the echo of our own change is
      // suppressed even though the poll thread applies it slightly later.
      queue_klipper_write(text);
    }

    /**
     * @brief Resolve a kKindRef frame against the local blob store.
     * @details Text lands in klipper, PNG in the compositor's clipboard; file
     *          offers still belong to the GUI agent. The mime decides, exactly
     *          as the agent's inbound handler does.
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

      if (!clipboard_wire::is_text_mime(descriptor->mime) && !clipboard_wire::is_png_mime(descriptor->mime)) {
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

      if (clipboard_wire::is_png_mime(descriptor->mime)) {
        apply_inbound_image(blob.bytes);
        return;
      }

      apply_inbound_text(std::string { blob.bytes.begin(), blob.bytes.end() });
    }

    void
    on_inbound(clipboard_bridge::session_id, const payload_t &bytes) {
      if (!config::input.clipboard_sync) {
        return;
      }
      if (!running.load(std::memory_order_acquire)) {
        // Shutdown already started; the provider may be gone.
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

      if (header->kind == clipboard_bridge::kKindPng) {
        const auto payload = clipboard_wire::payload_of(bytes, *header);
        apply_inbound_image(payload_t { payload.begin(), payload.end() });
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
#ifdef SUNSHINE_BUILD_WAYLAND
        if (ext_text.load(std::memory_order_acquire)) {
          // The compositor carries text now; this thread has nothing to read.
          // It still stamps liveness, which is what gates the wire capability.
          if (bus) {
            sd_bus_unref(bus);
            bus = nullptr;
          }
          klipper_available = false;
          change_signal_subscribed = false;
          clipboard_bridge::bridge_t::instance().notify_gui_alive();

          std::unique_lock<std::mutex> lk { write_mu };
          write_cv.wait_for(lk, kBusWaitInterval, [&] {
            return !running.load(std::memory_order_acquire);
          });
          continue;
        }
#endif

        if (!klipper_available) {
          bus = open_bus();
          klipper_available = bus != nullptr;
          change_signal_subscribed = false;
          if (!klipper_available) {
            if (!logged_unavailable) {
              BOOST_LOG(info) << "Host clipboard sync unavailable (no session bus); will keep retrying"sv;
              logged_unavailable = true;
            }
            wait_retry();
            continue;
          }
          if (logged_unavailable) {
            BOOST_LOG(info) << "Host clipboard sync provider online"sv;
            logged_unavailable = false;
          }
        }

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
          wait_retry();
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
          wait_retry();
          continue;
        }

        // Only stamp liveness once the provider actually answered: a desktop
        // with a session bus but no klipper and no data-control keeps retrying
        // every ten seconds, and the agent-side window is thirty seconds, so
        // stamping before the read would advertise a text clipboard that
        // nothing can fill.
        clipboard_bridge::bridge_t::instance().notify_gui_alive();

        const auto session_count = clipboard_bridge::bridge_t::instance().session_count();
        if (!had_sessions && session_count > 0) {
          // Baseline the current clipboard when a session starts so the first
          // change on the host (not the state at connect) is what gets posted.
          last_seen = content;
        }
        had_sessions = session_count > 0;

        // The agent maps an empty clipboard read to "nothing to post" (its
        // text reader keeps only `Ok(t) if !t.is_empty()`): klipper returns an
        // empty string for non-text content (e.g. a copied image), which must
        // not overwrite the clients' clipboard with empty text.
        if (!content.empty() && content != last_seen && config::input.clipboard_sync && session_count > 0) {
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
          else {
            enqueue_item(clipboard_bridge::kKindText, clipboard_bridge::kMimeText,
              payload_t { content.begin(), content.end() }, "text"sv);
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

    /**
     * @brief Whether KDE's klipper service owns its name on the session bus.
     * @details klipper is the text transport *when it exists*. A desktop without
     *          it (niri, sway, Hyprland, plain weston) needs the compositor's
     *          data-control clipboard for text as well, otherwise copying text
     *          inside a session silently does nothing on those desktops.
     */
    bool
    klipper_present() {
      sd_bus *bus = open_bus();
      if (!bus) {
        return false;
      }
      std::string ignored;
      const bool present = klipper_get(bus, ignored);
      sd_bus_unref(bus);
      return present;
    }

  }  // namespace

  void
  start() {
    if (running.exchange(true)) {
      return;
    }

#ifdef SUNSHINE_BUILD_WAYLAND
    if (platf::clipboard_data::available()) {
      auto watcher = std::make_shared<platf::clipboard_data::watcher_t>();
      if (watcher->start(&on_desktop_image, &on_desktop_text)) {
        {
          std::lock_guard<std::mutex> lk { watcher_mu };
          data_watcher = watcher;
        }

        // klipper is the text transport only while the desktop has it. Probe
        // once, before the poll thread starts, so both halves agree.
        if (!klipper_present()) {
          ext_text.store(true, std::memory_order_release);
          BOOST_LOG(info) << "Host clipboard: no klipper on this desktop; text goes through the compositor"sv;
        }
      }
      else {
        BOOST_LOG(warning) << "Host clipboard: data-control provider failed to start; klipper only"sv;
      }
    }
    else {
      BOOST_LOG(info) << "Host clipboard: no data-control protocol on this session; image sync off"sv;
    }
#else
    if (!klipper_present()) {
      BOOST_LOG(warning) << "Host clipboard: no klipper on this desktop and no data-control support; text sync off"sv;
    }
#endif

    // Registered last: the enet control thread fires this on inbound frames, so
    // the provider has to exist (or be known absent) before it can be observed.
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

#ifdef SUNSHINE_BUILD_WAYLAND
    // After the poll thread: the watcher only publishes towards clients, so
    // stopping it last cannot race a text publish. Taking ownership out of the
    // shared slot keeps it alive while it stops.
    std::shared_ptr<platf::clipboard_data::watcher_t> watcher;
    {
      std::lock_guard<std::mutex> lk { watcher_mu };
      watcher = std::move(data_watcher);
      data_watcher.reset();
    }
    if (watcher) {
      watcher->stop();
    }
#endif

    BOOST_LOG(info) << "Host clipboard provider stopped"sv;
  }

  bool
  image_available() {
#ifdef SUNSHINE_BUILD_WAYLAND
    return watcher_snapshot() != nullptr;
#else
    return false;
#endif
  }

}  // namespace clipboard_host
