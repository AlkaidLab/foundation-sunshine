/**
 * @file src/system_info_probe.cpp
 * @brief Host probes behind GET /api/system/info.
 *
 * Every read is local: IP Helper and DXGI on Windows, getifaddrs and sysfs on
 * Linux, getifaddrs and sysctl on macOS. The inventory is cached by the caller.
 */
#include "system_info.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/asio/ip/address.hpp>

#include "network_reachability.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <combaseapi.h>
  #include <dxgi.h>
  #include <iphlpapi.h>
  #include <windows.h>
  #include <winsock2.h>
  #include <wlanapi.h>
  #include <ws2tcpip.h>
#else
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#ifdef __APPLE__
  #include <net/if_dl.h>
  #include <net/route.h>
  #include <sys/sysctl.h>
#endif

namespace system_info {
  namespace {
    namespace ip = boost::asio::ip;

    [[maybe_unused]] std::string
    read_file(const std::string &path) {
      std::ifstream input { path };
      if (!input) return {};
      std::ostringstream text;
      text << input.rdbuf();
      return text.str();
    }

    [[maybe_unused]] std::string
    first_line(std::string value) {
      while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
      const auto end = value.find('\n');
      if (end != std::string::npos) value.resize(end);
      return value;
    }

    [[maybe_unused]] std::optional<int>
    read_int(const std::string &path) {
      const auto value = parse_mhz(first_line(read_file(path)));
      if (!value) return std::nullopt;
      return *value;
    }

    [[maybe_unused]] std::optional<std::uint64_t>
    read_u64(const std::string &path) {
      const auto text = first_line(read_file(path));
      if (text.empty()) return std::nullopt;
      std::uint64_t value = 0;
      const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc {} || result.ptr == text.data()) return std::nullopt;
      return value;
    }

    std::string
    gpu_vendor_name(std::uint32_t vendor_id) {
      switch (vendor_id) {
        case 0x10de:
          return "nvidia";
        case 0x1002:
          return "amd";
        case 0x8086:
          return "intel";
        case 0x13b5:
          return "arm";
        case 0x5143:
          return "qualcomm";
        default:
          break;
      }
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "0x%04x", vendor_id);
      return buffer;
    }

    std::string
    cpu_vendor_name(std::string_view vendor) {
      if (vendor == "GenuineIntel") return "intel";
      if (vendor == "AuthenticAMD") return "amd";
      if (vendor == "ARM") return "arm";
      return std::string { vendor };
    }

    void
    sort_addresses(std::vector<std::string> &addresses) {
      std::sort(addresses.begin(), addresses.end());
      addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    }

    std::string
    host_name() {
      // ms-getcomputernameexw-20261103.
#ifdef _WIN32
      wchar_t buffer[256];
      DWORD size = static_cast<DWORD>(std::size(buffer));
      if (GetComputerNameExW(ComputerNameDnsHostname, buffer, &size) || GetComputerNameW(buffer, &size)) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        if (length > 0) {
          std::string text(static_cast<std::size_t>(length), '\0');
          WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), text.data(), length, nullptr, nullptr);
          return text;
        }
      }
      return {};
#else
      std::array<char, 256> buffer {};
      if (gethostname(buffer.data(), buffer.size()) != 0) return {};
      return std::string { buffer.data() };
#endif
    }

    // ---- Windows ---------------------------------------------------------
