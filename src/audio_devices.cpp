/**
 * @file src/audio_devices.cpp
 * @brief Recognising virtual devices and shaping the endpoint list (no Windows API in this file).
 */
#include "audio_devices.h"

#include <algorithm>
#include <cctype>

namespace audio_devices {
  namespace {
    std::string
    lower(std::string_view text) {
      std::string out(text);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    bool
    contains(std::string_view haystack, std::string_view needle) {
      return lower(haystack).find(lower(needle)) != std::string::npos;
    }

    bool
    equals(std::string_view a, std::string_view b) {
      return lower(a) == lower(b);
    }

    /// The adapter name, or the part of the friendly name in parentheses when the adapter is not known ("CABLE Input (VB-Audio Virtual Cable)").
    std::string
    adapter_of(const endpoint_t &endpoint) {
      if (!endpoint.adapter.empty()) return endpoint.adapter;
      const auto open = endpoint.name.rfind('(');
      const auto close = endpoint.name.rfind(')');
      if (open != std::string::npos && close != std::string::npos && close > open) {
        return endpoint.name.substr(open + 1, close - open - 1);
      }
      return {};
    }

    bool
    is_active(const endpoint_t &endpoint) {
      return endpoint.state == "active";
    }

    /// Default endpoint first, then the usable ones, then by name: the order a picker wants.
    bool
    list_before(const endpoint_t &a, const endpoint_t &b) {
      if (a.is_default != b.is_default) return a.is_default;
      if (is_active(a) != is_active(b)) return is_active(a);
      const auto an = lower(a.name);
      const auto bn = lower(b.name);
      if (an != bn) return an < bn;
      return a.id < b.id;
    }

    nlohmann::json
    endpoint_json(const endpoint_t &endpoint) {
      const auto kind = classify_virtual(endpoint);
      return {
        { "id", endpoint.id },
        { "name", endpoint.name },
        { "adapter", endpoint.adapter },
        { "description", endpoint.description },
        { "state", endpoint.state },
        { "form_factor", endpoint.form_factor },
        { "default", endpoint.is_default },
        { "default_communications", endpoint.is_default_communications },
        { "virtual", kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(kind)) },
      };
    }
  }  // namespace

  std::string_view
  form_factor_name(int form_factor) {
    // EndpointFormFactor, mmdeviceapi.h: the values count up from RemoteNetworkDevice = 0.
    switch (form_factor) {
      case 0:
        return "remote_network";
      case 1:
        return "speakers";
      case 2:
        return "line_level";
      case 3:
        return "headphones";
      case 4:
        return "microphone";
      case 5:
        return "headset";
      case 6:
        return "handset";
      case 7:
        return "digital_passthrough";
      case 8:
        return "spdif";
      case 9:
        return "digital_display";
      default:
        return "unknown";
    }
  }

  std::string_view
  state_name(unsigned long device_state) {
    // DEVICE_STATE_ACTIVE 0x1, DISABLED 0x2, NOTPRESENT 0x4, UNPLUGGED 0x8.
    switch (device_state) {
      case 0x1:
        return "active";
      case 0x2:
        return "disabled";
      case 0x4:
        return "not_present";
      case 0x8:
        return "unplugged";
      default:
        return "";
    }
  }

  bool
  is_vb_cable(const endpoint_t &endpoint) {
    return equals(adapter_of(endpoint), "VB-Audio Virtual Cable");
  }

  std::string_view
  classify_virtual(const endpoint_t &endpoint) {
    if (is_vb_cable(endpoint)) return virtual_kind::vb_cable;
    const auto adapter = adapter_of(endpoint);
    if (contains(adapter, "vb-audio") || contains(endpoint.name, "vb-audio")) return virtual_kind::vb_audio;
    if (contains(adapter, "Steam Streaming") || contains(endpoint.name, "Steam Streaming")) return virtual_kind::steam_streaming;
    if (contains(endpoint.name, "Sunshine Virtual Microphone") || contains(adapter, "Sunshine Virtual Microphone")) return virtual_kind::virtual_microphone;
    return {};
  }

