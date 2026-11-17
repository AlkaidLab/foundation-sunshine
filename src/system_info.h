/**
 * @file src/system_info.h
 * @brief Host inventory for the Web UI system tiles: CPU, GPUs, network
 *        adapters, operating system and host name.
 *
 * The inventory is read from the host's own interfaces and registries. It
 * never makes an external request. The JSON contract is GET /api/system/info.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace system_info {
  /// Version of the JSON contract served by GET /api/system/info.
  constexpr int JSON_VERSION = 1;

  /// Interval during which a cached inventory is reused without probing again.
  constexpr std::int64_t CACHE_TTL_SECONDS = 60;

  enum class nic_type_e {
    ethernet,
    wifi,
    mesh,
    virtual_interface,
    other,
  };

  struct wifi_info_t {
    std::string ssid_masked;  ///< Empty when the platform does not expose it.
    std::optional<int> signal;  ///< 0..100, when the platform reports a quality.
  };

  struct nic_info_t {
    std::string name;
    std::string description;
    nic_type_e type = nic_type_e::other;
    bool up = false;
    std::optional<int> link_speed_mbps;  ///< Nothing when the link reports no speed.
    std::vector<std::string> ipv4;
    std::vector<std::string> ipv6;  ///< Link-local addresses are excluded.
    std::string mac;  ///< Masked to the OUI, for example "aa:bb:cc".
    bool is_default_route = false;
    wifi_info_t wifi;
  };

  struct gpu_info_t {
    std::string name;
    std::string vendor;
    std::string driver_version;  ///< Empty when the platform does not expose it.
    std::optional<int> vram_mb;
    bool is_streaming_adapter = false;

    /// Values accepted when matching config video.adapter_name. Not served.
    std::vector<std::string> identifiers;
  };

  struct cpu_info_t {
    std::string model;
    std::string vendor;
    int cores = 0;
    int threads = 0;
    std::optional<int> base_mhz;
  };

  struct os_info_t {
    std::string name;
    std::string version;
    std::string build;
  };

  struct snapshot_t {
    int version = JSON_VERSION;
    std::string checked_at;  ///< UTC, "YYYY-MM-DDTHH:MM:SSZ".
    cpu_info_t cpu;
    std::vector<gpu_info_t> gpus;
    std::vector<nic_info_t> nics;
    os_info_t os;
    std::string host;
  };

  /// Derive the adapter type from the platform facts.
  nic_type_e
  classify_nic_type(std::string_view name, std::string_view description, bool wireless, bool mesh, bool tunnel);

  /// Mask a MAC address to its OUI: "aa:bb:cc:dd:ee:ff" becomes "aa:bb:cc".
  std::string
  mask_mac(std::string_view raw);

  /// Keep the beginning of an SSID and hide the rest.
  std::string
  mask_ssid(std::string_view ssid);

  /// CPU identity and topology from Linux /proc/cpuinfo text.
  cpu_info_t
  parse_linux_cpuinfo(std::string_view text);

  /// Operating system identity from os-release text and the kernel release.
  os_info_t
  parse_os_release(std::string_view text, std::string_view kernel_release);

  /// "3400.000" and "3400 MHz" both yield 3400.
  std::optional<int>
  parse_mhz(std::string_view text);

  /**
   * @brief Whether a GPU is the one Sunshine encodes on.
   * @param configured  config video.adapter_name, empty for automatic selection.
   * @param identifiers Adapter names and device paths accepted for this GPU.
   * @param index       Position in enumeration order, used when automatic.
   */
  bool
  is_streaming_adapter(std::string_view configured, const std::vector<std::string> &identifiers, std::size_t index);

  /// Read the inventory, reusing the cached result unless refresh is requested.
  snapshot_t
  sample(std::string_view configured_adapter, bool refresh);

  /// Read the inventory from the host without consulting the cache.
  snapshot_t
  collect(std::string_view configured_adapter);

  /// Render a snapshot as the versioned JSON contract.
  nlohmann::json
  to_json(const snapshot_t &snapshot);

  std::string_view
  to_string(nic_type_e value);
}  // namespace system_info
