/**
 * @file src/platform/windows/display_vram_internal.cpp
 * @brief See display_vram_internal.h.
 */
#include "display_vram_internal.h"

#include <utility>

#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  void
  img_d3d_t::note_borrowed_vdd_frame_returned() {
    auto counter = std::move(borrowed_vdd_inflight_counter);
    if (counter) {
      auto current = counter->load(std::memory_order_relaxed);
      while (current > 0 &&
             !counter->compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {}
    }
  }

  void
  img_d3d_t::mark_borrowed_vdd_consumed() {
    borrowed_vdd_frame = false;
    borrowed_vdd_mutex.reset();
    borrowed_vdd_inflight_counter.reset();
    borrowed_vdd_slot = 0;
    encoder_acquire_key = 0;
    encoder_release_key = 0;
    producer_release_key = 0;
  }

  bool
  img_d3d_t::release_borrowed_vdd_after_convert(IDXGIKeyedMutex *encoder_mutex) {
    if (!borrowed_vdd_frame) {
      return true;
    }
    if (!encoder_mutex) {
      BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                         << borrowed_vdd_slot << ": missing encoder mutex"sv;
      return false;
    }

    HRESULT status = encoder_mutex->ReleaseSync(producer_release_key);
    if (FAILED(status)) {
      BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                         << borrowed_vdd_slot << " after convert [0x"sv
                         << util::hex(status).to_string_view() << ']';
      return false;
    }

    note_borrowed_vdd_frame_returned();
    mark_borrowed_vdd_consumed();
    return true;
  }

  bool
  img_d3d_t::abandon_borrowed_vdd_frame(bool log_busy, DWORD timeout_ms) {
    if (borrowed_vdd_frame && borrowed_vdd_mutex) {
      HRESULT status = borrowed_vdd_mutex->AcquireSync(encoder_acquire_key, timeout_ms);
      if (status == S_OK) {
        status = borrowed_vdd_mutex->ReleaseSync(producer_release_key);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "[vdd] failed to return borrowed slot "sv
                             << borrowed_vdd_slot << " [0x"sv
                             << util::hex(status).to_string_view() << ']';
          return false;
        }
        note_borrowed_vdd_frame_returned();
      }
      else {
        if (log_busy || status != static_cast<HRESULT>(WAIT_TIMEOUT)) {
          BOOST_LOG(warning) << "[vdd] failed to acquire borrowed slot "sv
                             << borrowed_vdd_slot << " for return [0x"sv
                             << util::hex(status).to_string_view() << ']';
        }
        return false;
      }
    }
    mark_borrowed_vdd_consumed();
    return true;
  }

  img_d3d_t::~img_d3d_t() {
    abandon_borrowed_vdd_frame(true, 16);
    if (encoder_texture_handle) {
      CloseHandle(encoder_texture_handle);
    }
  }

  texture_lock_helper::texture_lock_helper(texture_lock_helper &&other) {
    mutex.reset(other.mutex.release());
    locked = other.locked;
    other.locked = false;
  }

  texture_lock_helper &
  texture_lock_helper::operator=(texture_lock_helper &&other) {
    if (this == &other) {
      return *this;
    }
    if (locked && mutex) {
      mutex->ReleaseSync(0);
    }
    mutex.reset(other.mutex.release());
    locked = other.locked;
    other.locked = false;
    return *this;
  }

  texture_lock_helper::texture_lock_helper(IDXGIKeyedMutex *mutex):
      mutex(mutex) {
    if (this->mutex) {
      this->mutex->AddRef();
    }
  }

  texture_lock_helper::~texture_lock_helper() {
    if (locked && mutex) {
      mutex->ReleaseSync(0);
    }
  }

  bool
  texture_lock_helper::lock() {
    if (locked) {
      return true;
    }
    if (!mutex) {
      BOOST_LOG(error) << "Failed to acquire texture mutex: missing IDXGIKeyedMutex"sv;
      return false;
    }

    const HRESULT status = mutex->AcquireSync(0, INFINITE);
    if (status == S_OK) {
      locked = true;
    }
    else {
      BOOST_LOG(error) << "Failed to acquire texture mutex [0x"sv
                       << util::hex(status).to_string_view() << ']';
    }
    return locked;
  }
}  // namespace platf::dxgi