  bool
  is_vb_cable_driver(const driver_t &driver) {
    return equals(driver.name, "VB-Audio Virtual Cable");
  }

  vb_cable_status_t
  summarize_vb_cable(const std::vector<endpoint_t> &endpoints, const std::vector<driver_t> *drivers) {
    vb_cable_status_t status;
    const driver_t *driver = nullptr;
    if (drivers) {
      for (const auto &candidate : *drivers) {
        if (is_vb_cable_driver(candidate)) {
          driver = &candidate;
          break;
        }
      }
    }
    if (driver) {
      status.driver_present = true;
      status.driver_disabled = driver->disabled;
      status.installed = true;
    }
    const endpoint_t *render = nullptr;
    const endpoint_t *capture = nullptr;
    bool known = false;
    for (const auto &endpoint : endpoints) {
      if (!is_vb_cable(endpoint)) continue;
      known = true;
      if (endpoint.state != "not_present") status.installed = true;
      const endpoint_t *&slot = endpoint.flow == flow_e::render ? render : capture;
      // keep the usable endpoint when a side is listed twice
      if (!slot || (!is_active(*slot) && is_active(endpoint))) slot = &endpoint;
    }
    if (render) {
      status.render_id = render->id;
      status.render_name = render->name;
    }
    if (capture) {
      status.capture_id = capture->id;
      status.capture_name = capture->name;
    }
    if (!known && !driver) {
      status.state = "missing";
    }
    else if (render && capture && is_active(*render) && is_active(*capture)) {
      status.state = "ready";
    }
    else if ((render && is_active(*render)) || (capture && is_active(*capture))) {
      status.state = "partial";
    }
    else if (driver && driver->disabled) {
      status.state = "disabled";
    }
    else if (!driver && !status.installed && drivers) {
      // Windows remembers the endpoints of a removed device; with the device tree read and no driver in it, it is not installed
      status.state = "missing";
    }
    else {
      status.state = "unavailable";
    }
    return status;
  }

  nlohmann::json
  to_json(const std::vector<endpoint_t> &endpoints, const std::vector<driver_t> *drivers) {
    std::vector<endpoint_t> render;
    std::vector<endpoint_t> capture;
    for (const auto &endpoint : endpoints) {
      if (endpoint.state == "not_present") continue;
      (endpoint.flow == flow_e::render ? render : capture).push_back(endpoint);
    }
    std::stable_sort(render.begin(), render.end(), list_before);
    std::stable_sort(capture.begin(), capture.end(), list_before);

    auto render_json = nlohmann::json::array();
    for (const auto &endpoint : render) render_json.push_back(endpoint_json(endpoint));
    auto capture_json = nlohmann::json::array();
    for (const auto &endpoint : capture) capture_json.push_back(endpoint_json(endpoint));

    const auto cable = summarize_vb_cable(endpoints, drivers);
    return {
      { "success", true },
      { "version", JSON_VERSION },
      { "supported", true },
      { "render", render_json },
      { "capture", capture_json },
      { "vb_cable", {
                      { "installed", cable.installed },
                      { "driver_present", cable.driver_present },
                      { "driver_disabled", cable.driver_disabled },
                      { "state", cable.state },
                      { "render_id", cable.render_id },
                      { "render_name", cable.render_name },
                      { "capture_id", cable.capture_id },
                      { "capture_name", cable.capture_name },
                    } },
    };
  }

  nlohmann::json
  unsupported_json() {
    return {
      { "success", true },
      { "version", JSON_VERSION },
      { "supported", false },
      { "render", nlohmann::json::array() },
      { "capture", nlohmann::json::array() },
      { "vb_cable", {
                      { "installed", false },
                      { "driver_present", false },
                      { "driver_disabled", false },
                      { "state", "unsupported" },
                      { "render_id", "" },
                      { "render_name", "" },
                      { "capture_id", "" },
                      { "capture_name", "" },
                    } },
    };
  }
}  // namespace audio_devices
