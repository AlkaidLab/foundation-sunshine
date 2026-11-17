#pragma once

#include "src/sparxie/art.h"
#include <atomic>
#include <mutex>
#include <string>

namespace sparxie {
  // Remember a proven WebP component initialization failure for this process.
  // A corrupt input or another WIC failure must not disable the component.
  class WebpDecodePolicy {
  public:
    template<class WicDecode, class WebpDecode>
    Rgba decode(bool is_webp, std::string *diagnostic, WicDecode &&wic, WebpDecode &&webp) {
      std::unique_lock<std::mutex> probe_lock(probe_mutex_, std::defer_lock);
      if (is_webp && state_.load() == State::unknown) {
        probe_lock.lock();
      }
      if (is_webp && state_.load() == State::unavailable) {
        Rgba image = webp();
        if (diagnostic) {
          diagnostic->clear();
          if (image.w == 0 || image.h == 0 || image.px.empty()) {
            *diagnostic = "libwebp decode failed (WIC WebP decoder unavailable)";
          }
        }
        return image;
      }

      bool component_init_failed = false;
      Rgba image = wic(diagnostic, &component_init_failed);
      if (image.w > 0 && image.h > 0 && !image.px.empty()) {
        if (is_webp && state_.load() == State::unknown) {
          state_.store(State::available);
        }
        return image;
      }
      if (!is_webp) {
        return image;
      }

      image = webp();
      if (image.w > 0 && image.h > 0 && !image.px.empty()) {
        bool first_unavailable = true;
        if (component_init_failed) {
          first_unavailable = state_.exchange(State::unavailable) != State::unavailable;
        }
        if (diagnostic) {
          if (first_unavailable) {
            *diagnostic += "; libwebp fallback succeeded";
          }
          else {
            diagnostic->clear();
          }
        }
        return image;
      }
      if (diagnostic) {
        *diagnostic += "; libwebp decode failed";
      }
      return image;
    }

  private:
    enum class State { unknown, available, unavailable };
    std::atomic<State> state_ { State::unknown };
    std::mutex probe_mutex_;
  };
}  // namespace sparxie
