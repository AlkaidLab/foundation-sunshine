/**
 * @file src/audio_devices.h
 * @brief The audio endpoints of the host as the Web UI and the Panel list them.
 *
 * Served by GET /api/audio/devices. Windows reads the render and capture endpoints through the
 * MMDevice API (IMMDeviceEnumerator, see src/platform/windows/audio_devices.cpp); this file holds
 * everything that does not need Windows: how an endpoint is recognised as a known virtual device
 * (VB-Audio Virtual Cable, Steam Streaming Speakers/Microphone, our virtual microphone), how the
 * state of VB-CABLE is summarised, and the JSON the route sends.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace audio_devices {
  /// Version of the JSON contract served by GET /api/audio/devices.
  constexpr int JSON_VERSION = 1;

  /// Direction of an endpoint, as IMMEndpoint::GetDataFlow reports it.
  enum class flow_e {
    render,  ///< playback: eRender
    capture,  ///< recording: eCapture
  };

  /// One audio endpoint as the host reads it (all strings UTF-8).
  struct endpoint_t {
    flow_e flow = flow_e::render;
    std::string id;  ///< IMMDevice::GetId, e.g. {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
    std::string name;  ///< PKEY_Device_FriendlyName, e.g. "CABLE Input (VB-Audio Virtual Cable)"
    std::string adapter;  ///< PKEY_DeviceInterface_FriendlyName, e.g. "VB-Audio Virtual Cable"
    std::string description;  ///< PKEY_Device_DeviceDesc, e.g. "CABLE Input"
    std::string state;  ///< active, disabled, unplugged, not_present (a remembered endpoint whose device is gone or disabled)
    std::string form_factor;  ///< EndpointFormFactor as a word, see form_factor_name
    bool is_default = false;  ///< the default endpoint of its direction for the console role
    bool is_default_communications = false;  ///< the default endpoint for the communications role
  };

  /// Known virtual devices, by the word the JSON carries.
  namespace virtual_kind {
    constexpr std::string_view vb_cable = "vb_cable";  ///< the VB-Audio Virtual Cable the host's microphone backend writes to
    constexpr std::string_view vb_audio = "vb_audio";  ///< another VB-Audio product (Cable A/B, Voicemeeter): not what the microphone backend uses
    constexpr std::string_view steam_streaming = "steam_streaming";  ///< Steam Streaming Speakers / Microphone
    constexpr std::string_view virtual_microphone = "virtual_microphone";  ///< the host's own USB/IP microphone
  }  // namespace virtual_kind

  /**
   * @brief The word for an EndpointFormFactor value (mmdeviceapi.h).
   * @return speakers, line_level, headphones, microphone, headset, handset, digital_passthrough, spdif,
   *         digital_display, remote_network, or "unknown"
   */
  std::string_view
  form_factor_name(int form_factor);

  /// The word for a DEVICE_STATE_* value: active, disabled, unplugged, not_present; "" for a value that is none of them.
  std::string_view
  state_name(unsigned long device_state);

  /// Which known virtual device an endpoint is, or "" for a physical or unknown one.
  std::string_view
  classify_virtual(const endpoint_t &endpoint);

  /**
   * @brief Whether the endpoint is the VB-CABLE the microphone backend uses.
   *
   * The backend matches the adapter name "VB-Audio Virtual Cable" (src/platform/windows/mic_write.cpp);
   * the same test is used here so that "installed" means "the backend finds it".
   */
  bool
  is_vb_cable(const endpoint_t &endpoint);

  /// A device of the Windows device tree (the driver side of an audio endpoint): present means it is in the tree, even when it is disabled.
  struct driver_t {
    std::string name;  ///< the friendly name or description of the device node, e.g. "VB-Audio Virtual Cable"
    long problem = 0;  ///< the CM_PROB_* code of the device node, 0 when it has none, -1 when it could not be read
    bool disabled = false;  ///< the device is disabled in the Device Manager (CM_PROB_DISABLED, code 22)
    std::string instance_id;  ///< the device instance id (e.g. ROOT\\MEDIA\\0000), what pnputil takes
  };

  /// Whether a device node is the VB-Audio Virtual Cable (the name the microphone backend matches).
  bool
  is_vb_cable_driver(const driver_t &driver);

  /// What the host knows about VB-CABLE from the endpoint list and, when it could read it, the device tree.
  struct vb_cable_status_t {
    bool installed = false;  ///< the driver's device is in the device tree, or a VB-CABLE endpoint exists that is active, disabled or unplugged
    bool driver_present = false;  ///< the device node of the driver was found (only when the device tree was read)
    bool driver_disabled = false;  ///< ... and it is disabled in the Device Manager
    /// ready (render and capture active), partial (one side usable), disabled (the driver is installed but its device is
    /// disabled in the Device Manager), unavailable (known, but nothing usable for another reason: an endpoint disabled or
    /// unplugged, a driver problem; or, when the device tree was not read, endpoints Windows only remembers), missing (not installed)
    std::string state = "missing";
    std::string render_id;  ///< "CABLE Input": the host writes the client's microphone here
    std::string capture_id;  ///< "CABLE Output": applications record the microphone from here
    std::string render_name;
    std::string capture_name;
  };

  /// `drivers`: the device nodes read from the device tree, or nullptr when they could not be read (then the endpoints decide alone).
  vb_cable_status_t
  summarize_vb_cable(const std::vector<endpoint_t> &endpoints, const std::vector<driver_t> *drivers = nullptr);

  /// The JSON of GET /api/audio/devices. Render endpoints and capture endpoints are two lists, the default one first;
  /// remembered (not present) endpoints are not listed, they only count for the VB-CABLE state.
  nlohmann::json
  to_json(const std::vector<endpoint_t> &endpoints, const std::vector<driver_t> *drivers = nullptr);

  /// The JSON for a platform that cannot list audio endpoints (Linux, macOS).
  nlohmann::json
  unsupported_json();

#ifdef _WIN32
  /**
   * @brief Read the render and capture endpoints (in every state) through MMDevice.
   * @return false when the enumerator cannot be created; the list is then empty
   */
  bool
  enumerate_endpoints(std::vector<endpoint_t> &out);

  /**
   * @brief Read the VB-Audio Virtual Cable device nodes of the sound class (SetupAPI, CM_Get_DevNode_Status). Read only.
   * @return false when the device list cannot be read; `out` is then empty and means nothing
   */
  bool
  enumerate_vb_cable_drivers(std::vector<driver_t> &out);
#endif
}  // namespace audio_devices
