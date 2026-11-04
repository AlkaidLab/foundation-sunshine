/**
 * @file src/system_info.cpp
 * @brief Inventory helpers, JSON rendering and the short-lived cache for
 *        GET /api/system/info.
 */
#include "system_info.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <map>

#include <nlohmann/json.hpp>

namespace system_info {
  namespace {
    std::string
    lower(std::string_view value) {
      std::string result { value };
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return result;
    }

    std::string
    trim(std::string_view value) {
      std::size_t first = 0;
      std::size_t last = value.size();
      while (first < last && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
      while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
      return std::string { value.substr(first, last - first) };
    }

    bool
    starts_with_any(const std::string &value, std::initializer_list<std::string_view> prefixes) {
      for (const auto prefix : prefixes) {
        if (value.rfind(prefix, 0) == 0) return true;
      }
      return false;
    }

    std::string
    unquote(std::string_view value) {
      auto text = trim(value);
      if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
      }
      return text;
    }

    template <typename T>
    nlohmann::json
    optional_json(const std::optional<T> &value) {
      return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    }
  }  // namespace

  nic_type_e
  classify_nic_type(std::string_view name, std::string_view description, bool wireless, bool mesh, bool tunnel) {
    const auto label = lower(name) + ' ' + lower(description);
    if (mesh) return nic_type_e::mesh;
    if (tunnel) return nic_type_e::other;
    if (starts_with_any(lower(name), {
                                       "br",
                                       "docker",
                                       "virbr",
                                       "veth",
                                       "lxc",
                                       "cni",
                                       "flannel",
                                       "kube",
                                       "vmnet",
                                       "vboxnet",
                                       "tap",
                                       "tun",
                                       "utun",
                                       "wg",
                                       "zt",
                                     }) ||
        label.find("virtual") != std::string::npos || label.find("hyper-v") != std::string::npos || label.find("vmware") != std::string::npos || label.find("loopback") != std::string::npos) {
      return nic_type_e::virtual_interface;
    }
    if (wireless) return nic_type_e::wifi;
    return nic_type_e::ethernet;
  }

  std::string
  mask_mac(std::string_view raw) {
    std::vector<unsigned> bytes;
    unsigned value = 0;
    bool have_nibble = false;
    for (const auto character : raw) {
      const auto byte = static_cast<unsigned char>(character);
      if (character == ':' || character == '-' || character == '.') continue;
      int digit = -1;
      if (byte >= '0' && byte <= '9')
        digit = byte - '0';
      else if (byte >= 'a' && byte <= 'f')
        digit = byte - 'a' + 10;
      else if (byte >= 'A' && byte <= 'F')
        digit = byte - 'A' + 10;
      else
        return {};
      value = (value << 4) | static_cast<unsigned>(digit);
      have_nibble = !have_nibble;
      if (!have_nibble) {
        bytes.push_back(value);
        value = 0;
      }
    }
    if (have_nibble || bytes.size() < 3) return {};
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x", bytes[0], bytes[1], bytes[2]);
    return buffer;
  }

  std::string
  mask_ssid(std::string_view ssid) {
    if (ssid.empty()) return {};
    // Keep at most two UTF-8 code points so a multi-byte name is not split.
    std::size_t kept = 0;
    std::size_t position = 0;
    while (position < ssid.size() && kept < 2) {
      const auto byte = static_cast<unsigned char>(ssid[position]);
      std::size_t length = 1;
      if ((byte & 0xe0U) == 0xc0U)
        length = 2;
      else if ((byte & 0xf0U) == 0xe0U)
        length = 3;
      else if ((byte & 0xf8U) == 0xf0U)
        length = 4;
      position = std::min(ssid.size(), position + length);
      ++kept;
    }
    return std::string { ssid.substr(0, position) } + "***";
  }

  cpu_info_t
  parse_linux_cpuinfo(std::string_view text) {
    cpu_info_t result;
    int processors = 0;
    int cores = 0;
    std::string model_fallback;
    std::string vendor_fallback;
    std::size_t position = 0;
    while (position < text.size()) {
      const auto end = text.find('\n', position);
      const auto line = text.substr(position, end == std::string_view::npos ? text.size() - position : end - position);
      position = end == std::string_view::npos ? text.size() : end + 1;
      const auto separator = line.find(':');
      if (separator == std::string_view::npos) continue;
      const auto key = trim(line.substr(0, separator));
      const auto value = trim(line.substr(separator + 1));
      if (key == "processor") {
        ++processors;
      }
      else if (key == "model name" && result.model.empty()) {
        result.model = value;
      }
      else if (key == "vendor_id" && result.vendor.empty()) {
        result.vendor = value;
      }
      else if (key == "cpu cores" && cores == 0) {
        if (const auto parsed = parse_mhz(value)) cores = *parsed;
      }
      else if (key == "siblings" && result.threads == 0) {
        if (const auto parsed = parse_mhz(value)) result.threads = *parsed;
      }
      else if ((key == "Processor" || key == "Hardware" || key == "Model") && model_fallback.empty()) {
        model_fallback = value;
      }
      else if (key == "CPU implementer" && vendor_fallback.empty()) {
        vendor_fallback = value;
      }
    }
    if (result.model.empty()) result.model = model_fallback;
    if (result.vendor.empty()) result.vendor = vendor_fallback;
    if (result.threads == 0) result.threads = processors;
    result.cores = cores > 0 ? cores : result.threads;
    if (result.cores > result.threads) result.cores = result.threads;
    return result;
  }