#ifdef _WIN32
    std::string
    to_utf8(const wchar_t *value) {
      if (!value || !*value) return {};
      const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
      if (length <= 1) return {};
      std::string text(static_cast<std::size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), length, nullptr, nullptr);
      text.pop_back();
      return text;
    }

    std::optional<std::string>
    registry_string(HKEY root, const wchar_t *path, const wchar_t *name) {
      DWORD type = 0;
      DWORD size = 0;
      if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return std::nullopt;
      }
      std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
      if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, &type, buffer.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return to_utf8(buffer.data());
    }

    std::optional<DWORD>
    registry_dword(HKEY root, const wchar_t *path, const wchar_t *name) {
      DWORD value = 0;
      DWORD type = 0;
      DWORD size = sizeof(value);
      if (RegGetValueW(root, path, name, RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) return std::nullopt;
      return value;
    }

    cpu_info_t
    probe_cpu() {
      // ms-reggetvaluew-20261103; ms-getlogicalprocessorinformationex-20261103.
      cpu_info_t cpu;
      constexpr auto path = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
      if (const auto model = registry_string(HKEY_LOCAL_MACHINE, path, L"ProcessorNameString")) cpu.model = *model;
      if (const auto vendor = registry_string(HKEY_LOCAL_MACHINE, path, L"VendorIdentifier")) cpu.vendor = cpu_vendor_name(*vendor);
      if (const auto mhz = registry_dword(HKEY_LOCAL_MACHINE, path, L"~MHz")) cpu.base_mhz = static_cast<int>(*mhz);

      // RelationProcessorCore yields one record per physical core; the mask
      // lists the logical processors of that core.
      DWORD size = 0;
      GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
      if (size == 0) return cpu;
      std::vector<std::uint8_t> buffer(size);
      if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &size)) {
        return cpu;
      }
      int cores = 0;
      int threads = 0;
      for (DWORD offset = 0; offset < size;) {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) {
          ++cores;
          auto mask = entry->Processor.GroupMask[0].Mask;
          while (mask) {
            threads += static_cast<int>(mask & 1U);
            mask >>= 1U;
          }
        }
        if (entry->Size == 0) break;
        offset += entry->Size;
      }
      cpu.cores = cores;
      cpu.threads = threads > 0 ? threads : cores;
      return cpu;
    }

    std::string
    driver_version_for(const std::string &description) {
      // Display adapter class key: ms-reggetvaluew-20261103.
      constexpr auto class_path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
      HKEY root = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_path, 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return {};
      std::string result;
      for (DWORD index = 0;; ++index) {
        wchar_t subkey[64];
        DWORD length = static_cast<DWORD>(std::size(subkey));
        if (RegEnumKeyExW(root, index, subkey, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const auto desc = registry_string(root, subkey, L"DriverDesc");
        if (!desc) continue;
        auto left = *desc;
        auto right = description;
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (left != right) continue;
        if (const auto version = registry_string(root, subkey, L"DriverVersion")) {
          result = *version;
          break;
        }
      }
      RegCloseKey(root);
      return result;
    }

    void
    probe_gpus(std::vector<gpu_info_t> &gpus) {
      // ms-dxgi-adapter-desc1-20261103, ms-dxgi-enumadapters1-20261103,
      // ms-dxgi-adapter-flag-20261103.
      IDXGIFactory1 *factory = nullptr;
      if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) return;
      for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) != S_OK || !adapter) break;
        DXGI_ADAPTER_DESC1 description {};
        const bool valid = SUCCEEDED(adapter->GetDesc1(&description));
        adapter->Release();
        if (!valid) continue;
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        gpu_info_t gpu;
        gpu.name = to_utf8(description.Description);
        gpu.vendor = gpu_vendor_name(description.VendorId);
        gpu.driver_version = driver_version_for(gpu.name);
        if (description.DedicatedVideoMemory > 0) {
          gpu.vram_mb = static_cast<int>(description.DedicatedVideoMemory / (1024ULL * 1024ULL));
        }
        gpu.identifiers.push_back(gpu.name);
        gpus.push_back(std::move(gpu));
      }
      factory->Release();
    }

    std::set<std::uint32_t>
    default_route_indexes() {
      std::set<std::uint32_t> result;
      DWORD size = 0;
      if (GetIpForwardTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return result;
      std::vector<std::uint8_t> buffer(size);
      auto *table = reinterpret_cast<MIB_IPFORWARDTABLE *>(buffer.data());
      if (GetIpForwardTable(table, &size, FALSE) != NO_ERROR) return result;
      for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto &row = table->table[index];
        if (row.dwForwardDest == 0 && row.dwForwardMask == 0) result.insert(row.dwForwardIfIndex);
      }
      return result;
    }

    using wifi_state_t = wifi_info_t;

    std::map<std::string, wifi_state_t>
    wifi_states() {
      // ms-wlanopenhandle-20261103, ms-wlanenuminterfaces-20261103,
      // ms-wlanqueryinterface-20261103, ms-wlan-association-attributes-20261103.
      std::map<std::string, wifi_state_t> result;
      HANDLE client = nullptr;
      DWORD version = 0;
      if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || !client) return result;
      PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
      if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces) {
        for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
          const auto &entry = interfaces->InterfaceInfo[index];
          PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
          DWORD attributes_size = 0;
          WLAN_OPCODE_VALUE_TYPE opcode_type {};
          if (WlanQueryInterface(client, &entry.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
                &attributes_size, reinterpret_cast<PVOID *>(&attributes), &opcode_type) != ERROR_SUCCESS ||
              !attributes) {
            continue;
          }
          wchar_t guid[64];
          if (StringFromGUID2(entry.InterfaceGuid, guid, static_cast<int>(std::size(guid))) > 0) {
            const auto &association = attributes->wlanAssociationAttributes;
            wifi_state_t state;
            std::string ssid(reinterpret_cast<const char *>(association.dot11Ssid.ucSSID),
              std::min<std::size_t>(association.dot11Ssid.uSSIDLength, sizeof(association.dot11Ssid.ucSSID)));
            state.ssid_masked = mask_ssid(ssid);
            state.signal = static_cast<int>(std::min<ULONG>(association.wlanSignalQuality, 100));
            auto key = to_utf8(guid);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            result.emplace(std::move(key), std::move(state));
          }
          WlanFreeMemory(attributes);
        }
        WlanFreeMemory(interfaces);
      }
      WlanCloseHandle(client, nullptr);
      return result;
    }

    void
    probe_nics(std::vector<nic_info_t> &nics) {
      // ms-ip-adapter-addresses-lh-20261103, iana-iftype-20261103,
      // ms-getipforwardtable-20261103.
      ULONG size = 16384;
      std::vector<std::uint8_t> buffer(size);
      ULONG status = ERROR_BUFFER_OVERFLOW;
      for (int attempt = 0; attempt < 3; ++attempt) {
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(size);
      }
      if (status != NO_ERROR) return;

      const auto routes = default_route_indexes();
      const auto wifi = wifi_states();
      for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        nic_info_t nic;
        nic.name = adapter->FriendlyName ? to_utf8(adapter->FriendlyName) : std::string(adapter->AdapterName ? adapter->AdapterName : "");
        nic.description = to_utf8(adapter->Description);
        nic.up = adapter->OperStatus == IfOperStatusUp;
        nic.is_default_route = routes.count(adapter->IfIndex) != 0;
        if (adapter->TransmitLinkSpeed > 0) nic.link_speed_mbps = static_cast<int>(adapter->TransmitLinkSpeed / 1000000ULL);
        if (adapter->PhysicalAddressLength >= 3) {
          char mac[32];
          std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
            adapter->PhysicalAddress[0], adapter->PhysicalAddress[1], adapter->PhysicalAddress[2],
            adapter->PhysicalAddressLength > 3 ? adapter->PhysicalAddress[3] : 0,
            adapter->PhysicalAddressLength > 4 ? adapter->PhysicalAddress[4] : 0,
            adapter->PhysicalAddressLength > 5 ? adapter->PhysicalAddress[5] : 0);
          nic.mac = mask_mac(mac);
        }
        for (auto *address = adapter->FirstUnicastAddress; address; address = address->Next) {
          const auto *socket_address = address->Address.lpSockaddr;
          if (!socket_address) continue;
          if (socket_address->sa_family == AF_INET) {
            const auto *v4 = reinterpret_cast<const sockaddr_in *>(socket_address);
            ip::address_v4::bytes_type bytes;
            std::memcpy(bytes.data(), &v4->sin_addr, bytes.size());
            nic.ipv4.push_back(ip::address_v4(bytes).to_string());
          }
          else if (socket_address->sa_family == AF_INET6) {
            const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(socket_address);
            // rfc4291-20261103 section 2.5.6: fe80::/10 is link-local.
            if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) continue;
            ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
            nic.ipv6.push_back(ip::address_v6(bytes, v6->sin6_scope_id).to_string());
          }
        }
        sort_addresses(nic.ipv4);
        sort_addresses(nic.ipv6);

        const bool wireless = adapter->IfType == IF_TYPE_IEEE80211;
        const bool tunnel = adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP ||
                            adapter->IfType == IF_TYPE_WWANPP || adapter->IfType == IF_TYPE_WWANPP2;
        std::vector<std::string> addresses = nic.ipv4;
        addresses.insert(addresses.end(), nic.ipv6.begin(), nic.ipv6.end());
        const bool mesh = reachability::classify_adapter(nic.name, nic.description, addresses) != reachability::mesh_kind_e::none;
        nic.type = classify_nic_type(nic.name, nic.description, wireless, mesh, tunnel);
        if (wireless) {
          std::string key = adapter->AdapterName ? adapter->AdapterName : "";
          std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (const auto found = wifi.find(key); found != wifi.end()) nic.wifi = found->second;
        }
        nics.push_back(std::move(nic));
      }
    }

    os_info_t
    probe_os() {
      // ms-os-version-20261103, ms-reggetvaluew-20261103.
      os_info_t os;
      constexpr auto path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
      os.name = registry_string(HKEY_LOCAL_MACHINE, path, L"ProductName").value_or("Windows");
      os.version = registry_string(HKEY_LOCAL_MACHINE, path, L"DisplayVersion").value_or(registry_string(HKEY_LOCAL_MACHINE, path, L"ReleaseId").value_or(""));
      const auto revision = registry_dword(HKEY_LOCAL_MACHINE, path, L"UBR");
      const auto build_number = registry_string(HKEY_LOCAL_MACHINE, path, L"CurrentBuildNumber").value_or(std::to_string(registry_dword(HKEY_LOCAL_MACHINE, path, L"CurrentBuildNumber").value_or(0)));
      if (revision)
        os.build = build_number + "." + std::to_string(*revision);
      else
        os.build = build_number;
      if (parse_mhz(build_number).value_or(0) >= 22000) {
        const auto marker = os.name.find("Windows 10");
        if (marker != std::string::npos) os.name.replace(marker, std::string("Windows 10").size(), "Windows 11");
      }
      return os;
    }
