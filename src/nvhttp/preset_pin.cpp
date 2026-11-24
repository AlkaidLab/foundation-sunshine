/**
 * @file src/nvhttp/preset_pin.cpp
 * @brief QR preset PIN state shared by the pairing and QR routes.
 */
#include "src/nvhttp.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>

#include "src/logging.h"

using namespace std::literals;

namespace nvhttp {
  namespace {
    struct preset_pin_state_t {
      std::string pin;
      std::string name;
      std::chrono::steady_clock::time_point expires_at;
      bool paired = false;
      std::uint64_t generation = 0;
      std::mutex mutex;
    };

    preset_pin_state_t preset_pin_state;
  }  // namespace

  bool
  set_preset_pin(const std::string &pin, const std::string &name, int timeout_seconds) {
    return set_preset_pin_with_generation(pin, name, timeout_seconds).has_value();
  }

  std::optional<std::uint64_t>
  set_preset_pin_with_generation(const std::string &pin, const std::string &name, int timeout_seconds) {
    if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      BOOST_LOG(warning) << "Invalid preset PIN: must be 4 digits";
      return std::nullopt;
    }

    std::lock_guard lock { preset_pin_state.mutex };
    if (preset_pin_state.generation == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    ++preset_pin_state.generation;
    preset_pin_state.pin = pin;
    preset_pin_state.name = name;
    preset_pin_state.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    preset_pin_state.paired = false;
    BOOST_LOG(info) << "Preset PIN set for QR pairing, expires in " << timeout_seconds << "s";
    return preset_pin_state.generation;
  }

  std::string
  consume_preset_pin() {
    std::lock_guard lock { preset_pin_state.mutex };
    if (preset_pin_state.pin.empty()) {
      return std::string {};
    }
    if (std::chrono::steady_clock::now() > preset_pin_state.expires_at) {
      preset_pin_state.pin.clear();
      preset_pin_state.name.clear();
      return std::string {};
    }
    std::string pin = std::move(preset_pin_state.pin);
    preset_pin_state.pin.clear();
    preset_pin_state.name.clear();
    return pin;
  }

  void
  clear_preset_pin() {
    std::lock_guard lock { preset_pin_state.mutex };
    preset_pin_state.pin.clear();
    preset_pin_state.name.clear();
    preset_pin_state.paired = true;
  }

  bool
  clear_preset_pin_if_generation(std::uint64_t generation) {
    std::lock_guard lock { preset_pin_state.mutex };
    if (generation == 0 || generation != preset_pin_state.generation) return false;
    preset_pin_state.pin.clear();
    preset_pin_state.name.clear();
    preset_pin_state.paired = true;
    return true;
  }

  std::string
  get_qr_pair_status() {
    std::lock_guard lock { preset_pin_state.mutex };
    if (preset_pin_state.paired) return "paired";
    if (preset_pin_state.pin.empty()) return "inactive";
    if (std::chrono::steady_clock::now() > preset_pin_state.expires_at) {
      preset_pin_state.pin.clear();
      preset_pin_state.name.clear();
      return "expired";
    }
    return "active";
  }
}  // namespace nvhttp