  os_info_t
  parse_os_release(std::string_view text, std::string_view kernel_release) {
    os_info_t result;
    std::map<std::string, std::string> values;
    std::size_t position = 0;
    while (position < text.size()) {
      const auto end = text.find('\n', position);
      const auto line = text.substr(position, end == std::string_view::npos ? text.size() - position : end - position);
      position = end == std::string_view::npos ? text.size() : end + 1;
      const auto trimmed = trim(line);
      if (trimmed.empty() || trimmed.front() == '#') continue;
      const auto separator = trimmed.find('=');
      if (separator == std::string::npos) continue;
      values[trimmed.substr(0, separator)] = unquote(trimmed.substr(separator + 1));
    }
    const auto value = [&](std::string_view key) {
      const auto found = values.find(std::string(key));
      return found == values.end() ? std::string {} : found->second;
    };
    result.name = value("NAME");
    if (result.name.empty()) result.name = value("PRETTY_NAME");
    result.version = value("VERSION_ID");
    if (result.version.empty()) result.version = value("VERSION");
    result.build = std::string { kernel_release };
    return result;
  }

  std::optional<int>
  parse_mhz(std::string_view text) {
    const auto trimmed = trim(text);
    if (trimmed.empty()) return std::nullopt;
    unsigned value = 0;
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (result.ec != std::errc {} || result.ptr == trimmed.data()) return std::nullopt;
    return static_cast<int>(value);
  }

  bool
  is_streaming_adapter(std::string_view configured, const std::vector<std::string> &identifiers, std::size_t index) {
    if (configured.empty()) return index == 0;
    const auto wanted = lower(configured);
    // A device path is also accepted by its last component (/dev/dri/renderD128).
    const auto separator = wanted.find_last_of("/\\");
    const auto basename = separator == std::string::npos ? wanted : wanted.substr(separator + 1);
    for (const auto &identifier : identifiers) {
      const auto candidate = lower(identifier);
      if (candidate == wanted || candidate == basename) return true;
      if (!candidate.empty() && (candidate.find(wanted) != std::string::npos || wanted.find(candidate) != std::string::npos)) return true;
    }
    return false;
  }

  nlohmann::json
  to_json(const snapshot_t &snapshot) {
    nlohmann::json output;
    output["version"] = snapshot.version;
    output["checked_at"] = snapshot.checked_at;
    output["cpu"] = {
      { "model", snapshot.cpu.model },
      { "vendor", snapshot.cpu.vendor },
      { "cores", snapshot.cpu.cores },
      { "threads", snapshot.cpu.threads },
      { "base_mhz", optional_json(snapshot.cpu.base_mhz) },
    };
    output["gpus"] = nlohmann::json::array();
    for (const auto &gpu : snapshot.gpus) {
      output["gpus"].push_back({
        { "name", gpu.name },
        { "vendor", gpu.vendor },
        { "driver_version", gpu.driver_version },
        { "vram_mb", optional_json(gpu.vram_mb) },
        { "is_streaming_adapter", gpu.is_streaming_adapter },
      });
    }
    output["nics"] = nlohmann::json::array();
    for (const auto &nic : snapshot.nics) {
      output["nics"].push_back({
        { "name", nic.name },
        { "description", nic.description },
        { "type", to_string(nic.type) },
        { "up", nic.up },
        { "link_speed_mbps", optional_json(nic.link_speed_mbps) },
        { "ipv4", nic.ipv4 },
        { "ipv6", nic.ipv6 },
        { "mac", nic.mac },
        { "is_default_route", nic.is_default_route },
        { "wifi", {
                    { "ssid_masked", nic.wifi.ssid_masked },
                    { "signal", optional_json(nic.wifi.signal) },
                  } },
      });
    }
    output["os"] = {
      { "name", snapshot.os.name },
      { "version", snapshot.os.version },
      { "build", snapshot.os.build },
    };
    output["host"] = { { "name", snapshot.host } };
    return output;
  }

  std::string_view
  to_string(nic_type_e value) {
    switch (value) {
      case nic_type_e::ethernet:
        return "ethernet";
      case nic_type_e::wifi:
        return "wifi";
      case nic_type_e::mesh:
        return "mesh";
      case nic_type_e::virtual_interface:
        return "virtual";
      case nic_type_e::other:
        return "other";
    }
    return "other";
  }

}  // namespace system_info
