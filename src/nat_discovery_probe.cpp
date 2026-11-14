/**
 * @file src/nat_discovery_probe.cpp
 * @brief Host side of NAT discovery: interface enumeration and the STUN exchanges.
 *
 * Unlike the reachability check, this asks external STUN servers, and only when the network
 * overview is requested. Each server gets a Binding request over UDP; retransmission follows
 * RFC 8489 section 6.2.1 with RTO 500 ms and Rc reduced to 3 for an interactive page (sends at
 * 0, 500 and 1500 ms, the transaction fails at 2500 ms). All servers of a family are asked in
 * parallel from one socket, so the mapping tests of RFC 5780 section 4.3 run on the same
 * local port as test I.
 */
#include "nat_discovery.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/v6_only.hpp>

#include "public_address.h"
#include "utility.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <windows.h>
#else
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

using namespace std::literals;

namespace nat_discovery {
  namespace {
    namespace ip = boost::asio::ip;
    using clock = std::chrono::steady_clock;

    constexpr auto RESOLVE_BUDGET = 2s;
    constexpr auto RTO = 500ms;
    constexpr int RC = 3;
    constexpr auto FINAL_WAIT = 1000ms;  ///< after the last transmission (Rm = 2 at RTO 500 ms)

#ifndef _WIN32
    int
    prefix_from_mask(const sockaddr *mask) {
      if (!mask || mask->sa_family != AF_INET) return 0;
      std::uint32_t value = ntohl(reinterpret_cast<const sockaddr_in *>(mask)->sin_addr.s_addr);
      int bits = 0;
      while (value & 0x80000000u) {
        ++bits;
        value <<= 1;
      }
      return bits;
    }
#endif

