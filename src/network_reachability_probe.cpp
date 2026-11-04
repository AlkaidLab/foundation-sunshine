/**
 * @file src/network_reachability_probe.cpp
 * @brief Host probes backing the reachability assessment.
 *
 * Everything here is confined to the host: interface enumeration, the process'
 * own listening sockets, the local Windows firewall policy, the gateway's UPnP
 * IGD service and a TLS handshake against the host's own WAN address.
 */
#include "network_reachability.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#include "utility.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <iphlpapi.h>
  #include <netfw.h>
  #include <oleauto.h>
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#ifdef __APPLE__
  #include <libproc.h>
  #include <sys/proc_info.h>
#endif

using namespace std::literals;

namespace reachability {
  namespace {
    namespace ip = boost::asio::ip;

    struct adapter_entry_t {
      std::uint32_t index = 0;
      std::string name;
      std::string description;
      std::vector<std::string> addresses;
      bool up = false;
    };

    struct socket_table_t {
      std::map<std::uint16_t, std::vector<std::string>> tcp;
      std::map<std::uint16_t, std::vector<std::string>> udp;
    };

    void
    add_socket(socket_table_t &table, const std::string &proto, std::uint16_t port, std::string address) {
      auto &target = proto == "tcp" ? table.tcp : table.udp;
      auto &entries = target[port];
      if (std::find(entries.begin(), entries.end(), address) == entries.end()) {
        entries.push_back(std::move(address));
      }
    }

    [[maybe_unused]] bool
    is_loopback(const std::string &value) {
      if (value == "::1" || value.rfind("127.", 0) == 0) return true;
      boost::system::error_code error;
      const auto parsed = ip::make_address(value, error);
      if (error) return false;
      return parsed.is_loopback();
    }

    std::set<std::uint32_t>
    default_route_indexes() {
      // Default-route tables: man-proc-20261103.
      std::set<std::uint32_t> result;
#if defined(__linux__)
      {
        std::ifstream routes { "/proc/net/route" };
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
      }
      {
        std::ifstream routes { "/proc/net/ipv6_route" };
        std::string line;
        while (std::getline(routes, line)) {
          std::istringstream fields { line };
          std::string destination, prefix, source, source_prefix, next_hop, metric, ref, use, flags, name;
          if (!(fields >> destination >> prefix >> source >> source_prefix >> next_hop >> metric >> ref >> use >> flags >> name)) continue;
          if (prefix != "00" || destination != std::string(32, '0')) continue;
          if (const auto index = if_nametoindex(name.c_str())) result.insert(index);
        }
      }
#elif defined(_WIN32)
      DWORD size = 0;
      if (GetIpForwardTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<unsigned char> buffer(size);
        auto *table = reinterpret_cast<MIB_IPFORWARDTABLE *>(buffer.data());
        if (GetIpForwardTable(table, &size, FALSE) == NO_ERROR) {
          for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto &row = table->table[i];
            if (row.dwForwardDest == 0 && row.dwForwardMask == 0) result.insert(row.dwForwardIfIndex);
          }
        }
      }
#endif
      return result;
    }

    bool
    is_virtual_adapter(const std::string &value) {
      auto name = value;
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      for (const auto prefix : { "br", "docker", "virbr", "veth", "lxc", "cni", "flannel", "kube", "vmnet", "vboxnet", "tap", "tun", "utun", "tailscale", "wg", "wt", "zt" }) {
        if (name.rfind(prefix, 0) == 0) return true;
      }
      return false;
    }

    struct resolve_state_t {
      std::mutex mutex;
      std::condition_variable ready;
      bool done = false;
      std::vector<std::string> addresses;
    };

