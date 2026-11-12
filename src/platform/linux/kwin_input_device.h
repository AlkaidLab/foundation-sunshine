/**
 * @file src/platform/linux/kwin_input_device.h
 * @brief Point KWin's per-device knobs of our synthetic touchscreen at the streamed output.
 *
 * KWin decides where a touch device's 0..1 coordinate range lands (see upstream
 * `Connection::applyScreenToDevice()` and the `LIBINPUT_EVENT_TOUCH_DOWN` branch of
 * `Connection::processEvents()`):
 *
 *   - every touch device gets ONE output; if that output is null the event is dropped
 *     with "Touch down received for device with no output assigned";
 *   - without a device name/uuid it is guessed from the physical size, then falls back
 *     to the *internal* panel, then to the first output — so a virtual touchscreen on a
 *     laptop gets the (possibly disabled) internal panel instead of the display we stream;
 *   - `mapToWorkspace` then selects the coordinate space: false confines the device to
 *     that single output, true spans the whole workspace.
 *
 * Windows has neither problem (absolute touch input spans the virtual desktop), so for
 * parity we ask KWin for `mapToWorkspace` and name the output explicitly. Both are
 * best-effort: no KWin, no session bus, or a refused property all leave the device as it
 * was and never fail a session.
 */
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platf::kwin_input {
  /**
   * @brief Name inputtino (and the legacy backend) give the synthetic touchscreen.
   */
  inline constexpr std::string_view kTouchDeviceName { "Touch passthrough" };

  /**
   * @brief The KWin knobs we need on our touch device.
   */
  struct mapping_t {
    /// Output to pin the device to. Empty means "leave KWin's guess alone".
    std::string output_name;
    /// Span the whole workspace instead of a single output (Windows parity).
    bool map_to_workspace = true;
  };

  /**
   * @brief Decide the mapping to request for our touch device.
   * @param enabled_outputs Connector names KWin currently drives, in /sys order.
   * @param streamed_output The connector Sunshine is streaming, if it is known.
   * @return A mapping; `output_name` is empty when no output is enabled at all.
   */
  mapping_t
  plan_mapping(const std::vector<std::string> &enabled_outputs, const std::string &streamed_output);

  /**
   * @brief Pick the `/sys` event node names whose device name matches.
   * @param nodes Pairs of (event node name, device name), as found under /sys/class/input.
   * @param device_name Name to look for.
   */
  std::vector<std::string>
  collect_matching_nodes(const std::vector<std::pair<std::string, std::string>> &nodes, std::string_view device_name);

  /**
   * @brief Connectors that are connected and currently driven, e.g. {"DP-2"}.
   */
  std::vector<std::string>
  enabled_outputs();

  /**
   * @brief Event node names (e.g. "event30") of the device carrying @p device_name.
   */
  std::vector<std::string>
  find_event_nodes(std::string_view device_name);

  /**
   * @brief Whether KWin owns its name on the session bus.
   */
  bool
  available();

  /**
   * @brief Apply @p mapping to one of our event nodes.
   * @return True when every requested property was accepted.
   */
  bool
  apply(const std::string &event_node, const mapping_t &mapping, std::string *error = nullptr);

  /**
   * @brief Apply the planned mapping to every synthetic touchscreen we created.
   * @details Called right after the device is created and again after every display
   *          configuration change, because the set of enabled outputs changes with it
   *          (a virtual display appears and the physical one may go away).
   * @param streamed_output The connector being streamed, when known.
   */
  void
  sync_our_touch_devices(const std::string &streamed_output = {});
}  // namespace platf::kwin_input