    std::string
    address_text(const sockaddr *address) {
      if (address->sa_family == AF_INET) {
        ip::address_v4::bytes_type bytes;
        std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in *>(address)->sin_addr, bytes.size());
        return ip::address_v4(bytes).to_string();
      }
      ip::address_v6::bytes_type bytes;
      std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr, bytes.size());
      return ip::address_v6(bytes).to_string();
    }

    struct resolve_state_t {
      std::mutex mutex;
      std::condition_variable ready;
      std::size_t pending = 0;
      std::vector<std::vector<std::string>> results;
    };

    // getaddrinfo() has no timeout; every lookup runs on its own thread and the probe waits a bounded time.
    std::vector<std::vector<std::string>>
    resolve_all(const std::vector<server_t> &servers, int family) {
      auto state = std::make_shared<resolve_state_t>();
      state->results.resize(servers.size());
      state->pending = servers.size();
      for (std::size_t i = 0; i < servers.size(); ++i) {
        std::thread { [state, i, host = servers[i].host, family]() {
          addrinfo hints {};
          hints.ai_family = family == 6 ? AF_INET6 : AF_INET;
          hints.ai_socktype = SOCK_DGRAM;
          addrinfo *result = nullptr;
          std::vector<std::string> addresses;
          if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0) {
            for (auto *entry = result; entry; entry = entry->ai_next) {
              if (!entry->ai_addr || entry->ai_family != hints.ai_family) continue;
              auto text = address_text(entry->ai_addr);
              if (std::find(addresses.begin(), addresses.end(), text) == addresses.end()) addresses.push_back(std::move(text));
            }
            freeaddrinfo(result);
          }
          std::lock_guard lock { state->mutex };
          state->results[i] = std::move(addresses);
          --state->pending;
          state->ready.notify_all();
        } }.detach();
      }
      std::unique_lock lock { state->mutex };
      state->ready.wait_for(lock, RESOLVE_BUDGET, [&]() { return state->pending == 0; });
      return state->results;
    }

    struct transaction_t {
      ip::udp::endpoint target;
      transaction_id_t id {};
      std::vector<std::uint8_t> request;
      int sent = 0;
      clock::time_point first_sent;
      clock::time_point next_send;
      bool done = false;
      bool send_failed = false;
      std::optional<binding_response_t> response;
      std::int64_t rtt_ms = 0;
    };

    transaction_t
    make_transaction(const ip::udp::endpoint &target, std::mt19937_64 &random) {
      transaction_t transaction;
      transaction.target = target;
      for (auto &byte : transaction.id) byte = static_cast<std::uint8_t>(random());
      transaction.request = build_binding_request(transaction.id);
      return transaction;
    }

    /// Run the transactions in parallel on one socket until each is answered or has timed out.
    void
    exchange(ip::udp::socket &socket, std::vector<transaction_t> &transactions) {
      const auto start = clock::now();
      for (auto &transaction : transactions) transaction.next_send = start;
      std::array<std::uint8_t, 1500> buffer {};
      while (true) {
        const auto now = clock::now();
        bool open = false;
        for (auto &transaction : transactions) {
          if (transaction.done) continue;
          if (transaction.sent < RC && now >= transaction.next_send) {
            boost::system::error_code error;
            socket.send_to(boost::asio::buffer(transaction.request), transaction.target, 0, error);
            if (transaction.sent == 0) transaction.first_sent = now;
            ++transaction.sent;
            if (error) transaction.send_failed = true;
            // RTO doubles after each transmission: 0, 500, 1500 ms.
            transaction.next_send = now + RTO * (1 << (transaction.sent - 1));
          }
          const auto expires = transaction.sent >= RC ? transaction.next_send - RTO * (1 << (RC - 1)) + FINAL_WAIT : clock::time_point::max();
          if (transaction.sent >= RC && now >= expires) {
            transaction.done = true;
            continue;
          }
          open = true;
        }
        if (!open) return;

        boost::system::error_code error;
        ip::udp::endpoint sender;
        const auto length = socket.receive_from(boost::asio::buffer(buffer), sender, 0, error);
        if (error == boost::asio::error::would_block || error == boost::asio::error::try_again) {
          std::this_thread::sleep_for(5ms);
          continue;
        }
        if (error) {
          // An ICMP error reported on the socket (for example port unreachable); keep waiting for the others.
          std::this_thread::sleep_for(5ms);
          continue;
        }
        for (auto &transaction : transactions) {
          if (transaction.done || sender != transaction.target) continue;
          auto parsed = parse_binding_response(buffer.data(), length, transaction.id);
          if (!parsed) continue;
          transaction.response = std::move(parsed);
          transaction.done = true;
          transaction.rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - transaction.first_sent).count();
          break;
        }
      }
    }

    ip::udp::endpoint
    endpoint_of(const std::string &address, std::uint16_t port) {
      return { ip::make_address(address), port };
    }

    std::string
    target_text(const transport_address_t &value) {
      return format_transport(value);
    }

    /// `bind_address`: the source to test from (the stable global IPv6 address); empty lets the routing table choose,
    /// which for IPv6 picks a temporary address (RFC 6724 rule 7) that clients must not be given.
    family_discovery_t
    discover_family(int family, const std::vector<server_t> &servers, const std::vector<std::string> &local_addresses, const std::string &bind_address) {
      family_discovery_t result;
      result.family = family;
      const auto resolved = resolve_all(servers, family);

      std::vector<std::size_t> asked;  // index into servers of each transaction
      std::optional<ip::udp::endpoint> first_target;
      for (std::size_t i = 0; i < servers.size(); ++i) {
        server_observation_t observation;
        observation.server = servers[i].host.find(':') != std::string::npos ? "["s + servers[i].host + "]:" + std::to_string(servers[i].port) : servers[i].host + ":" + std::to_string(servers[i].port);
        if (resolved[i].empty()) {
          // An IP literal of the other family is not a lookup failure.
          const auto scope_literal = public_address::classify(servers[i].host) == public_address::kind_e::ip;
          const bool v6_literal = servers[i].host.find(':') != std::string::npos;
          observation.error = scope_literal && v6_literal != (family == 6) ? "other_family" : "resolve_failed";
        }
        else {
          observation.address = resolved[i].front();
          if (is_fake_ip(observation.address) && std::find(result.notes.begin(), result.notes.end(), "dns_fake_ip") == result.notes.end()) {
            result.notes.push_back("dns_fake_ip");
          }
          if (!first_target) first_target = endpoint_of(observation.address, servers[i].port);
        }
        result.servers.push_back(std::move(observation));
      }
      if (!first_target) return result;

      boost::asio::io_context context;
      const auto protocol = family == 6 ? ip::udp::v6() : ip::udp::v4();
      boost::system::error_code error;
      ip::udp::endpoint local_endpoint { protocol, 0 };
      if (!bind_address.empty()) {
        const auto parsed = ip::make_address(bind_address, error);
        if (!error && parsed.is_v6() == (family == 6)) local_endpoint = { parsed, 0 };
        error.clear();
      }

      // The source address the routing table picks towards the internet; connect() on UDP sends nothing.
      {
        ip::udp::socket route { context };
        route.open(protocol, error);
        if (!error && local_endpoint.address() != ip::address {}) {
          route.bind(local_endpoint, error);
          if (error) {
            local_endpoint = { protocol, 0 };
            error.clear();
          }
        }
        if (!error) route.connect(*first_target, error);
        if (error) {
          for (auto &observation : result.servers) {
            if (observation.error.empty()) observation.error = "no_route";
          }
          return result;
        }
        result.local_address = route.local_endpoint(error).address().to_string();
      }

      ip::udp::socket socket { context };
      socket.open(protocol, error);
      if (!error && family == 6) socket.set_option(ip::v6_only(true), error);
      if (!error) socket.bind(local_endpoint, error);
      if (!error) socket.non_blocking(true, error);
      if (error) return result;
      result.socket_ok = true;
      const auto local_port = socket.local_endpoint(error).port();

      std::random_device device;
      std::mt19937_64 random { (static_cast<std::uint64_t>(device()) << 32) ^ device() };

      std::vector<transaction_t> transactions;
      for (std::size_t i = 0; i < servers.size(); ++i) {
        if (result.servers[i].address.empty()) continue;
        transactions.push_back(make_transaction(endpoint_of(result.servers[i].address, servers[i].port), random));
        asked.push_back(i);
      }
      exchange(socket, transactions);

      for (std::size_t t = 0; t < transactions.size(); ++t) {
        auto &observation = result.servers[asked[t]];
        const auto &transaction = transactions[t];
        if (!transaction.response) {
          observation.error = transaction.send_failed ? "send_failed" : "timeout";
          continue;
        }
        if (!transaction.response->success || !transaction.response->mapped) {
          observation.error = "error_response";
          continue;
        }
        observation.answered = true;
        observation.mapped = transaction.response->mapped;
        observation.other = transaction.response->other;
        observation.rtt_ms = transaction.rtt_ms;
      }
      summarize_family(result, local_addresses);

      // RFC 5780 section 4.3 against the first server that returns a usable OTHER-ADDRESS.
      mapping_inputs_t inputs;
      inputs.local = transport_address_t { result.local_address, local_port };
      inputs.local_addresses = local_addresses;
      const server_observation_t *primary = nullptr;
      std::uint16_t primary_port = 0;
      for (std::size_t i = 0; i < result.servers.size(); ++i) {
        const auto &observation = result.servers[i];
        if (!observation.answered || !observation.other) continue;
        if (observation.other->address == observation.address || observation.other->address.empty()) continue;
        if (classify_address(observation.other->address) != address_scope_e::global) continue;
        primary = &observation;
        primary_port = servers[i].port;
        break;
      }
      const server_observation_t *first_answer = nullptr;
      const server_observation_t *second_answer = nullptr;
      for (const auto &observation : result.servers) {
        if (!observation.answered) continue;
        if (!first_answer) first_answer = &observation;
        else if (!second_answer && observation.address != first_answer->address) second_answer = &observation;
      }

      if (primary) {
        inputs.method = method_e::rfc5780;
        inputs.test1 = primary->mapped;
        result.tests.push_back({ "I", primary->address + ":" + std::to_string(primary_port), primary->mapped });
        const auto first = classify_mapping(inputs);
        if (first.nat_present.value_or(false)) {
          const transport_address_t test2_target { primary->other->address, primary_port };
          std::vector<transaction_t> second;
          second.push_back(make_transaction(endpoint_of(test2_target.address, test2_target.port), random));
          exchange(socket, second);
          if (second[0].response && second[0].response->success) inputs.test2 = second[0].response->mapped;
          result.tests.push_back({ "II", target_text(test2_target), inputs.test2 });
          inputs.test3_possible = primary->other->port != primary_port;
          if (inputs.test2 && *inputs.test2 != *inputs.test1 && inputs.test3_possible) {
            std::vector<transaction_t> third;
            third.push_back(make_transaction(endpoint_of(primary->other->address, primary->other->port), random));
            exchange(socket, third);
            if (third[0].response && third[0].response->success) inputs.test3 = third[0].response->mapped;
            result.tests.push_back({ "III", target_text(*primary->other), inputs.test3 });
          }
        }
      }
      else if (first_answer && second_answer) {
        inputs.method = method_e::two_servers;
        inputs.test1 = first_answer->mapped;
        inputs.test2 = second_answer->mapped;
        result.tests.push_back({ "I", first_answer->server, first_answer->mapped });
        result.tests.push_back({ "II", second_answer->server, second_answer->mapped });
      }
      else if (first_answer) {
        inputs.method = method_e::none;
        inputs.test1 = first_answer->mapped;
        result.tests.push_back({ "I", first_answer->server, first_answer->mapped });
      }
      result.mapping = classify_mapping(inputs);
      return result;
    }