    // getaddrinfo() has no timeout (man-getaddrinfo-20261103). The lookup runs on its own thread and the
    // probe waits a bounded time; a slow resolver leaves the host name
    // unresolved instead of holding the reachability cache lock.
    std::vector<std::string>
    resolve_host(std::string_view host, std::chrono::seconds timeout) {
      const std::string name { host };
      auto state = std::make_shared<resolve_state_t>();
      std::thread worker { [state, name]() {
#ifdef _WIN32
        static std::once_flag winsock_once;
        std::call_once(winsock_once, []() {
          WSADATA data;
          WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *result = nullptr;
        std::vector<std::string> addresses;
        if (getaddrinfo(name.c_str(), nullptr, &hints, &result) == 0) {
          for (auto *entry = result; entry; entry = entry->ai_next) {
            if (!entry->ai_addr) continue;
            if (entry->ai_family == AF_INET) {
              const auto *v4 = reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
              ip::address_v4::bytes_type bytes;
              std::memcpy(bytes.data(), &v4->sin_addr, bytes.size());
              addresses.push_back(ip::address_v4(bytes).to_string());
            }
            else if (entry->ai_family == AF_INET6) {
              const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
              ip::address_v6::bytes_type bytes;
              std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
              addresses.push_back(ip::address_v6(bytes).to_string());
            }
          }
          freeaddrinfo(result);
        }
        {
          std::lock_guard lock { state->mutex };
          state->addresses = std::move(addresses);
          state->done = true;
        }
        state->ready.notify_all();
      } };
      worker.detach();
      std::unique_lock lock { state->mutex };
      state->ready.wait_for(lock, timeout, [&]() { return state->done; });
      return state->addresses;
    }

    std::vector<adapter_entry_t>
    enumerate_adapters() {
      // GetAdaptersAddresses field contract: ms-getadaptersaddresses-20261103.
      std::vector<adapter_entry_t> result;
#ifdef _WIN32
      const auto wide_to_utf8 = [](const wchar_t *value) {
        if (!value) return std::string {};
        const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1) return std::string {};
        std::string text(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), length, nullptr, nullptr);
        text.pop_back();
        return text;
      };

      ULONG size = 16384;
      std::vector<unsigned char> buffer(size);
      ULONG status = ERROR_BUFFER_OVERFLOW;
      for (int attempt = 0; attempt < 3; ++attempt) {
        auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
          nullptr, adapters, &size);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(size);
      }
      if (status != NO_ERROR) return result;
      for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        adapter_entry_t entry;
        entry.index = adapter->IfIndex;
        entry.name = adapter->FriendlyName ? wide_to_utf8(adapter->FriendlyName) : std::string(adapter->AdapterName);
        entry.description = wide_to_utf8(adapter->Description);
        entry.up = adapter->OperStatus == IfOperStatusUp;
        for (auto *address = adapter->FirstUnicastAddress; address; address = address->Next) {
          const auto *sockaddr = address->Address.lpSockaddr;
          if (!sockaddr) continue;
          if (sockaddr->sa_family == AF_INET) {
            const auto *v4 = reinterpret_cast<const sockaddr_in *>(sockaddr);
            ip::address_v4::bytes_type bytes;
            std::memcpy(bytes.data(), &v4->sin_addr, bytes.size());
            entry.addresses.push_back(ip::address_v4(bytes).to_string());
          }
          else if (sockaddr->sa_family == AF_INET6) {
            const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(sockaddr);
            ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
            entry.addresses.push_back(ip::address_v6(bytes, v6->sin6_scope_id).to_string());
          }
        }
        if (!entry.addresses.empty()) result.push_back(std::move(entry));
      }
#else
      ifaddrs *addresses = nullptr;
      if (getifaddrs(&addresses) != 0) return result;
      auto cleanup = util::fail_guard([&]() { freeifaddrs(addresses); });
      std::map<std::string, std::size_t> index;
      for (auto *entry = addresses; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || (entry->ifa_flags & IFF_LOOPBACK)) continue;
        const auto family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        std::string value;
        if (family == AF_INET) {
          const auto *v4 = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
          ip::address_v4::bytes_type bytes;
          std::memcpy(bytes.data(), &v4->sin_addr, bytes.size());
          value = ip::address_v4(bytes).to_string();
        }
        else {
          const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(entry->ifa_addr);
          ip::address_v6::bytes_type bytes;
          std::memcpy(bytes.data(), &v6->sin6_addr, bytes.size());
          value = ip::address_v6(bytes, v6->sin6_scope_id).to_string();
        }
        if (is_loopback(value)) continue;
        const std::string name { entry->ifa_name ? entry->ifa_name : "" };
        auto found = index.find(name);
        if (found == index.end()) {
          found = index.emplace(name, result.size()).first;
          result.push_back({ if_nametoindex(name.c_str()), name, {}, {}, (entry->ifa_flags & IFF_UP) != 0 });
        }
        result[found->second].addresses.push_back(std::move(value));
      }
#endif
      return result;
    }

    // ---- listening sockets -------------------------------------------------

