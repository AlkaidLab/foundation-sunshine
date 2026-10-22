/**
 * @file src/platform/linux/clipboard_data.cpp
 * @brief Text and image support for the host-side clipboard provider over
 *        `ext_data_control_v1`.
 *
 * Two directions, one connection:
 *
 *   desktop -> clients: the compositor sends `selection` naming an offer; we
 *     pick the flavour the wire can carry (PNG when offered, otherwise the most
 *     specific UTF-8 text spelling) and `receive()` it on a pipe, read the
 *     bytes, and hand them to the matching sink.
 *
 *   clients -> desktop: we create a data source offering that flavour and call
 *     `set_selection()`; the compositor asks us for the bytes through `send()`
 *     whenever a paste happens, so the source stays alive until another client
 *     takes the selection (we then see `cancelled`).
 *
 * `ext_data_control_v1` is the modern spelling of the data-control protocol and
 * is what Plasma 6 implements; wlroots compositors still only export the
 * wlr-flavoured name, so this module stays inert there rather than pretending
 * to work.
 */
#include "clipboard_data.h"

#include "clipboard_mime.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  #include <algorithm>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <cstring>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <unordered_map>
  #include <utility>
  #include <vector>

  #include <fcntl.h>
  #include <poll.h>
  #include <unistd.h>

  #include <wayland-client.h>

  #include "ext-data-control-v1.h"
  #include "src/clipboard_blob_store.h"
  #include "src/logging.h"

namespace platf::clipboard_data {
  using namespace std::string_view_literals;

  namespace {
    /// The only image flavour the wire contract knows (clipboard_bridge::kMimePng).
    constexpr std::string_view kPngMime = clipboard_mime::png_mime;

    /**
     * @brief How long a single pipe transfer may take before we give up.
     * @details Bounded because the peer is another process: a wedged source app
     *          must not stall the watcher thread indefinitely.
     */
    constexpr auto kTransferTimeout = std::chrono::seconds { 10 };

    /// Never buffer more than the blob store would accept for the same item.
    constexpr std::size_t kMaxTransferBytes = clipboard_blob_store::kMaxBlobBytes;

    /// Close a descriptor exactly once.
    void
    close_fd(int &fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }

    // -----------------------------------------------------------------------
    // registry probe (available())
    // -----------------------------------------------------------------------
    struct probe_t {
      bool manager = false;
      bool seat = false;
    };

    void
    probe_global(void *data, wl_registry *, std::uint32_t, const char *interface, std::uint32_t) {
      auto &probe = *static_cast<probe_t *>(data);
      if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        probe.manager = true;
      }
      else if (std::strcmp(interface, "wl_seat") == 0) {
        probe.seat = true;
      }
    }

    void
    probe_global_remove(void *, wl_registry *, std::uint32_t) {}

    constexpr wl_registry_listener kProbeListener = { probe_global, probe_global_remove };