#ifdef __linux__
    std::map<std::string, if_inet6_row_t>
    read_if_inet6() {
      std::map<std::string, if_inet6_row_t> rows;
      std::ifstream file { "/proc/net/if_inet6" };
      std::string line;
      while (std::getline(file, line)) {
        auto row = parse_if_inet6_line(line);
        if (row) rows[row->address] = std::move(*row);
      }
      return rows;
    }
#endif

    std::mutex cache_mutex;
    discovery_t cached;
    std::int64_t cached_at = 0;
    std::string cache_key;
  }  // namespace

  std::string
  host_time_zone() {
#ifdef _WIN32
    DYNAMIC_TIME_ZONE_INFORMATION info {};
    if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) return {};
    std::string name;
    for (const wchar_t *c = info.TimeZoneKeyName; *c; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    return name;
#else
    if (const char *tz = std::getenv("TZ"); tz && *tz) return tz[0] == ':' ? std::string { tz + 1 } : std::string { tz };
    std::error_code error;
    const auto target = std::filesystem::read_symlink("/etc/localtime", error);
    if (!error) return target.string();
    std::ifstream file { "/etc/timezone" };
    std::string name;
    std::getline(file, name);
    return name;
#endif
  }

  std::vector<interface_t>
  enumerate_interfaces() {
    std::vector<interface_t> result;
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
    // GetAdaptersAddresses: ms-getadaptersaddresses-20261103; unicast fields: ms-ip-adapter-unicast-20261104.
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
      status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
      if (status != ERROR_BUFFER_OVERFLOW) break;
      buffer.resize(size);
    }
    if (status != NO_ERROR) return result;
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); adapter; adapter = adapter->Next) {
      if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      interface_t entry;
      entry.index = adapter->IfIndex;
      entry.name = adapter->FriendlyName ? wide_to_utf8(adapter->FriendlyName) : std::string(adapter->AdapterName);
      entry.description = wide_to_utf8(adapter->Description);
      entry.up = adapter->OperStatus == IfOperStatusUp;
      for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
        const auto *address = unicast->Address.lpSockaddr;
        if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) continue;
        interface_address_t item;
        item.address = address_text(address);
        item.family = address->sa_family == AF_INET6 ? 6 : 4;
        item.prefix_length = unicast->OnLinkPrefixLength <= 128 ? unicast->OnLinkPrefixLength : 0;
        item.scope = classify_address(item.address);
        if (item.family == 6) {
          item.lifetime = lifetime_from_windows(static_cast<int>(unicast->SuffixOrigin), unicast->Flags);
          item.deprecated = unicast->DadState == IpDadStateDeprecated || unicast->PreferredLifetime == 0;
        }
        entry.addresses.push_back(std::move(item));
      }
      if (!entry.addresses.empty()) result.push_back(std::move(entry));
    }