#if defined(__linux__)
    std::set<std::string>
    own_socket_inodes(std::uint32_t process_id) {
      std::set<std::string> inodes;
      const auto directory = process_id == 0 ? "/proc/self/fd"s : "/proc/"s + std::to_string(process_id) + "/fd"s;
      std::error_code error;
      std::filesystem::directory_iterator iterator { directory, error };
      if (error) return inodes;
      for (const auto &entry : iterator) {
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (error) {
          error.clear();
          continue;
        }
        const auto text = target.string();
        if (text.rfind("socket:[", 0) == 0 && text.back() == ']') {
          inodes.insert(text.substr(8, text.size() - 9));
        }
      }
      return inodes;
    }

    std::uint8_t
    hex_byte(const std::string &text, std::size_t offset) {
      const auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return 0;
      };
      return static_cast<std::uint8_t>((hex(text[offset]) << 4) | hex(text[offset + 1]));
    }

    void
    read_proc_table(const char *path, const std::string &proto, const std::set<std::string> &inodes,
      socket_table_t &table) {
      std::ifstream input { path };
      std::string line;
      std::getline(input, line);
      while (std::getline(input, line)) {
        std::istringstream fields { line };
        std::string slot, local, remote, state, queues, timers, retransmits, uid, timeout, inode;
        if (!(fields >> slot >> local >> remote >> state >> queues >> timers >> retransmits >> uid >> timeout >> inode)) {
          continue;
        }
        if (inodes.find(inode) == inodes.end()) continue;
        if (proto == "tcp" && state != "0A") continue;
        const auto colon = local.rfind(':');
        if (colon == std::string::npos) continue;
        const auto hex_address = local.substr(0, colon);
        const auto port = static_cast<std::uint16_t>(std::stoul(local.substr(colon + 1), nullptr, 16));
        std::string address;
        if (hex_address.size() == 8) {
          ip::address_v4::bytes_type bytes {
            hex_byte(hex_address, 6), hex_byte(hex_address, 4), hex_byte(hex_address, 2), hex_byte(hex_address, 0)
          };
          address = ip::address_v4(bytes).to_string();
          if (bytes == ip::address_v4::bytes_type {}) address = "0.0.0.0";
        }
        else if (hex_address.size() == 32) {
          ip::address_v6::bytes_type bytes {};
          for (std::size_t word = 0; word < 4; ++word) {
            const auto base = word * 8;
            bytes[word * 4] = hex_byte(hex_address, base + 6);
            bytes[word * 4 + 1] = hex_byte(hex_address, base + 4);
            bytes[word * 4 + 2] = hex_byte(hex_address, base + 2);
            bytes[word * 4 + 3] = hex_byte(hex_address, base);
          }
          address = ip::address_v6(bytes).to_string();
          if (bytes == ip::address_v6::bytes_type {}) address = "::";
        }
        else {
          continue;
        }
        add_socket(table, proto, port, std::move(address));
      }
    }

    socket_table_t
    own_sockets(std::uint32_t process_id) {
      socket_table_t table;
      const auto inodes = own_socket_inodes(process_id);
      read_proc_table("/proc/net/tcp", "tcp", inodes, table);
      read_proc_table("/proc/net/tcp6", "tcp", inodes, table);
      read_proc_table("/proc/net/udp", "udp", inodes, table);
      read_proc_table("/proc/net/udp6", "udp", inodes, table);
      return table;
    }
#elif defined(_WIN32)
    std::string
    v4_from_long(DWORD value) {
      const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
      return std::to_string(bytes[0]) + '.' + std::to_string(bytes[1]) + '.' + std::to_string(bytes[2]) + '.' + std::to_string(bytes[3]);
    }

    socket_table_t
    own_sockets(std::uint32_t process_id) {
      // Listener tables: ms-getextendedtcptable-20261103 and ms-getextendedudptable-20261103.
      socket_table_t table;
      const auto target_process = process_id == 0 ? GetCurrentProcessId() : process_id;
      {
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == ERROR_INSUFFICIENT_BUFFER) {
          std::vector<unsigned char> buffer(size);
          auto *entries = reinterpret_cast<MIB_TCPTABLE_OWNER_PID *>(buffer.data());
          if (GetExtendedTcpTable(entries, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
            for (DWORD i = 0; i < entries->dwNumEntries; ++i) {
              const auto &row = entries->table[i];
              if (row.dwOwningPid != target_process) continue;
              add_socket(table, "tcp", ntohs(static_cast<std::uint16_t>(row.dwLocalPort)), v4_from_long(row.dwLocalAddr));
            }
          }
        }
      }
      {
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0) == ERROR_INSUFFICIENT_BUFFER) {
          std::vector<unsigned char> buffer(size);
          auto *entries = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID *>(buffer.data());
          if (GetExtendedTcpTable(entries, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
            for (DWORD i = 0; i < entries->dwNumEntries; ++i) {
              const auto &row = entries->table[i];
              if (row.dwOwningPid != target_process) continue;
              ip::address_v6::bytes_type bytes;
              std::memcpy(bytes.data(), row.ucLocalAddr, bytes.size());
              add_socket(table, "tcp", ntohs(static_cast<std::uint16_t>(row.dwLocalPort)), ip::address_v6(bytes).to_string());
            }
          }
        }
      }
      {
        DWORD size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER) {
          std::vector<unsigned char> buffer(size);
          auto *entries = reinterpret_cast<MIB_UDPTABLE_OWNER_PID *>(buffer.data());
          if (GetExtendedUdpTable(entries, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < entries->dwNumEntries; ++i) {
              const auto &row = entries->table[i];
              if (row.dwOwningPid != target_process) continue;
              add_socket(table, "udp", ntohs(static_cast<std::uint16_t>(row.dwLocalPort)), v4_from_long(row.dwLocalAddr));
            }
          }
        }
      }
      {
        DWORD size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER) {
          std::vector<unsigned char> buffer(size);
          auto *entries = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID *>(buffer.data());
          if (GetExtendedUdpTable(entries, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            for (DWORD i = 0; i < entries->dwNumEntries; ++i) {
              const auto &row = entries->table[i];
              if (row.dwOwningPid != target_process) continue;
              ip::address_v6::bytes_type bytes;
              std::memcpy(bytes.data(), row.ucLocalAddr, bytes.size());
              add_socket(table, "udp", ntohs(static_cast<std::uint16_t>(row.dwLocalPort)), ip::address_v6(bytes).to_string());
            }
          }
        }
      }
      return table;
    }