#endif

    // ---- Linux and macOS ------------------------------------------------
#ifndef _WIN32
    struct local_adapter_t {
      std::string name;
      std::string description;
      std::vector<std::string> addresses;
      std::vector<std::string> ipv4;
      std::vector<std::string> ipv6;
      std::string mac;
      bool up = false;
      bool wireless = false;
      std::uint32_t index = 0;
    };

    std::vector<local_adapter_t>
    enumerate_local_adapters() {
      std::vector<local_adapter_t> result;
      ifaddrs *addresses = nullptr;
      if (getifaddrs(&addresses) != 0) return result;
      auto cleanup = [&]() { freeifaddrs(addresses); };
      std::map<std::string, std::size_t> index;
      for (auto *entry = addresses; entry; entry = entry->ifa_next) {
        if (!entry->ifa_name || (entry->ifa_flags & IFF_LOOPBACK)) continue;
        const std::string name { entry->ifa_name };
        auto found = index.find(name);
        if (found == index.end()) {
          found = index.emplace(name, result.size()).first;
          local_adapter_t adapter;
          adapter.name = name;
          adapter.description = name;
          adapter.up = (entry->ifa_flags & IFF_UP) != 0;
          adapter.index = if_nametoindex(name.c_str());
          result.push_back(std::move(adapter));
        }
        auto &adapter = result[found->second];
        if (!entry->ifa_addr) continue;
        if (entry->ifa_addr->sa_family == AF_INET) {
          const auto *v4 = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
          ip::address_v4::bytes_type bytes;
          std::memcpy(bytes.data(), &v4->sin_addr, bytes.size());
          const auto text = ip::address_v4(bytes).to_string();
          adapter.ipv4.push_back(text);
          adapter.addresses.push_back(text);
        }
        else if (entry->ifa_addr->sa_family == AF_INET6) {
          const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(entry->ifa_addr);
          ip::address_v6::bytes_type bytes;
          std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
          const ip::address_v6 parsed { bytes, v6->sin6_scope_id };
          // rfc4291-20261103 section 2.5.6: fe80::/10 is link-local.
          if (parsed.is_link_local()) continue;
          const auto text = parsed.to_string();
          adapter.ipv6.push_back(text);
          adapter.addresses.push_back(text);
        }
  #ifdef __APPLE__
        else if (entry->ifa_addr->sa_family == AF_LINK) {
          const auto *link = reinterpret_cast<const sockaddr_dl *>(entry->ifa_addr);
          const auto *raw = reinterpret_cast<const unsigned char *>(LLADDR(link));
          if (raw && link->sdl_alen >= 3) {
            char mac[32];
            std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", raw[0], raw[1], raw[2],
              link->sdl_alen > 3 ? raw[3] : 0, link->sdl_alen > 4 ? raw[4] : 0, link->sdl_alen > 5 ? raw[5] : 0);
            adapter.mac = mask_mac(mac);
          }
        }
  #endif
      }
      cleanup();
      for (auto &adapter : result) {
        sort_addresses(adapter.ipv4);
        sort_addresses(adapter.ipv6);
        sort_addresses(adapter.addresses);
      }
      return result;
    }

    std::set<std::uint32_t>
    local_default_route_indexes() {
      std::set<std::uint32_t> result;
  #if defined(__linux__)
      std::istringstream routes { read_file("/proc/net/route") };
      std::string line;
      std::getline(routes, line);
      while (std::getline(routes, line)) {
        std::istringstream fields { line };
        std::string name, destination, gateway, flags;
        if (!(fields >> name >> destination >> gateway >> flags)) continue;
        if (destination != "00000000") continue;
        try {
          if ((std::stoul(flags, nullptr, 16) & 0x1U) == 0) continue;
        }
        catch (...) {
          continue;
        }
        if (const auto index = if_nametoindex(name.c_str())) result.insert(index);
      }
      std::istringstream routes6 { read_file("/proc/net/ipv6_route") };
      while (std::getline(routes6, line)) {
        std::istringstream fields { line };
        std::string destination, prefix, source, source_prefix, next_hop, metric, references, use, flags, name;
        if (!(fields >> destination >> prefix >> source >> source_prefix >> next_hop >> metric >> references >> use >> flags >> name)) continue;
        if (prefix != "00" || destination != std::string(32, '0')) continue;
        if (const auto index = if_nametoindex(name.c_str())) result.insert(index);
      }
  #elif defined(__APPLE__)
      int mib[] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_DUMP, 0 };
      std::size_t size = 0;
      if (sysctl(mib, 6, nullptr, &size, nullptr, 0) != 0 || size == 0) return result;
      std::vector<std::uint8_t> buffer(size);
      if (sysctl(mib, 6, buffer.data(), &size, nullptr, 0) != 0) return result;
      for (std::size_t offset = 0; offset + sizeof(rt_msghdr) <= size;) {
        const auto *header = reinterpret_cast<const rt_msghdr *>(buffer.data() + offset);
        if (header->rtm_msglen == 0) break;
        if ((header->rtm_flags & RTF_GATEWAY) && header->rtm_addrs & RTA_DST) {
          const auto *destination = reinterpret_cast<const sockaddr *>(buffer.data() + offset + sizeof(rt_msghdr));
          const bool default_route = destination->sa_family == AF_INET ?
                                       reinterpret_cast<const sockaddr_in *>(destination)->sin_addr.s_addr == 0 :
                                       destination->sa_family == AF_INET6 &&
                                         IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6 *>(destination)->sin6_addr);
          if (default_route) result.insert(header->rtm_index);
        }
        offset += header->rtm_msglen;
      }
  #endif
      return result;
    }

    std::optional<int>
    wifi_signal_percent(const std::string &name) {
  #if defined(__linux__)
      std::istringstream table { read_file("/proc/net/wireless") };
      std::string line;
      for (int header = 0; header < 2 && std::getline(table, line); ++header) {
      }
      while (std::getline(table, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto interface_name = line.substr(0, colon);
        while (!interface_name.empty() && std::isspace(static_cast<unsigned char>(interface_name.back()))) interface_name.pop_back();
        if (interface_name != name) continue;
        std::istringstream fields { line.substr(colon + 1) };
        std::string status, link;
        if (!(fields >> status >> link)) continue;
        auto value = parse_mhz(link);
        if (!value) return std::nullopt;
        return std::clamp(*value, 0, 100);
      }
      return std::nullopt;
  #else
      static_cast<void>(name);
      return std::nullopt;
  #endif
    }

  #if defined(__APPLE__)
    std::map<std::uint32_t, int>
    interface_speeds();
  #endif

    void
    probe_nics(std::vector<nic_info_t> &nics) {
      // linux-sysfs-class-net-20261103, linux-operstates-20261103,
      // man-proc-20261103 (/proc/net/route and /proc/net/ipv6_route),
      // apple-getifaddrs-20261103, apple-sysctl-20261103.
      const auto routes = local_default_route_indexes();
  #if defined(__APPLE__)
      const auto speeds = interface_speeds();
  #endif
      for (auto &adapter : enumerate_local_adapters()) {
  #if defined(__linux__)
        adapter.description = adapter.name;
        const auto base = std::string { "/sys/class/net/" } + adapter.name;
        const auto mac = first_line(read_file(base + "/address"));
        if (!mac.empty()) adapter.mac = mask_mac(mac);
        const auto operstate = first_line(read_file(base + "/operstate"));
        if (!operstate.empty()) adapter.up = adapter.up && (operstate == "up" || operstate == "unknown");
        std::error_code wireless_error;
        adapter.wireless = std::filesystem::is_directory(base + "/wireless", wireless_error) && !wireless_error;
  #endif
        nic_info_t nic;
        nic.name = adapter.name;
        nic.description = adapter.description;
        nic.up = adapter.up;
        nic.mac = adapter.mac;
        nic.ipv4 = adapter.ipv4;
        nic.ipv6 = adapter.ipv6;
        nic.is_default_route = routes.count(adapter.index) != 0;
  #if defined(__linux__)
        if (const auto speed = read_int(std::string { "/sys/class/net/" } + adapter.name + "/speed")) {
          if (*speed > 0) nic.link_speed_mbps = *speed;
        }
        if (adapter.wireless) nic.wifi.signal = wifi_signal_percent(adapter.name);
  #elif defined(__APPLE__)
        if (const auto found = speeds.find(adapter.index); found != speeds.end()) nic.link_speed_mbps = found->second;
  #endif
        const bool mesh = reachability::classify_adapter(adapter.name, adapter.description, adapter.addresses) != reachability::mesh_kind_e::none;
  #if defined(__APPLE__)
        const bool tunnel = adapter.name.rfind("utun", 0) == 0 || adapter.name.rfind("tun", 0) == 0 ||
                            adapter.name.rfind("tap", 0) == 0 || adapter.name.rfind("gif", 0) == 0 ||
                            adapter.name.rfind("stf", 0) == 0 || adapter.name.rfind("wg", 0) == 0;
  #else
        const bool tunnel = adapter.name.rfind("tun", 0) == 0 || adapter.name.rfind("tap", 0) == 0 ||
                            adapter.name.rfind("wg", 0) == 0;
  #endif
        nic.type = classify_nic_type(adapter.name, adapter.description, adapter.wireless, mesh, tunnel);
        nics.push_back(std::move(nic));
      }
    }

  #if defined(__APPLE__)
    std::map<std::uint32_t, int>
    interface_speeds() {
      std::map<std::uint32_t, int> speeds;
      int mib[] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
      std::size_t size = 0;
      if (sysctl(mib, 6, nullptr, &size, nullptr, 0) != 0 || size == 0) return speeds;
      std::vector<std::uint8_t> buffer(size);
      if (sysctl(mib, 6, buffer.data(), &size, nullptr, 0) != 0) return speeds;
      for (std::size_t offset = 0; offset + sizeof(if_msghdr) <= size;) {
        const auto *header = reinterpret_cast<const if_msghdr *>(buffer.data() + offset);
        if (header->ifm_msglen == 0) break;
        if (header->ifm_type == RTM_IFINFO2) {
          const auto *detail = reinterpret_cast<const if_msghdr2 *>(header);
          const auto bits = detail->ifm_data.ifi_baudrate;
          if (bits > 0) speeds[detail->ifm_index] = static_cast<int>(bits / 1000000ULL);
        }
        offset += header->ifm_msglen;
      }
      return speeds;
    }
  #endif

    cpu_info_t
    probe_cpu() {
  #if defined(__linux__)
      // linux-x86-proc-c-20261103, linux-intel-pstate-20261103.
      auto cpu = parse_linux_cpuinfo(read_file("/proc/cpuinfo"));
      cpu.vendor = cpu_vendor_name(cpu.vendor);
      if (const auto base = read_int("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency")) {
        cpu.base_mhz = *base / 1000;
      }
      return cpu;
  #elif defined(__APPLE__)
      cpu_info_t cpu;
      const auto sysctl_string = [](const char *name) {
        std::array<char, 512> buffer {};
        std::size_t size = buffer.size();
        if (sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) return std::string {};
        return std::string { buffer.data() };
      };
      cpu.model = sysctl_string("machdep.cpu.brand_string");
      if (cpu.model.empty()) cpu.model = sysctl_string("hw.model");
      cpu.vendor = cpu_vendor_name(sysctl_string("machdep.cpu.vendor"));
      const auto count = [](const char *name) {
        int value = 0;
        std::size_t size = sizeof(value);
        if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return 0;
        return value;
      };
      cpu.cores = count("hw.physicalcpu");
      cpu.threads = count("hw.logicalcpu");
      if (cpu.threads == 0) cpu.threads = cpu.cores;
      return cpu;
  #endif
    }

    os_info_t
    probe_os() {
  #if defined(__linux__)
      // man-os-release-20261103, man-proc-20261103.
      const auto release = first_line(read_file("/proc/sys/kernel/osrelease"));
      auto os = parse_os_release(read_file("/etc/os-release"), release);
      if (os.name.empty()) os.name = "Linux";
      return os;
  #elif defined(__APPLE__)
      os_info_t os { "macOS", "", "" };
      const auto sysctl_string = [](const char *name) {
        std::array<char, 256> buffer {};
        std::size_t size = buffer.size();
        if (sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) return std::string {};
        return std::string { buffer.data() };
      };
      os.version = sysctl_string("kern.osproductversion");
      os.build = sysctl_string("kern.osversion");
      return os;
  #endif
    }

  #if defined(__linux__)
    void
    probe_gpus(std::vector<gpu_info_t> &gpus) {
      // sysfs class attributes: linux-sysfs-class-net-20261103.
      std::error_code error;
      for (std::filesystem::directory_iterator iterator { "/sys/class/drm", error }, end; !error && iterator != end; iterator.increment(error)) {
        const auto filename = iterator->path().filename().string();
        if (filename.rfind("card", 0) != 0 || filename.find('-') != std::string::npos) continue;
        const auto device = iterator->path() / "device";
        const auto vendor_text = first_line(read_file((device / "vendor").string()));
        if (vendor_text.empty()) continue;
        unsigned vendor_id = 0;
        try {
          vendor_id = static_cast<unsigned>(std::stoul(vendor_text, nullptr, 0));
        }
        catch (...) {
          continue;
        }
        gpu_info_t gpu;
        gpu.vendor = gpu_vendor_name(vendor_id);
        auto product = first_line(read_file((device / "product_name").string()));
        gpu.name = product.empty() ? gpu.vendor + " graphics adapter" : std::move(product);
        if (const auto vram = read_u64((device / "mem_info_vram_total").string())) {
          if (*vram > 0) gpu.vram_mb = static_cast<int>(*vram / (1024 * 1024));
        }
        std::error_code link_error;
        const auto driver = std::filesystem::read_symlink(device / "driver", link_error);
        if (!link_error) {
          gpu.driver_version = first_line(read_file("/sys/module/" + driver.filename().string() + "/version"));
        }
        gpu.identifiers.push_back(gpu.name);
        gpu.identifiers.push_back(filename);
        try {
          gpu.identifiers.push_back("renderD" + std::to_string(128 + std::stoi(filename.substr(4))));
        }
        catch (...) {
        }
        gpus.push_back(std::move(gpu));
      }
    }
  #elif defined(__APPLE__)
    void
    probe_gpus(std::vector<gpu_info_t> &gpus) {
      // No cheap public API exposes the adapter name and VRAM; macOS reports none.
      static_cast<void>(gpus);
    }
  #endif
#endif
  }  // namespace

  snapshot_t
  collect(std::string_view configured_adapter) {
    snapshot_t snapshot;
    snapshot.host = host_name();
    snapshot.cpu = probe_cpu();
    probe_gpus(snapshot.gpus);
    probe_nics(snapshot.nics);
    snapshot.os = probe_os();
    for (std::size_t index = 0; index < snapshot.gpus.size(); ++index) {
      snapshot.gpus[index].is_streaming_adapter = is_streaming_adapter(configured_adapter, snapshot.gpus[index].identifiers, index);
    }
    return snapshot;
  }
}  // namespace system_info