#else
    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return result;
    auto cleanup = util::fail_guard([&]() { freeifaddrs(addresses); });
  #ifdef __linux__
    const auto inet6 = read_if_inet6();
  #endif
    std::map<std::string, std::size_t> index;
    for (auto *entry = addresses; entry; entry = entry->ifa_next) {
      if (!entry->ifa_addr || (entry->ifa_flags & IFF_LOOPBACK)) continue;
      const auto family = entry->ifa_addr->sa_family;
      if (family != AF_INET && family != AF_INET6) continue;
      const std::string name { entry->ifa_name ? entry->ifa_name : "" };
      auto found = index.find(name);
      if (found == index.end()) {
        found = index.emplace(name, result.size()).first;
        interface_t item;
        item.name = name;
        item.index = if_nametoindex(name.c_str());
        result.push_back(std::move(item));
      }
      auto &target = result[found->second];
      target.up = target.up || (entry->ifa_flags & IFF_UP) != 0;
      interface_address_t item;
      item.address = address_text(entry->ifa_addr);
      item.family = family == AF_INET6 ? 6 : 4;
      item.scope = classify_address(item.address);
      if (family == AF_INET) {
        item.prefix_length = static_cast<std::uint8_t>(prefix_from_mask(entry->ifa_netmask));
      }
  #ifdef __linux__
      else {
        // Linux: IFA_F_TEMPORARY and IFA_F_DEPRECATED (linux-if-addr-h-v6.12). Without the temporary flag a
        // global address is a stable one: EUI-64, RFC 7217 stable privacy, DHCPv6 or manual.
        const auto row = inet6.find(item.address);
        if (row != inet6.end()) {
          item.prefix_length = row->second.prefix_length;
          item.lifetime = (row->second.flags & IFA_TEMPORARY) ? lifetime_e::temporary : lifetime_e::stable;
          item.deprecated = (row->second.flags & IFA_DEPRECATED) != 0;
        }
      }
  #endif
      target.addresses.push_back(std::move(item));
    }