#elif defined(__APPLE__)
    socket_table_t
    own_sockets(std::uint32_t process_id) {
      socket_table_t table;
      const auto target_process = process_id == 0 ? getpid() : static_cast<pid_t>(process_id);
      const int buffer_size = proc_pidinfo(target_process, PROC_PIDLISTFDS, 0, nullptr, 0);
      if (buffer_size <= 0) return table;
      std::vector<proc_fdinfo> descriptors(static_cast<std::size_t>(buffer_size) / sizeof(proc_fdinfo));
      const int used = proc_pidinfo(target_process, PROC_PIDLISTFDS, 0, descriptors.data(), buffer_size);
      if (used <= 0) return table;
      const auto count = static_cast<std::size_t>(used) / sizeof(proc_fdinfo);
      for (std::size_t i = 0; i < count; ++i) {
        if (descriptors[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
        socket_fdinfo info {};
        if (proc_pidfdinfo(target_process, descriptors[i].proc_fd, PROC_PIDFDSOCKETINFO, &info, sizeof(info)) != sizeof(info)) continue;
        if (info.psi.soi_kind == SOCKINFO_TCP) {
          if (info.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN) continue;
          const auto &in = info.psi.soi_proto.pri_tcp.tcpsi_ini;
          const auto port = ntohs(static_cast<std::uint16_t>(in.insi_lport));
          if (in.insi_vflag & INI_IPV4) {
            ip::address_v4::bytes_type bytes;
            std::memcpy(bytes.data(), &in.insi_laddr.ina_46.i46a_addr4, bytes.size());
            add_socket(table, "tcp", port, ip::address_v4(bytes).to_string());
          }
          else {
            ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &in.insi_laddr.ina_6, bytes.size());
            add_socket(table, "tcp", port, ip::address_v6(bytes).to_string());
          }
        }
        else if (info.psi.soi_kind == SOCKINFO_IN) {
          const auto &in = info.psi.soi_proto.pri_in;
          const auto port = ntohs(static_cast<std::uint16_t>(in.insi_lport));
          if (in.insi_vflag & INI_IPV4) {
            ip::address_v4::bytes_type bytes;
            std::memcpy(bytes.data(), &in.insi_laddr.ina_46.i46a_addr4, bytes.size());
            add_socket(table, "udp", port, ip::address_v4(bytes).to_string());
          }
          else {
            ip::address_v6::bytes_type bytes;
            std::memcpy(bytes.data(), &in.insi_laddr.ina_6, bytes.size());
            add_socket(table, "udp", port, ip::address_v6(bytes).to_string());
          }
        }
      }
      return table;
    }
#else
    socket_table_t
    own_sockets(std::uint32_t) {
      return {};
    }
#endif

    void
    apply_sockets(std::vector<port_status_t> &ports, const socket_table_t &table) {
      for (auto &port : ports) {
        const auto &source = port.proto == "tcp" ? table.tcp : table.udp;
        const auto found = source.find(port.port);
        if (found == source.end()) {
          port.listening = false;
          continue;
        }
        port.listening = true;
        port.bound = found->second;
        std::sort(port.bound.begin(), port.bound.end());
      }
    }

    // ---- Windows firewall --------------------------------------------------

#ifdef _WIN32
    template <typename Interface>
    struct com_ptr_t {
      Interface *value = nullptr;
      com_ptr_t() = default;
      com_ptr_t(const com_ptr_t &) = delete;
      com_ptr_t &
      operator=(const com_ptr_t &) = delete;
      ~com_ptr_t() {
        if (value) value->Release();
      }
      Interface **
      put() {
        return &value;
      }
      Interface *
      operator->() const {
        return value;
      }
      explicit
      operator bool() const {
        return value != nullptr;
      }
    };

    const CLSID CLSID_NetFwPolicy2_ = { 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
    const IID IID_INetFwPolicy2_ = { 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
    const IID IID_INetFwRules_ = { 0x9C4C6277, 0x5027, 0x441E, { 0xAF, 0xAE, 0xCA, 0x1F, 0x54, 0x2D, 0xA0, 0x09 } };
    const IID IID_INetFwRule_ = { 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };

    // NET_FW_IP_PROTOCOL_ANY from icftypes.h; not exposed by every mingw-w64 header.
    constexpr long NET_FW_IP_PROTOCOL_ANY_ = 256;

    std::string
    trim(const std::string &value) {
      const auto first = value.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) return {};
      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    std::string
    bstr_to_utf8(BSTR value) {
      if (!value) return {};
      const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
      if (length <= 1) return {};
      std::string text(static_cast<std::size_t>(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), length, nullptr, nullptr);
      text.pop_back();
      return text;
    }

    std::string
    module_path() {
      std::array<wchar_t, 32768> buffer {};
      const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length == 0) return {};
      const auto wide = std::wstring { buffer.data(), length };
      const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (size <= 1) return {};
      std::string text(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(), size, nullptr, nullptr);
      text.pop_back();
      return text;
    }

    std::string
    lowercase(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return value;
    }

    std::string
    basename(const std::string &path) {
      const auto position = path.find_last_of("\\/");
      return position == std::string::npos ? path : path.substr(position + 1);
    }

    bool
    ports_match(const std::string &list, std::uint16_t port) {
      const auto value = trim(list);
      if (value.empty() || value == "*") return true;
      std::size_t start = 0;
      while (start <= value.size()) {
        const auto end = value.find(',', start);
        const auto token = trim(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (token == "*") return true;
        const auto dash = token.find('-');
        try {
          if (dash == std::string::npos) {
            if (static_cast<std::uint16_t>(std::stoul(token)) == port) return true;
          }
          else {
            const auto low = static_cast<std::uint16_t>(std::stoul(token.substr(0, dash)));
            const auto high = static_cast<std::uint16_t>(std::stoul(token.substr(dash + 1)));
            if (port >= low && port <= high) return true;
          }
        }
        catch (...) {
          // Non-numeric entries describe ICMP or a localized keyword; ignore.
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
      return false;
    }

    struct firewall_rule_t {
      bool block = false;
      long profiles = 0;
      long protocol = 0;
      bool program_match = false;
      std::string local_ports;
    };

    void
    apply_windows_firewall(std::vector<port_status_t> &ports) {
      // Rule and profile semantics: ms-inetfwpolicy2-20261103, ms-inetfwrule-20261103,
      // ms-defaultinboundaction-20261103.
      const auto initialize = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      const bool uninitialize = initialize == S_OK || initialize == S_FALSE;
      auto cleanup = util::fail_guard([&]() {
        if (uninitialize) CoUninitialize();
      });
      if (initialize != S_OK && initialize != S_FALSE && initialize != RPC_E_CHANGED_MODE) {
        return;
      }

      com_ptr_t<INetFwPolicy2> policy;
      if (FAILED(CoCreateInstance(CLSID_NetFwPolicy2_, nullptr, CLSCTX_INPROC_SERVER, IID_INetFwPolicy2_,
            reinterpret_cast<void **>(policy.put())))) {
        return;
      }

      long current_profiles = 0;
      if (FAILED(policy->get_CurrentProfileTypes(&current_profiles)) || current_profiles == 0) return;

      long enabled_mask = 0;
      bool default_allow = true;
      bool default_block = true;
      for (const auto profile : { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC }) {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(policy->get_FirewallEnabled(profile, &enabled))) continue;
        if (enabled != VARIANT_TRUE || (current_profiles & profile) == 0) continue;
        enabled_mask |= profile;
        NET_FW_ACTION default_action = NET_FW_ACTION_BLOCK;
        if (FAILED(policy->get_DefaultInboundAction(profile, &default_action))) {
          default_allow = false;
          default_block = false;
          continue;
        }
        default_allow = default_allow && default_action == NET_FW_ACTION_ALLOW;
        default_block = default_block && default_action == NET_FW_ACTION_BLOCK;
      }
      if (enabled_mask == 0) {
        for (auto &port : ports) port.firewall = firewall_e::allowed;
        return;
      }

      com_ptr_t<INetFwRules> rules;
      if (FAILED(policy->get_Rules(rules.put())) || !rules) return;

      const IID IID_IEnumVARIANT_ = { 0x00020404, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
      com_ptr_t<IUnknown> enumerator_unknown;
      if (FAILED(rules->get__NewEnum(enumerator_unknown.put())) || !enumerator_unknown) return;
      com_ptr_t<IEnumVARIANT> enumerator;
      if (FAILED(enumerator_unknown->QueryInterface(IID_IEnumVARIANT_, reinterpret_cast<void **>(enumerator.put()))) || !enumerator) return;

      const auto executable = lowercase(module_path());
      const auto executable_name = basename(executable);
      std::vector<firewall_rule_t> compiled;
      VARIANT item;
      VariantInit(&item);
      ULONG fetched = 0;
      while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        com_ptr_t<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal) {
          item.pdispVal->QueryInterface(IID_INetFwRule_, reinterpret_cast<void **>(rule.put()));
        }
        VariantClear(&item);
        if (!rule) continue;

        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(rule->get_Enabled(&enabled)) || enabled != VARIANT_TRUE) continue;
        NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
        if (FAILED(rule->get_Direction(&direction)) || direction != NET_FW_RULE_DIR_IN) continue;
        NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
        if (FAILED(rule->get_Action(&action))) continue;
        long profiles = 0;
        if (FAILED(rule->get_Profiles(&profiles)) || (profiles & enabled_mask) == 0) continue;
        long protocol = 0;
        if (FAILED(rule->get_Protocol(&protocol))) continue;

        BSTR application = nullptr;
        if (FAILED(rule->get_ApplicationName(&application))) {
          if (application) SysFreeString(application);
          continue;
        }
        const auto application_text = lowercase(bstr_to_utf8(application));
        if (application) SysFreeString(application);
        const bool any_program = application_text.empty();
        const bool program_match = any_program ||
                                   application_text == executable ||
                                   basename(application_text) == executable_name;

        BSTR local_ports = nullptr;
        if (FAILED(rule->get_LocalPorts(&local_ports))) {
          if (local_ports) SysFreeString(local_ports);
          continue;
        }
        auto ports_text = bstr_to_utf8(local_ports);
        if (local_ports) SysFreeString(local_ports);

        compiled.push_back({ action == NET_FW_ACTION_BLOCK, profiles, protocol, program_match, std::move(ports_text) });
      }

      for (auto &port : ports) {
        bool blocked = false;
        bool allowed_all = false;
        bool allowed_partial = false;
        const long protocol = port.proto == "tcp" ? IPPROTO_TCP : IPPROTO_UDP;
        for (const auto &rule : compiled) {
          if (!rule.program_match) continue;
          if (rule.protocol != 0 && rule.protocol != protocol && rule.protocol != NET_FW_IP_PROTOCOL_ANY_) continue;
          if (!ports_match(rule.local_ports, port.port)) continue;
          if (rule.block) {
            blocked = true;
            continue;
          }
          if ((rule.profiles & enabled_mask) == enabled_mask) {
            allowed_all = true;
          }
          else {
            allowed_partial = true;
          }
        }
        if (blocked) {
          port.firewall = firewall_e::blocked;
        }
        else if (allowed_all) {
          port.firewall = firewall_e::allowed;
        }
        else if (default_allow) {
          port.firewall = firewall_e::allowed;
        }
        else if (default_block && !allowed_partial) {
          port.firewall = firewall_e::blocked;
        }
        else {
          port.firewall = firewall_e::unknown;
        }
      }
    }
#else
    void
    apply_windows_firewall(std::vector<port_status_t> &) {
    }
#endif

    // ---- gateway -----------------------------------------------------------

    struct upnp_device_t {
      UPNPDev *value = nullptr;
      ~upnp_device_t() {
        if (value) freeUPNPDevlist(value);
      }
    };

    struct upnp_urls_t {
      UPNPUrls value {};
      ~upnp_urls_t() {
        FreeUPNPUrls(&value);
      }
    };

    int
    query_valid_igd(UPNPDev *device, UPNPUrls *urls, IGDdatas *data, char *lan_address, std::size_t length) {
#if MINIUPNPC_API_VERSION >= 18
      return UPNP_GetValidIGD(device, urls, data, lan_address, static_cast<int>(length), nullptr, 0);
#else
      return UPNP_GetValidIGD(device, urls, data, lan_address, static_cast<int>(length));
#endif
    }

    router_status_t
    query_router(const std::vector<port_status_t> &ports) {
      // WANIPConnection calls: miniupnpc-upnpcommands-2.3.3.
      router_status_t router;
      int error = 0;
      upnp_device_t device { upnpDiscover(1500, nullptr, nullptr, 0, 0, 2, &error) };
      if (!device.value || error) return router;

      IGDdatas data {};
      upnp_urls_t urls;
      std::array<char, 46> lan_address {};
      const auto status = query_valid_igd(device.value, &urls.value, &data, lan_address.data(), lan_address.size());
      if (status != UPNP_CONNECTED_IGD && status != UPNP_PRIVATEIP_IGD) return router;
      if (data.first.servicetype[0] == 0) return router;

      router.protocol = "upnp-igd";
      std::array<char, 64> external_address {};
      if (UPNP_GetExternalIPAddress(urls.value.controlURL, data.first.servicetype, external_address.data()) == UPNPCOMMAND_SUCCESS) {
        if (const auto parsed = parse_igd_external_ip(external_address.data())) {
          router.wan_ip = *parsed;
        }
      }
      router.wan_ip_class = classify_wan_ip(router.wan_ip);

      for (const auto &port : ports) {
        std::array<char, 64> internal_client {};
        std::array<char, 16> internal_port {};
        std::array<char, 128> description {};
        std::array<char, 8> enabled {};
        std::array<char, 16> lease {};
        auto protocol = port.proto;
        std::transform(protocol.begin(), protocol.end(), protocol.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto port_text = std::to_string(port.port);
        const auto error_code = UPNP_GetSpecificPortMappingEntry(
          urls.value.controlURL,
          data.first.servicetype,
          port_text.c_str(),
          protocol.c_str(),
          nullptr,
          internal_client.data(),
          internal_port.data(),
          description.data(),
          enabled.data(),
          lease.data());
        if (const auto mapping = parse_igd_mapping(error_code, internal_client.data(), lease.data(), port.proto, port.port)) {
          router.mappings.push_back(*mapping);
        }
      }
      return router;
    }

    // ---- hairpin -----------------------------------------------------------

    std::string
    read_text_file(const std::string &path) {
      std::ifstream input { path, std::ios::binary };
      if (!input) return {};
      std::ostringstream buffer;
      buffer << input.rdbuf();
      return buffer.str();
    }

    bool
    certificate_matches(X509 *certificate, const std::string &pem) {
      if (!certificate || pem.empty()) return false;
      BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
      if (!bio) return false;
      X509 *local = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);
      if (!local) return false;
      const bool same = X509_cmp(certificate, local) == 0;
      X509_free(local);
      return same;
    }

    bool
    certificate_is_ours(SSL *ssl, const std::string &certificate_path) {
      // Peer certificate identity: rfc8446-20261103 section 4.4.2.
      if (!ssl) return false;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
      X509 *peer = SSL_get1_peer_certificate(ssl);
#else
      X509 *peer = SSL_get_peer_certificate(ssl);
#endif
      if (!peer) return false;
      auto cleanup = util::fail_guard([&]() { X509_free(peer); });
      const auto pem = read_text_file(certificate_path);
      if (pem.empty()) return false;
      return certificate_matches(peer, pem);
    }

    // One TLS handshake against an address and port, bounded to 2.5 seconds in total. The certificate is
    // not verified; `identity_match` says whether it is this host's own. `error` tells a refusal (a host
    // answered) from a timeout (nothing did) and from an address the network cannot route.
    handshake_t
    handshake_to(const std::string &address, std::uint16_t port, const std::string &certificate_path) {
      handshake_t result;
      if (address.empty() || port == 0) {
        result.error = "failed";
        return result;
      }
      const auto started = std::chrono::steady_clock::now();

      try {
        boost::asio::io_context context;
        boost::asio::ssl::context tls { boost::asio::ssl::context::tls_client };
        tls.set_verify_mode(boost::asio::ssl::verify_none);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream { context, tls };
        stream.set_verify_callback([](bool, boost::asio::ssl::verify_context &) { return true; });

        boost::asio::steady_timer timer { context };
        boost::asio::ip::tcp::resolver resolver { context };
        bool connected = false;
        bool timed_out = false;
        std::string error;
        const auto stop_timer = [&]() {
          timer.cancel();
        };

        timer.expires_after(std::chrono::milliseconds(2500));
        timer.async_wait([&](const boost::system::error_code &wait_error) {
          if (wait_error) return;
          timed_out = true;
          boost::system::error_code ignored;
          resolver.cancel();
          stream.next_layer().cancel(ignored);
          stream.next_layer().close(ignored);
        });

        resolver.async_resolve(address, std::to_string(port), [&](const boost::system::error_code &resolve_error, const boost::asio::ip::tcp::resolver::results_type &endpoints) {
          if (resolve_error) {
            error = "failed";
            stop_timer();
            return;
          }
          boost::asio::async_connect(stream.next_layer(), endpoints, [&](const boost::system::error_code &connect_error, const boost::asio::ip::tcp::endpoint &) {
            if (connect_error) {
              error = connect_error == boost::asio::error::connection_refused ? "refused" : "unreachable";
              stop_timer();
              return;
            }
            stream.async_handshake(boost::asio::ssl::stream_base::client, [&](const boost::system::error_code &handshake_error) {
              connected = !handshake_error;
              if (handshake_error) error = "tls_failed";
              stop_timer();
            });
          });
        });

        context.run();
        result.connected = connected;
        result.identity_match = connected && certificate_is_ours(stream.native_handle(), certificate_path);
        if (!connected) result.error = timed_out ? "timeout" : error.empty() ? "failed" : error;
        boost::system::error_code ignored;
        stream.next_layer().close(ignored);
      }
      catch (...) {
        result.connected = false;
        result.identity_match = false;
        result.error = "failed";
      }
      result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      return result;
    }

    hairpin_status_t
    test_hairpin(const std::string &wan_ip, std::uint16_t port, const std::string &certificate_path) {
      hairpin_status_t result;
      result.port = port;
      if (wan_ip.empty() || port == 0) return result;
      result.tried = true;
      const auto attempt = handshake_to(wan_ip, port, certificate_path);
      result.ok = attempt.connected;
      result.identity_match = attempt.identity_match;
      return result;
    }

    // ---- snapshot ----------------------------------------------------------

    snapshot_t
    collect_snapshot(const probe_options_t &options, std::int64_t now) {
      snapshot_t snapshot;
      snapshot.checked_at = format_utc_iso(now);
      snapshot.ports = options.ports;

      apply_sockets(snapshot.ports, own_sockets(options.process_id));
      apply_windows_firewall(snapshot.ports);

      const auto adapters = enumerate_adapters();
      std::string host_public_ip;
      const auto default_routes = default_route_indexes();
      for (const auto &adapter : adapters) {
        if (!adapter.up) continue;
        if (!default_routes.empty()) {
          if (default_routes.find(adapter.index) == default_routes.end()) continue;
        }
        else if (is_virtual_adapter(adapter.name) || is_virtual_adapter(adapter.description)) {
          continue;
        }
        for (const auto &address : adapter.addresses) {
          if (classify_wan_ip(address) != ip_class_e::public_address) continue;
          const auto parsed = boost::asio::ip::make_address(address);
          if (host_public_ip.empty() || (parsed.is_v4() && boost::asio::ip::make_address(host_public_ip).is_v6())) {
            host_public_ip = address;
          }
        }
      }

      mesh_inputs_t mesh_inputs;
      mesh_inputs.ports = snapshot.ports;
      for (const auto &adapter : adapters) {
        const auto kind = classify_adapter(adapter.name, adapter.description, adapter.addresses);
        if (kind == mesh_kind_e::none) continue;
        adapter_t mesh_adapter;
        mesh_adapter.kind = kind;
        mesh_adapter.name = adapter.name;
        mesh_adapter.up = adapter.up;
        for (const auto &address : adapter.addresses) {
          const auto parsed = boost::asio::ip::make_address(address);
          if (parsed.is_v6() && parsed.to_v6().is_link_local()) continue;
          mesh_adapter.addresses.push_back(address);
        }
        if (mesh_adapter.addresses.empty()) continue;
        mesh_inputs.adapters.push_back(std::move(mesh_adapter));
      }

      const auto origin = options.origin_web_ui_allowed.empty() ? "pc"s : options.origin_web_ui_allowed;
      bool origin_allows_mesh = !mesh_inputs.adapters.empty();
      for (const auto &adapter : mesh_inputs.adapters) {
        for (const auto &address : adapter.addresses) {
          origin_allows_mesh = origin_allows_mesh && origin_allows(origin, address);
        }
      }
      mesh_inputs.origin_allows_mesh = origin_allows_mesh;

      const auto evidence = observed_evidence(now);
      mesh_inputs.evidence = evidence;

      public_inputs_t public_inputs;
      public_inputs.ports = snapshot.ports;
      public_inputs.host_public_ip = host_public_ip;
      public_inputs.web_ui_exposed_to_wan = origin == "wan";
      public_inputs.evidence = evidence;

      const auto router = query_router(snapshot.ports);
      router_status_t router_status = router;

      // The endpoints in force (external_endpoints, or the legacy external_ip), probed in parallel.
      const auto endpoint_list = public_address::effective_endpoints(options.external_endpoints, options.external_ip);
      const auto default_http_port = options.default_http_port != 0 ? options.default_http_port : static_cast<std::uint16_t>(options.hairpin_port + public_address::HTTPS_PORT_OFFSET);
      endpoint_prober_t prober;
      prober.resolve = [](const std::string &host) { return resolve_host(host, std::chrono::seconds { 2 }); };
      prober.handshake = [&options](const std::string &address, std::uint16_t port) { return handshake_to(address, port, options.certificate_path); };
      public_inputs.endpoints = probe_endpoints(endpoint_list.endpoints, default_http_port, router_status.wan_ip, prober);

      // The first endpoint also fills the single-address fields older clients of the API read.
      configured_address_t configured;
      if (!public_inputs.endpoints.empty()) {
        const auto &first = public_inputs.endpoints.front();
        configured = evaluate_configured_address(first.host, first.kind == public_address::kind_e::hostname ? first.resolved : std::vector<std::string> {}, router_status.wan_ip);
      }
      else {
        configured = evaluate_configured_address({}, {}, router_status.wan_ip);
      }

      // The hairpin field reports the first public endpoint; without one, the router's own outside address.
      const auto hairpin_port = options.hairpin_port;
      hairpin_status_t hairpin;
      const endpoint_result_t *shown = nullptr;
      for (const auto &endpoint : public_inputs.endpoints) {
        if (endpoint.scope != scope_e::public_address || endpoint.attempts.empty()) continue;
        if (!shown || (endpoint.reached && !shown->reached)) shown = &endpoint;
      }
      if (shown) {
        const auto &attempt = *std::find_if(shown->attempts.begin(), shown->attempts.end(), [&](const endpoint_attempt_t &candidate) { return candidate.address == shown->tested_address; });
        hairpin.tried = true;
        hairpin.ok = attempt.handshake.connected;
        hairpin.identity_match = attempt.handshake.identity_match;
        hairpin.port = shown->https_port;
        hairpin.target = attempt.address;
      }
      else if (classify_wan_ip(router_status.wan_ip) == ip_class_e::public_address) {
        hairpin = test_hairpin(router_status.wan_ip, hairpin_port, options.certificate_path);
        hairpin.target = router_status.wan_ip;
      }
      else {
        hairpin.port = hairpin_port;
      }
      public_inputs.router = router_status;
      public_inputs.hairpin = hairpin;
      public_inputs.configured = configured;

      snapshot.public_reachability = evaluate_public(public_inputs, now);
      snapshot.mesh = evaluate_mesh(mesh_inputs, now);
      return snapshot;
    }

    std::mutex cache_mutex;
    snapshot_t cached_snapshot;
    std::int64_t cached_at = 0;
    std::string cache_key;

    std::string
    options_key(const probe_options_t &options) {
      std::ostringstream key;
      for (const auto &port : options.ports) {
        key << port.proto << '/' << port.port << '/' << port.role << ';';
      }
      key << options.certificate_path << '|' << options.origin_web_ui_allowed << '|' << options.hairpin_port << '|' << options.process_id;
      key << '|' << options.external_ip << '|' << options.external_endpoints << '|' << options.default_http_port;
      return key.str();
    }
  }  // namespace

  snapshot_t
  probe(const probe_options_t &options, bool refresh) {
    std::lock_guard lock { cache_mutex };
    const auto now = now_epoch_seconds();
    const auto key = options_key(options);
    if (!refresh && cached_at != 0 && key == cache_key && now - cached_at < CACHE_TTL_SECONDS) {
      return cached_snapshot;
    }
    cached_snapshot = collect_snapshot(options, now);
    cached_at = now;
    cache_key = key;
    return cached_snapshot;
  }
}  // namespace reachability
