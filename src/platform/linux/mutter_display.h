#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <systemd/sd-bus.h>

/**
 * @file src/platform/linux/mutter_display.h
 * @brief GNOME's display configuration API (`org.gnome.Mutter.DisplayConfig`).
 *
 * GNOME implements neither KScreen nor `wlr-output-management`: on Wayland the
 * only way to read or change monitor layout is Mutter's own D-Bus interface.
 * Its model differs from both backends we already speak to, and the difference
 * is worth stating because it shapes this header:
 *
 *  - A *monitor* is a physical connector with a list of modes, and each mode
 *    carries a real id string that `ApplyMonitorsConfig` takes back verbatim.
 *  - A *logical monitor* is a rectangle on the desktop that shows one or more
 *    monitors. Listing several monitors in one logical monitor is exactly the
 *    "replication" that kscreen-doctor cannot express.
 *  - Writes replace the whole configuration, atomically, in one call: there is
 *    no per-output verb. Callers therefore query, mutate the returned state,
 *    and apply it.
 *  - An output is disabled by leaving it out of every logical monitor, not by
 *    calling an "off" verb.
 *  - Primary is a flag on a logical monitor, so it can be set here (kscreen can
 *    too, niri cannot).
 *  - HDR is the monitor's `color-mode`: 1 is BT.2100 (HDR), 0 is the default.
 */
namespace platf::mutter {

  constexpr const char *kService = "org.gnome.Mutter.DisplayConfig";
  constexpr const char *kPath = "/org/gnome/Mutter/DisplayConfig";
  constexpr const char *kInterface = "org.gnome.Mutter.DisplayConfig";

  /// Query budget. GetCurrentState answers from memory, so it is fast.
  constexpr auto kProbeTimeout = std::chrono::milliseconds { 1500 };
  /// A write blocks until Mutter finishes the modeset; give it room.
  constexpr auto kChangeTimeout = std::chrono::milliseconds { 10'000 };
  /// After a failure, stay quiet for a while instead of re-probing per query.
  constexpr auto kRetryCooldown = std::chrono::milliseconds { 20'000 };

  /// `color-mode` values from the interface documentation.
  constexpr unsigned int kColorModeDefault = 0;
  constexpr unsigned int kColorModeBt2100 = 1;  ///< HDR
  constexpr unsigned int kColorModeSdrNative = 2;

  /// `ApplyMonitorsConfig` method field.
  constexpr unsigned int kMethodVerify = 0;
  constexpr unsigned int kMethodTemporary = 1;
  constexpr unsigned int kMethodPersistent = 2;

  struct mode_t {
    std::string id;
    int width = 0;
    int height = 0;
    double refresh = 0.0;
    bool current = false;
    bool preferred = false;
  };

  struct monitor_t {
    std::string connector;
    std::string vendor;
    std::string product;
    std::string serial;
    std::vector<mode_t> modes;
    std::string display_name;
    unsigned int color_mode = kColorModeDefault;
    std::vector<unsigned int> supported_color_modes;
  };

  struct logical_monitor_t {
    int x = 0;
    int y = 0;
    double scale = 1.0;
    unsigned int transform = 0;
    bool primary = false;
    std::vector<std::string> connectors;
  };

  struct state_t {
    unsigned int serial = 0;
    std::vector<monitor_t> monitors;
    std::vector<logical_monitor_t> logical_monitors;
  };

  /**
   * @brief Whether this session can be driven through Mutter's interface.
   * @details Checks that the well-known name is already owned — never that it
   *          can be *activated*, because a D-Bus activation attempt outside
   *          GNOME would spawn nothing useful and burn the probe timeout.
   */
  bool
  session_supports_mutter();

  /// False while a recent failure is still cooling down.
  bool
  probe_allowed();

  /// Record that the interface answered; clears the cooldown.
  void
  note_available();

  /// Record why the interface is unusable; starts the cooldown.
  void
  note_unavailable(const std::string &reason);

  /**
   * @brief Read the current configuration.
   * @return The state, or nothing when the interface is unreachable or the
   *         answer cannot be understood.
   */
  std::optional<state_t>
  query_state();

  /**
   * @brief Replace the configuration with @p state.
   * @details Persistent, like the settings Sunshine restores later. The caller
   *          is expected to re-read the state to decide whether it took effect:
   *          Mutter can accept the call and still lay things out differently
   *          (a scale or mode it considers unsupported, for instance).
   */
  bool
  apply(const state_t &state);

  /// Monitor entry for @p connector, or nullptr.
  const monitor_t *
  find_monitor(const state_t &state, const std::string &connector);

  /// Monitor entry for @p connector, mutable, or nullptr.
  monitor_t *
  find_monitor(state_t &state, const std::string &connector);

  /// Logical monitor showing @p connector, or nullptr when it is disabled.
  const logical_monitor_t *
  find_logical_monitor(const state_t &state, const std::string &connector);

  /// Logical monitor that shows @p connector, mutable, or nullptr.
  logical_monitor_t *
  find_logical_monitor(state_t &state, const std::string &connector);

  /// The mode @p monitor currently runs, or nullptr.
  const mode_t *
  current_mode(const monitor_t &monitor);

  /// The mode of @p monitor matching the given size and rate, or nullptr.
  const mode_t *
  matching_mode(const monitor_t &monitor, int width, int height, double refresh);

  // ── exposed for tests ────────────────────────────────────────────────────
  /**
   * @brief Decode a `GetCurrentState` reply.
   * @details Split out from query_state() so a test can round-trip a state it
   *          appended to a message itself, without a compositor.
   */
  bool
  read_state(sd_bus_message *reply, state_t &out);

  /**
   * @brief Append the arguments of `ApplyMonitorsConfig` for @p state.
   * @details Split out for the same reason; the test appends, then reads the
   *          arguments back with sd-bus and compares.
   */
  bool
  append_apply_args(sd_bus_message *message, const state_t &state);

}  // namespace platf::mutter
