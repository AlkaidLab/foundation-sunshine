/**
 * @file src/platform/windows/cursor_tracker.cpp
 * @brief Polls the Windows cursor for cursor protocol v2.
 *
 * Runs only while a session is subscribed. Sunshine runs inside the console
 * session (as SYSTEM in service mode), so the thread binds to the current input
 * desktop, which also covers the secure desktop.
 */
// platform includes first: winsock2.h must precede windows.h
#include <winsock2.h>
#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "src/cursor_channel.h"
#include "src/host_cursor.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace host_cursor::tracker {
  namespace {
    constexpr auto poll_interval = 5ms;  // 200 Hz
    constexpr auto shape_revalidate_interval = 1s;
    // Rebinding is cheap and catches input-desktop switches (UAC, lock screen)
    // that do not make GetCursorInfo() fail.
    constexpr auto desktop_rebind_interval = 1s;
    constexpr auto cost_log_interval = 30s;

    // Binds the calling thread to the current input desktop. A desktop this
    // process cannot open (the secure desktop when Sunshine is not SYSTEM) makes
    // cursor information unavailable; the caller logs the change.
    bool
    bind_input_desktop() {
      HDESK desktop = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
      if (!desktop) {
        return false;
      }
      const bool bound = SetThreadDesktop(desktop);
      CloseDesktop(desktop);
      return bound;
    }

    std::optional<std::vector<std::uint8_t>>
    bitmap_bits(HBITMAP bitmap, const BITMAP &bm) {
      std::vector<std::uint8_t> bits(std::size_t(bm.bmWidthBytes) * bm.bmHeight);
      if (bits.empty() || GetBitmapBits(bitmap, LONG(bits.size()), bits.data()) != LONG(bits.size())) {
        return std::nullopt;
      }
      return bits;
    }

    // Sunshine's system DPI is the primary display's when it started, which differs
    // from the streamed display's when it started at another scale (at boot, say,
    // before the virtual display existed). GetIconInfo() returns standard-size
    // cursors at the size for the system DPI.
    double
    gdi_display_scale(std::uint32_t width, std::uint32_t height, POINT position) {
      UINT dpi_x = 0, dpi_y = 0;
      if (FAILED(GetDpiForMonitor(MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        return 1;
      }
      const auto read_standard = GetSystemMetricsForDpi(SM_CXCURSOR, GetDpiForSystem());
      const auto display_standard = GetSystemMetricsForDpi(SM_CXCURSOR, dpi_x);
      return host_cursor_bitmap::gdi_display_scale(width, height, std::uint32_t((std::max)(read_standard, 0)), std::uint32_t((std::max)(display_standard, 0)));
    }

    // GetIconInfo() and the raw bitmap bits, as GDI draws them. Masks are
    // WORD-aligned 1 bpp rows; a monochrome cursor stacks its AND and XOR masks
    // in one bitmap.
    std::optional<host_cursor_bitmap::image_t>
    convert_hcursor(HCURSOR cursor, POINT position) {
      ICONINFO info {};
      if (!GetIconInfo(cursor, &info)) {
        return std::nullopt;
      }
      auto free_bitmaps = util::fail_guard([&]() {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
      });
      const auto at_display_size = [&](std::optional<host_cursor_bitmap::image_t> image, LONG width, LONG height) {
        if (image) {
          image->pixel_scale *= gdi_display_scale(std::uint32_t(width), std::uint32_t(height), position);
        }
        return image;
      };

      BITMAP mask_bm {};
      if (!info.hbmMask || !GetObject(info.hbmMask, sizeof(mask_bm), &mask_bm) || mask_bm.bmBitsPixel != 1) {
        return std::nullopt;
      }
      auto mask = bitmap_bits(info.hbmMask, mask_bm);
      if (!mask) {
        return std::nullopt;
      }

      if (!info.hbmColor) {
        return at_display_size(
          host_cursor_bitmap::from_monochrome(mask_bm.bmWidth, mask_bm.bmHeight / 2, mask_bm.bmWidthBytes, *mask, info.xHotspot, info.yHotspot),
          mask_bm.bmWidth,
          mask_bm.bmHeight / 2);
      }

      BITMAP color_bm {};
      if (!GetObject(info.hbmColor, sizeof(color_bm), &color_bm) || color_bm.bmWidth <= 0 || color_bm.bmHeight <= 0) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> color;
      if (color_bm.bmBitsPixel == 32) {
        auto bits = bitmap_bits(info.hbmColor, color_bm);
        if (!bits) return std::nullopt;
        color = std::move(*bits);
      }
      else {
        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = color_bm.bmWidth;
        bmi.bmiHeader.biHeight = -color_bm.bmHeight;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        color.resize(std::size_t(color_bm.bmWidth) * color_bm.bmHeight * 4);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return std::nullopt;
        const auto rows = GetDIBits(dc, info.hbmColor, 0, color_bm.bmHeight, color.data(), &bmi, DIB_RGB_COLORS);
        DeleteDC(dc);
        if (rows != color_bm.bmHeight) return std::nullopt;
        color_bm.bmWidthBytes = color_bm.bmWidth * 4;
      }

      bool has_alpha = false;
      for (std::size_t i = 3; i < color.size() && !has_alpha; i += 4) has_alpha = color[i] != 0;
      if (has_alpha) {
        return at_display_size(
          host_cursor_bitmap::from_color(color_bm.bmWidth, color_bm.bmHeight, color_bm.bmWidthBytes, color, info.xHotspot, info.yHotspot),
          color_bm.bmWidth,
          color_bm.bmHeight);
      }
      return at_display_size(
        host_cursor_bitmap::from_color_and_mask(color_bm.bmWidth, color_bm.bmHeight, color_bm.bmWidthBytes, color, mask_bm.bmWidthBytes, *mask, info.xHotspot, info.yHotspot),
        color_bm.bmWidth,
        color_bm.bmHeight);
    }

    class tracker_t {
    public:
      std::shared_ptr<void>
      acquire() {
        std::lock_guard lg { _users_lock };
        if (_users++ == 0) {
          BOOST_LOG(info) << "Cursor protocol v2: tracker started"sv;
          cursor_channel::add_shape_consumer();
          _running.store(true, std::memory_order_release);
          _thread = std::jthread([this](std::stop_token stop) {
            run(stop);
          });
        }
        return std::shared_ptr<void>(this, [](void *self) {
          static_cast<tracker_t *>(self)->release();
        });
      }

      snapshot_t
      latest() {
        std::lock_guard lg { _snapshot_lock };
        return _snapshot;
      }

      bool
      running() const {
        return _running.load(std::memory_order_acquire);
      }

      injection_history_t injections;

    private:
      void
      release() {
        // Join under the lock so a subscription racing this release never starts a
        // second poller while the old one finishes its last poll (at most one interval).
        std::lock_guard lg { _users_lock };
        if (--_users != 0) return;
        _running.store(false, std::memory_order_release);
        _thread.request_stop();
        _thread.join();
        cursor_channel::remove_shape_consumer();
        BOOST_LOG(info) << "Cursor protocol v2: tracker stopped"sv;
      }

      // The shape Desktop Duplication or the VDD driver reported, while one of them
      // captures: that is what Windows draws on the captured output, at its DPI and
      // the user's cursor size. Otherwise nothing, and GetIconInfo() is used.
      std::optional<std::uint32_t>
      capture_shape_id() {
        if (!cursor_channel::producer_available()) {
          _capture_shape_id.reset();
          return std::nullopt;
        }
        cursor_channel::snapshot_t shape;
        if (cursor_channel::copy_shape_if_newer(_capture_shape_revision, shape)) {
          if (auto image = host_cursor_bitmap::from_color(shape.width, shape.height, std::uint32_t(shape.width) * 4, shape.bgra,
                std::uint32_t((std::max)(shape.hotspot_x, std::int16_t(0))), std::uint32_t((std::max)(shape.hotspot_y, std::int16_t(0))))) {
            _capture_shape_id = shapes().insert(std::move(*image));
          }
        }
        return _capture_shape_id;
      }

      void
      bind_desktop(clock::time_point now, bool force) {
        if (!force && _last_bind_attempt && now - *_last_bind_attempt < desktop_rebind_interval) {
          return;
        }
        _last_bind_attempt = now;
        _desktop_bound = bind_input_desktop();
      }

      void
      publish(snapshot_t next) {
        std::lock_guard lg { _snapshot_lock };
        next.generation = _snapshot.generation;
        if (next.available != _snapshot.available || next.visible != _snapshot.visible ||
            next.shape_id != _snapshot.shape_id || next.x != _snapshot.x || next.y != _snapshot.y ||
            next.clipped != _snapshot.clipped || !(next.clip == _snapshot.clip) || next.warp_serial != _snapshot.warp_serial) {
          next.generation++;
          _snapshot = next;
        }
      }

      void
      run(std::stop_token stop) {
        // Physical coordinates, and the display's real DPI from GetDpiForMonitor().
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        _capture_shape_id.reset();
        _last_bind_attempt.reset();
        bind_desktop(clock::now(), true);

        HCURSOR last_cursor = nullptr;
        std::optional<std::uint32_t> gdi_shape_id;
        clock::time_point last_gdi_read {};
        std::optional<point_t> last_position;
        std::uint64_t warp_serial = 0;
        bool was_available = true;

        std::chrono::nanoseconds cost_total {};
        std::chrono::nanoseconds cost_max {};
        std::uint64_t polls = 0;
        auto cost_window_start = clock::now();

        auto next_poll = clock::now();
        while (!stop.stop_requested()) {
          const auto now = clock::now();

          bind_desktop(now, false);
          CURSORINFO cursor_info { sizeof(cursor_info) };
          bool available = _desktop_bound && GetCursorInfo(&cursor_info);

          snapshot_t next;
          if (available) {
            const auto vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const auto vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const auto vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const auto vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            next.available = true;
            // CURSOR_SUPPRESSED only means touch or pen input is active.
            next.visible = (cursor_info.flags & (CURSOR_SHOWING | CURSOR_SUPPRESSED)) && cursor_info.hCursor;
            next.x = cursor_info.ptScreenPos.x - vx;
            next.y = cursor_info.ptScreenPos.y - vy;

            RECT clip {};
            if (GetClipCursor(&clip) && !(clip.left <= vx && clip.top <= vy && clip.right >= vx + vw && clip.bottom >= vy + vh)) {
              next.clipped = true;
              next.clip = { clip.left - vx, clip.top - vy, clip.right - vx, clip.bottom - vy };
            }

            if (auto capture_id = capture_shape_id()) {
              next.shape_id = *capture_id;
            }
            else {
              if (next.visible && (cursor_info.hCursor != last_cursor || now - last_gdi_read >= shape_revalidate_interval)) {
                last_cursor = cursor_info.hCursor;
                last_gdi_read = now;
                auto image = convert_hcursor(cursor_info.hCursor, cursor_info.ptScreenPos);
                gdi_shape_id = image ? std::optional { shapes().insert(std::move(*image)) } : std::nullopt;
              }
              next.shape_id = gdi_shape_id.value_or(0);
            }

            const point_t position { next.x, next.y };
            if (last_position && *last_position != position && !injections.explains(position.x, position.y, now)) {
              ++warp_serial;
            }
            last_position = position;
            next.warp_serial = warp_serial;
          }
          if (available != was_available) {
            BOOST_LOG(info) << "Cursor protocol v2: cursor information "sv << (available ? "available"sv : "unavailable, falling back to host compositing"sv);
            was_available = available;
          }
          publish(next);

          const auto cost = clock::now() - now;
          cost_total += cost;
          cost_max = (std::max)(cost_max, std::chrono::duration_cast<std::chrono::nanoseconds>(cost));
          ++polls;
          if (now - cost_window_start >= cost_log_interval) {
            BOOST_LOG(debug) << "Cursor protocol v2: "sv << polls << " polls, average "sv
                             << std::chrono::duration_cast<std::chrono::microseconds>(cost_total / (std::max)(polls, std::uint64_t { 1 })).count()
                             << " us, max "sv << std::chrono::duration_cast<std::chrono::microseconds>(cost_max).count() << " us"sv;
            cost_total = cost_max = {};
            polls = 0;
            cost_window_start = now;
          }

          next_poll += poll_interval;
          if (next_poll < now) next_poll = now + poll_interval;
          std::this_thread::sleep_until(next_poll);
        }
      }

      std::mutex _users_lock;
      std::size_t _users = 0;
      std::atomic_bool _running { false };
      std::jthread _thread;

      std::mutex _snapshot_lock;
      snapshot_t _snapshot;

      std::uint64_t _capture_shape_revision = 0;  // tracker thread only
      std::optional<std::uint32_t> _capture_shape_id;  // tracker thread only

      std::optional<clock::time_point> _last_bind_attempt;  // tracker thread only
      bool _desktop_bound = false;  // tracker thread only
    };

    tracker_t &
    instance() {
      static tracker_t tracker;
      return tracker;
    }
  }  // namespace

  bool
  supported() {
    return true;
  }

  std::shared_ptr<void>
  acquire() {
    return instance().acquire();
  }

  snapshot_t
  latest() {
    return instance().latest();
  }

  void
  note_absolute_injection(std::int32_t x, std::int32_t y) {
    auto &tracker = instance();
    if (tracker.running()) tracker.injections.note_absolute(x, y, clock::now());
  }
}  // namespace host_cursor::tracker