    // -----------------------------------------------------------------------
    // bounded pipe helpers
    // -----------------------------------------------------------------------
  }  // namespace

  bool
  available() {
    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
      return false;
    }

    bool ok = false;
    if (wl_registry *registry = wl_display_get_registry(display)) {
      probe_t probe;
      wl_registry_add_listener(registry, &kProbeListener, &probe);
      // Two roundtrips: the first collects the globals, the second catches any
      // that appeared while the first was in flight.
      if (wl_display_roundtrip(display) >= 0 && wl_display_roundtrip(display) >= 0) {
        ok = probe.manager && probe.seat;
        if (!ok) {
          BOOST_LOG(debug) << "Clipboard ext: ext_data_control_manager_v1="sv << probe.manager
                           << " seat="sv << probe.seat << " - data-control sync unavailable"sv;
        }
      }
      wl_registry_destroy(registry);
    }

    wl_display_disconnect(display);
    return ok;
  }

  struct watcher_t::impl_t {
    /// Per-offer bookkeeping; the listener payload lives here so its address
    /// stays valid for as long as the offer does.
    struct offer_t {
      std::vector<std::string> mimes;
      impl_t *owner = nullptr;
      ext_data_control_offer_v1 *offer = nullptr;
    };

    // ---- cross-thread ----
    std::thread thread;
    std::atomic<bool> running { false };
    std::mutex mu;
    /// At most one queued write; the newest wins whatever its flavour.
    struct pending_t {
      bytes_t bytes;
      bool image = false;
    };
    std::optional<pending_t> pending;
    sink_fn image_sink;
    text_sink_fn text_sink;
    int wake_read = -1;
    int wake_write = -1;

    // ---- thread-owned wayland state ----
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    ext_data_control_manager_v1 *manager = nullptr;
    wl_seat *seat = nullptr;
    ext_data_control_device_v1 *device = nullptr;
    ext_data_control_source_v1 *source = nullptr;
    /// Bytes the current data source serves on send(), and what they are.
    bytes_t source_bytes;
    bool source_is_image = false;
    std::unordered_map<ext_data_control_offer_v1 *, offer_t> offers;
    /// The offer named by the last `selection` event.
    ext_data_control_offer_v1 *selected = nullptr;

    // ---- in-flight transfers, pumped by the loop and never blocking it ----
    /// Compositor -> us: a pipe we are reading an offered item out of.
    int recv_fd = -1;
    bytes_t recv_buf;
    bool recv_is_image = false;
    std::chrono::steady_clock::time_point recv_deadline;
    /// Us -> compositor: pipes a paste is draining, one per pending send().
    struct out_transfer_t {
      int fd = -1;
      std::size_t offset = 0;
      std::chrono::steady_clock::time_point deadline;
    };
    std::vector<out_transfer_t> out_transfers;

    // -------------------------------------------------------------------
    // client -> desktop
    // -------------------------------------------------------------------
    void
    publish_source(bytes_t bytes, bool image) {
      if (!manager || !device) {
        return;
      }

      if (source) {
        // Replacing the selection: the compositor keeps its own reference, so
        // the old source may go away right after the new offer is installed.
        ext_data_control_source_v1_destroy(source);
        source = nullptr;
      }
      source_bytes.clear();
      source_is_image = false;

      source = ext_data_control_manager_v1_create_data_source(manager);
      if (!source) {
        BOOST_LOG(warning) << "Clipboard ext: could not create a data source"sv;
        return;
      }

      static constexpr ext_data_control_source_v1_listener kSourceListener = {
        // send(): the compositor wants the bytes for a paste.
        [](void *data, ext_data_control_source_v1 *, const char *mime, int32_t fd) {
          auto &state = *static_cast<impl_t *>(data);
          if (!mime || state.source_bytes.empty()) {
            ::close(fd);
            return;
          }
          // Serve whichever flavour the paste asked for, but only one we
          // actually advertised for this item.
          const bool matches = state.source_is_image ?
                                 kPngMime == mime :
                                 clipboard_mime::is_text_mime(mime);
          if (!matches) {
            ::close(fd);
            return;
          }
          // Serve from the loop, never from this callback: an item rarely fits
          // in the pipe buffer, and blocking here would freeze the very loop
          // that has to drain it.
          const int flags = ::fcntl(fd, F_GETFL, 0);
          if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          }
          state.out_transfers.push_back({ fd, 0, std::chrono::steady_clock::now() + kTransferTimeout });
        },
        // cancelled(): another client took the selection. Keep the proxy until
        // the next publish and stop advertising bytes we no longer serve.
        [](void *data, ext_data_control_source_v1 *) {
          BOOST_LOG(debug) << "Clipboard ext: our selection was cancelled"sv;
          static_cast<impl_t *>(data)->source_bytes.clear();
          static_cast<impl_t *>(data)->source_is_image = false;
        },
      };
      ext_data_control_source_v1_add_listener(source, &kSourceListener, this);
      if (image) {
        const std::string png_mime { clipboard_mime::png_mime };
        ext_data_control_source_v1_offer(source, png_mime.c_str());
      }
      else {
        // Advertise every UTF-8 spelling a paste might look for; the
        // compositor picks one and send() accepts any of them.
        for (const auto &mime: clipboard_mime::text_offers()) {
          ext_data_control_source_v1_offer(source, mime.c_str());
        }
      }

      source_bytes = std::move(bytes);
      source_is_image = image;
      ext_data_control_device_v1_set_selection(device, source);
      BOOST_LOG(debug) << "Clipboard ext: published "sv << source_bytes.size() << " bytes to the desktop as "sv
                       << (image ? "png"sv : "text"sv);
    }

    // -------------------------------------------------------------------
    // desktop -> clients
    // -------------------------------------------------------------------
    void
    drop_offers_except(ext_data_control_offer_v1 *keep) {
      for (auto it = offers.begin(); it != offers.end();) {
        auto *offer = it->first;
        if (offer == keep) {
          ++it;
          continue;
        }
        it = offers.erase(it);
        ext_data_control_offer_v1_destroy(offer);
      }
      if (selected != keep) {
        selected = keep;
      }
    }

    void
    handle_selection(ext_data_control_offer_v1 *offer) {
      // Offers from the previous round are dead once a new selection arrives.
      drop_offers_except(offer);

      if (!offer) {
        return;
      }

      const auto entry = offers.find(offer);
      if (entry == offers.end()) {
        return;
      }
      const auto choice = clipboard_mime::choose(entry->second.mimes);
      if (choice.kind == clipboard_mime::kind_e::none) {
        // Nothing the wire can carry (application/*, exotic image formats):
        // leave the clients' clipboard untouched rather than empty it.
        return;
      }

      start_receive(offer, choice.mime, choice.kind == clipboard_mime::kind_e::image);
    }

    /// Give up on the pipe we were reading; nothing is delivered.
    void
    abort_receive() {
      close_fd(recv_fd);
      recv_buf.clear();
    }

    /**
     * @brief Ask the compositor for one offered flavour and arm the loop to
     *        read its pipe.
     * @details The read is deliberately not done here: this runs inside
     *          dispatch_pending, and for our *own* source the bytes only arrive
     *          once the loop dispatches the resulting send() event.
     */
    void
    start_receive(ext_data_control_offer_v1 *offer, const std::string &mime, bool image) {
      abort_receive();

      int fds[2] { -1, -1 };
      if (::pipe2(fds, O_CLOEXEC) != 0) {
        BOOST_LOG(warning) << "Clipboard ext: pipe2 failed: "sv << std::strerror(errno);
        return;
      }

      ext_data_control_offer_v1_receive(offer, mime.c_str(), fds[1]);
      close_fd(fds[1]);
      const int flags = ::fcntl(fds[0], F_GETFL, 0);
      if (flags >= 0) {
        ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
      }

      recv_fd = fds[0];
      recv_is_image = image;
      recv_deadline = std::chrono::steady_clock::now() + kTransferTimeout;
      // Flush so the compositor starts pumping straight away.
      wl_display_flush(display);
    }

    /// Drain whatever the pipe has; completes the transfer on EOF.
    void
    pump_receive() {
      if (recv_fd < 0) {
        return;
      }

      std::uint8_t chunk[64 * 1024];
      for (;;) {
        const auto got = ::read(recv_fd, chunk, sizeof(chunk));
        if (got > 0) {
          if (recv_buf.size() + static_cast<std::size_t>(got) > kMaxTransferBytes) {
            BOOST_LOG(warning) << "Clipboard ext: item exceeds "sv << kMaxTransferBytes << " bytes; dropping"sv;
            abort_receive();
            return;
          }
          recv_buf.insert(recv_buf.end(), chunk, chunk + got);
          continue;
        }
        if (got == 0) {
          finish_receive();
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        BOOST_LOG(warning) << "Clipboard ext: read failed: "sv << std::strerror(errno);
        abort_receive();
        return;
      }
    }

    /// Close the pipe and deliver what we collected.
    void
    finish_receive() {
      close_fd(recv_fd);
      if (recv_buf.empty()) {
        return;
      }
      BOOST_LOG(debug) << "Clipboard ext: captured "sv << recv_buf.size() << " bytes from the desktop as "sv
                       << (recv_is_image ? "png"sv : "text"sv);
      if (recv_is_image) {
        if (image_sink) {
          image_sink(std::move(recv_buf));
        }
      }
      else if (text_sink) {
        text_sink(std::string { recv_buf.begin(), recv_buf.end() });
      }
      recv_buf.clear();
      recv_is_image = false;
    }

    /// Push pending paste bytes into their pipes; drop finished/expired ones.
    void
    pump_out_transfers() {
      const auto now = std::chrono::steady_clock::now();
      for (auto it = out_transfers.begin(); it != out_transfers.end();) {
        // The selection may have been cancelled mid-transfer, which empties
        // source_bytes; nothing more to serve then.
        if (it->offset >= source_bytes.size() || now > it->deadline) {
          if (now > it->deadline && it->offset < source_bytes.size()) {
            BOOST_LOG(warning) << "Clipboard ext: paste timed out after "sv << it->offset << " of "sv
                               << source_bytes.size() << " bytes"sv;
          }
          ::close(it->fd);
          it = out_transfers.erase(it);
          continue;
        }

        const auto put = ::write(it->fd, source_bytes.data() + it->offset, source_bytes.size() - it->offset);
        if (put < 0) {
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            ++it;
            continue;
          }
          // The pasting client went away; that is a normal failed paste.
          ::close(it->fd);
          it = out_transfers.erase(it);
          continue;
        }

        it->offset += static_cast<std::size_t>(put);
        if (it->offset >= source_bytes.size()) {
          ::close(it->fd);
          it = out_transfers.erase(it);
          continue;
        }
        ++it;
      }
    }

    // -------------------------------------------------------------------
    // connection setup
    // -------------------------------------------------------------------
    void
    create_device() {
      if (device || !manager || !seat) {
        return;
      }

      device = ext_data_control_manager_v1_get_data_device(manager, seat);
      if (!device) {
        BOOST_LOG(warning) << "Clipboard ext: could not create a data device"sv;
        return;
      }

      static constexpr ext_data_control_offer_v1_listener kOfferListener = {
        [](void *data, ext_data_control_offer_v1 *self_offer, const char *mime) {
          auto &entry = *static_cast<offer_t *>(data);
          if (mime) {
            entry.mimes.emplace_back(mime);
          }
          (void) self_offer;
        },
      };

      static constexpr ext_data_control_device_v1_listener kDeviceListener = {
        // data_offer: a new offer; its MIME list follows as `offer` events.
        [](void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
          auto &state = *static_cast<impl_t *>(data);
          auto &entry = state.offers[offer];
          entry.owner = &state;
          entry.offer = offer;
          ext_data_control_offer_v1_add_listener(offer, &kOfferListener, &entry);
        },
        // selection: the offer (or null) now owns the clipboard.
        [](void *data, ext_data_control_device_v1 *, ext_data_control_offer_v1 *offer) {
          static_cast<impl_t *>(data)->handle_selection(offer);
        },
        // finished: the compositor revoked our data device (session end).
        [](void *data, ext_data_control_device_v1 *) {
          BOOST_LOG(debug) << "Clipboard ext: data device finished"sv;
          static_cast<impl_t *>(data)->running.store(false, std::memory_order_release);
        },
        // primary_selection: the middle-click buffer, outside the wire contract.
        [](void *, ext_data_control_device_v1 *, ext_data_control_offer_v1 *) {},
      };

      ext_data_control_device_v1_add_listener(device, &kDeviceListener, this);
    }

    void
    setup_registry() {
      static constexpr wl_registry_listener kRegistryListener = {
        [](void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t) {
          auto &state = *static_cast<impl_t *>(data);
          if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 && !state.manager) {
            state.manager = static_cast<ext_data_control_manager_v1 *>(
              wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
          }
          else if (std::strcmp(interface, "wl_seat") == 0 && !state.seat) {
            state.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
          }
          // Either global may arrive first; the device needs both.
          state.create_device();
        },
        [](void *, wl_registry *, std::uint32_t) {},
      };

      registry = wl_display_get_registry(display);
      if (!registry) {
        return;
      }
      wl_registry_add_listener(registry, &kRegistryListener, this);
      // Two roundtrips: globals, then anything the first one raced with.
      wl_display_roundtrip(display);
      wl_display_roundtrip(display);
      create_device();
    }

    // -------------------------------------------------------------------
    // event loop
    // -------------------------------------------------------------------
    void
    apply_pending() {
      std::optional<pending_t> item;
      {
        std::lock_guard<std::mutex> lk { mu };
        item = std::move(pending);
        pending.reset();
      }
      if (item && running.load(std::memory_order_acquire)) {
        publish_source(std::move(item->bytes), item->image);
        wl_display_flush(display);
      }
    }

    void
    loop() {
      setup_registry();
      if (!manager || !device) {
        BOOST_LOG(warning) << "Clipboard ext: no data device; data-control sync stays off"sv;
        return;
      }
      BOOST_LOG(info) << "Clipboard data-control online"sv;

      std::vector<pollfd> fds;
      std::vector<out_transfer_t *> out_slots;

      while (running.load(std::memory_order_acquire)) {
        while (wl_display_prepare_read(display) != 0) {
          if (wl_display_dispatch_pending(display) < 0) {
            running.store(false, std::memory_order_release);
            break;
          }
        }
        if (!running.load(std::memory_order_acquire)) {
          wl_display_cancel_read(display);
          break;
        }

        const int flushed = wl_display_flush(display);
        if (flushed < 0 && errno != EAGAIN) {
          wl_display_cancel_read(display);
          BOOST_LOG(debug) << "Clipboard ext: display connection failed: "sv << std::strerror(errno);
          running.store(false, std::memory_order_release);
          break;
        }

        fds.clear();
        out_slots.clear();
        fds.push_back({ wl_display_get_fd(display), POLLIN, 0 });
        fds.push_back({ wake_read, POLLIN, 0 });
        if (recv_fd >= 0) {
          fds.push_back({ recv_fd, POLLIN, 0 });
        }
        for (auto &transfer : out_transfers) {
          out_slots.push_back(&transfer);
          fds.push_back({ transfer.fd, POLLOUT, 0 });
        }

        // Sleep no longer than the earliest transfer deadline, so a stalled
        // peer is noticed even when the compositor stays silent.
        auto earliest = std::chrono::steady_clock::time_point::max();
        if (recv_fd >= 0) {
          earliest = std::min(earliest, recv_deadline);
        }
        for (const auto &transfer : out_transfers) {
          earliest = std::min(earliest, transfer.deadline);
        }
        int timeout = -1;
        if (earliest != std::chrono::steady_clock::time_point::max()) {
          const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              earliest - std::chrono::steady_clock::now())
                              .count();
          timeout = left < 0 ? 0 : static_cast<int>(std::min<long long>(left + 1, 60000));
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
        if (ready < 0) {
          wl_display_cancel_read(display);
          if (errno == EINTR) {
            continue;
          }
          BOOST_LOG(debug) << "Clipboard ext: poll failed: "sv << std::strerror(errno);
          running.store(false, std::memory_order_release);
          break;
        }

        if (fds[0].revents & POLLIN) {
          if (wl_display_read_events(display) < 0) {
            BOOST_LOG(debug) << "Clipboard ext: read_events failed"sv;
            running.store(false, std::memory_order_release);
            break;
          }
        }
        else {
          wl_display_cancel_read(display);
        }

        if (wl_display_dispatch_pending(display) < 0) {
          BOOST_LOG(debug) << "Clipboard ext: dispatch failed"sv;
          running.store(false, std::memory_order_release);
          break;
        }

        if (fds[1].revents & POLLIN) {
          char drain[64];
          while (::read(wake_read, drain, sizeof(drain)) > 0) {
          }
        }

        if (recv_fd >= 0) {
          pump_receive();
        }
        if (recv_fd >= 0 && std::chrono::steady_clock::now() > recv_deadline) {
          BOOST_LOG(warning) << "Clipboard ext: transfer timed out after "sv << recv_buf.size()
                             << " bytes; dropping"sv;
          abort_receive();
        }
        pump_out_transfers();

        // Publishers are drained every iteration: the wake pipe only shortens
        // the wait, it is not the source of truth.
        apply_pending();
      }

      abort_receive();
      for (auto &transfer : out_transfers) {
        ::close(transfer.fd);
      }
      out_transfers.clear();
    }

    void
    teardown() {
      if (source) {
        ext_data_control_source_v1_destroy(source);
        source = nullptr;
      }
      for (auto &entry : offers) {
        ext_data_control_offer_v1_destroy(entry.first);
      }
      offers.clear();
      selected = nullptr;
      if (device) {
        ext_data_control_device_v1_destroy(device);
        device = nullptr;
      }
      if (seat) {
        wl_seat_destroy(seat);
        seat = nullptr;
      }
      if (manager) {
        ext_data_control_manager_v1_destroy(manager);
        manager = nullptr;
      }
      if (registry) {
        wl_registry_destroy(registry);
        registry = nullptr;
      }
    }
  };

  watcher_t::watcher_t(): _impl { std::make_unique<impl_t>() } {}

  watcher_t::~watcher_t() {
    stop();
  }

  bool
  watcher_t::start(sink_fn sink, text_sink_fn text_sink) {
    if (_impl->running.exchange(true)) {
      return true;
    }

    _impl->image_sink = std::move(sink);
    _impl->text_sink = std::move(text_sink);

    int fds[2] { -1, -1 };
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
      BOOST_LOG(warning) << "Clipboard ext: wake pipe failed: "sv << std::strerror(errno);
      _impl->running.store(false, std::memory_order_release);
      return false;
    }
    _impl->wake_read = fds[0];
    _impl->wake_write = fds[1];

    _impl->display = wl_display_connect(nullptr);
    if (!_impl->display) {
      BOOST_LOG(info) << "Clipboard ext: no Wayland session; data-control sync stays off"sv;
      close_fd(_impl->wake_read);
      close_fd(_impl->wake_write);
      _impl->running.store(false, std::memory_order_release);
      return false;
    }

    _impl->thread = std::thread([this] {
      _impl->loop();
      _impl->teardown();
      if (_impl->display) {
        wl_display_disconnect(_impl->display);
        _impl->display = nullptr;
      }
    });

    return true;
  }

  bool
  watcher_t::set_text(std::string text) {
    if (!_impl->running.load(std::memory_order_acquire) || text.empty()) {
      return false;
    }

    impl_t::pending_t item;
    item.bytes.assign(text.begin(), text.end());
    item.image = false;

    {
      std::lock_guard<std::mutex> lk { _impl->mu };
      _impl->pending = std::move(item);
    }

    if (_impl->wake_write >= 0) {
      const char byte = 1;
      [[maybe_unused]] const auto ignored = ::write(_impl->wake_write, &byte, 1);
    }
    return true;
  }

  void
  watcher_t::stop() {
    if (!_impl->running.exchange(false)) {
      if (_impl->thread.joinable()) {
        _impl->thread.join();
      }
      return;
    }

    // Wake the loop out of poll(); it sees `running == false` next iteration.
    if (_impl->wake_write >= 0) {
      const char byte = 1;
      [[maybe_unused]] const auto ignored = ::write(_impl->wake_write, &byte, 1);
    }
    if (_impl->thread.joinable()) {
      _impl->thread.join();
    }

    close_fd(_impl->wake_read);
    close_fd(_impl->wake_write);
    _impl->image_sink = nullptr;
    _impl->text_sink = nullptr;
  }

  bool
  watcher_t::set_image(png_t png) {
    if (!_impl->running.load(std::memory_order_acquire) || png.empty()) {
      return false;
    }

    {
      std::lock_guard<std::mutex> lk { _impl->mu };
      // Newest wins: a stale clipboard is worthless once a newer one lands.
      _impl->pending = impl_t::pending_t { std::move(png), true };
    }

    if (_impl->wake_write >= 0) {
      const char byte = 1;
      [[maybe_unused]] const auto ignored = ::write(_impl->wake_write, &byte, 1);
    }
    return true;
  }
}  // namespace platf::clipboard_data

#endif  // SUNSHINE_BUILD_WAYLAND