#endif
    for (auto &entry : result) {
      entry.is_virtual = is_virtual_adapter(entry.name) || is_virtual_adapter(entry.description);
      std::vector<std::string> texts;
      for (const auto &address : entry.addresses) texts.push_back(address.address);
      entry.mesh = reachability::classify_adapter(entry.name, entry.description, texts);
    }
    return result;
  }

  discovery_t
  discover(const discover_options_t &options, bool refresh) {
    std::lock_guard lock { cache_mutex };
    const auto now = reachability::now_epoch_seconds();
    const auto key = (options.enabled ? "on|"s : "off|"s) + options.region + "|" + join_servers(options.servers);
    if (!refresh && cached_at != 0 && key == cache_key && now - cached_at < CACHE_TTL_SECONDS) {
      return cached;
    }

    discovery_t discovery;
    discovery.checked_at = reachability::format_utc_iso(now);
    discovery.enabled = options.enabled;
    discovery.region = options.region;
    discovery.interfaces = enumerate_interfaces();
    discovery.ipv4.family = 4;
    discovery.ipv6.family = 6;

    std::vector<std::string> local_addresses;
    for (const auto &entry : discovery.interfaces) {
      for (const auto &address : entry.addresses) local_addresses.push_back(address.address);
    }

    if (options.enabled && !options.servers.empty()) {
      std::vector<server_t> servers = options.servers;
      if (servers.size() > MAX_SERVERS) servers.resize(MAX_SERVERS);
      // IPv6 is tested from the stable global address, the one the overview shows; IPv4 from what the routing table picks.
      const auto stable_v6 = preferred_global(discovery.interfaces, 6);
      family_discovery_t v6;
      std::thread worker { [&]() { v6 = discover_family(6, servers, local_addresses, stable_v6); } };
      discovery.ipv4 = discover_family(4, servers, local_addresses, {});
      worker.join();
      discovery.ipv6 = std::move(v6);
      // The servers this run sent a datagram to, for the page and the privacy note.
      for (const auto *family : { &discovery.ipv4, &discovery.ipv6 }) {
        for (const auto &server : family->servers) {
          if (server.address.empty() || server.error == "no_route") continue;
          if (std::find(discovery.contacted.begin(), discovery.contacted.end(), server.server) == discovery.contacted.end()) discovery.contacted.push_back(server.server);
        }
      }
    }

    // Mark the interfaces the host leaves through.
    for (auto &entry : discovery.interfaces) {
      for (const auto &address : entry.addresses) {
        if (!discovery.ipv4.local_address.empty() && address.address == discovery.ipv4.local_address) entry.egress_v4 = true;
        if (!discovery.ipv6.local_address.empty() && address.address == discovery.ipv6.local_address) entry.egress_v6 = true;
      }
    }

    cached = discovery;
    cached_at = now;
    cache_key = key;
    return cached;
  }
}  // namespace nat_discovery
