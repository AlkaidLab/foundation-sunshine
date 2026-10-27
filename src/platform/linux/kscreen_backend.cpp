/**
 * @file src/platform/linux/kscreen_backend.cpp
 * @brief KScreen availability gate and bounded kscreen-doctor runner.
 */
#include "kscreen_backend.h"

#include <atomic>
#include <cstdlib>
#include <regex>

#include "src/display_device/vdd_utils.h"
#include "src/logging.h"

namespace platf::kscreen {
  namespace {
    std::atomic<std::chrono::steady_clock::rep> probe_after { 0 };
    std::atomic<bool> unavailability_logged { false };

    std::chrono::steady_clock::time_point
    now() {
      return std::chrono::steady_clock::now();
    }
  }  // namespace

  bool
  session_supports_kscreen() {
    // niri exports NIRI_SOCKET to its children (and, through the user manager,
    // to services started from that session). Prefer it over the desktop
    // string, which some setups fake to "KDE" for toolkit compatibility.
    if (const char *socket = ::getenv("NIRI_SOCKET"); socket && *socket) {
      return false;
    }

    const char *desktop = ::getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !*desktop) {
      // A service started from SSH or at boot has no desktop yet; there is
      // nothing KScreen could answer for right now.
      return false;
    }

    const std::string value { desktop };
    return value.find("KDE") != std::string::npos ||
           value.find("Plasma") != std::string::npos ||
           value.find("plasma") != std::string::npos;
  }

  bool
  probe_allowed() {
    return now().time_since_epoch().count() >= probe_after.load(std::memory_order_relaxed);
  }

  void
  note_unavailable(const std::string &reason) {
    probe_after.store((now() + kRetryCooldown).time_since_epoch().count(), std::memory_order_relaxed);
    if (!unavailability_logged.exchange(true)) {
      BOOST_LOG(info) << "Compositor display management unavailable (" << reason
                      << "); the client's display settings are applied again once it responds";
    }
  }

  void
  note_available() {
    probe_after.store(0, std::memory_order_relaxed);
  }

  exec_result_t
  run(const std::string &args, std::chrono::milliseconds timeout) {
    // vdd_utils::run_logged bounds the child (kscreen-doctor can block
    // indefinitely on a wedged or missing compositor) and keeps inherited
    // sockets out of it.
    auto result = display_device::vdd_utils::run_logged("kscreen-doctor " + args, timeout);

    // kscreen-doctor colourises its output; callers parse plain text.
    static const std::regex ansi_re { "\x1b\\[[0-9;]*[A-Za-z]" };
    return { result.exit_code, std::regex_replace(result.output, ansi_re, "") };
  }
}  // namespace platf::kscreen
