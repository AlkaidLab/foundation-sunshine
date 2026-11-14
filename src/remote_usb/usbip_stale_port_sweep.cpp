/**
 * @file src/remote_usb/usbip_stale_port_sweep.cpp
 * @brief Remove native loopback imports left by an exited host process.
 *
 * usbip-win2 attaches a native device bridge (the DualSense, the virtual
 * microphone) on a loopback port.  When
 * the host exits abnormally, the virtual hub can keep the imported node even
 * though the bridge listener is gone.  The next service start must release
 * that port before it tries to attach a new lease.
 *
 * The selector deliberately recognizes only the native bridge identity: the
 * product advertises busid 1-1 with remote bus/device 001/001 on loopback.
 * A real controller forwarded from a client has a different remote device
 * number, and endpoints owned by this process are excluded explicitly.
 *
 * Sources: usbip-win2-port-cmd-v0.9.8.1.cpp (listing format),
 * usbip-win2-wsk-disconnect-v0.9.8.1.cpp and
 * usbip-win2-receive-irp-v0.9.8.1.cpp (socket-close behaviour),
 * usbip-win2-device-detach-v0.9.8.1.cpp (port reclaim and deletion), and
 * usbip-win2-reattach-wiki-v0.9.8.1.md (driver reattach defaults).
 */

#include "remote_usb_host_controller.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

namespace remote_usb {
  namespace {

    using namespace std::string_view_literals;

    constexpr std::uint16_t kDs5Vendor = 0x054c;
    constexpr std::uint16_t kDs5Product = 0x0ce6;
    constexpr std::string_view kDs5Busid = "1-1"sv;
    constexpr std::uint16_t kDs5RemoteBus = 1;
    constexpr std::uint16_t kDs5RemoteDevice = 1;

    std::mutex protected_endpoints_mutex;
    std::vector<endpoint> registered_protected_endpoints;

    std::string_view
    trim(std::string_view value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
      }
      return value;
    }

    bool
    parse_number(std::string_view value, unsigned base, std::uint16_t &result) {
      value = trim(value);
      if (value.empty()) {
        return false;
      }
      std::uint32_t parsed = 0;
      const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
      if (conversion.ec != std::errc {} || conversion.ptr != value.data() + value.size() ||
          parsed > 0xffffu) {
        return false;
      }
      result = static_cast<std::uint16_t>(parsed);
      return true;
    }

    bool
    parse_port_line(std::string_view line, std::uint16_t &port) {
      constexpr std::string_view prefix = "Port ";
      if (line.substr(0, prefix.size()) != prefix) {
        return false;
      }
      line.remove_prefix(prefix.size());
      const auto colon = line.find(':');
      if (colon == std::string_view::npos) {
        return false;
      }
      return parse_number(line.substr(0, colon), 10, port) && port != 0;
    }

    bool
    parse_product_ids(std::string_view line, std::uint16_t &vendor, std::uint16_t &product) {
      const auto open = line.rfind('(');
      if (open == std::string_view::npos) {
        return false;
      }
      const auto close = line.find(')', open);
      if (close == std::string_view::npos) {
        return false;
      }
      const auto pair = line.substr(open + 1, close - open - 1);
      if (pair.size() != 9 || pair[4] != ':') {
        return false;
      }
      return parse_number(pair.substr(0, 4), 16, vendor) &&
             parse_number(pair.substr(5, 4), 16, product);
    }

    bool
    is_loopback_host(std::string_view host) {
      host = trim(host);
      if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host.remove_prefix(1);
        host.remove_suffix(1);
      }
      if (host.empty()) {
        return false;
      }

      std::string lower(host);
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
      if (lower == "localhost") {
        return true;
      }

      boost::system::error_code error;
      const auto address = boost::asio::ip::make_address(std::string { host }, error);
      return !error && address.is_loopback();
    }

    bool
    parse_usbip_url(std::string_view line, usbip_imported_port &port) {
      constexpr std::string_view scheme = "usbip://";
      const auto scheme_at = line.find(scheme);
      if (scheme_at == std::string_view::npos) {
        return false;
      }
      auto rest = line.substr(scheme_at + scheme.size());
      const auto slash = rest.find('/');
      if (slash == std::string_view::npos) {
        return false;
      }

      const auto authority = trim(rest.substr(0, slash));
      const auto busid = trim(rest.substr(slash + 1));
      if (authority.empty() || busid.empty()) {
        return false;
      }

      std::string_view host;
      std::string_view service;
      if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close + 1 >= authority.size() ||
            authority[close + 1] != ':') {
          return false;
        }
        host = authority.substr(1, close - 1);
        service = authority.substr(close + 2);
      }
      else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
          return false;
        }
        host = authority.substr(0, colon);
        service = authority.substr(colon + 1);
      }

      std::uint16_t service_port = 0;
      if (!parse_number(service, 10, service_port) || service_port == 0) {
        return false;
      }

      port.server_address.assign(host);
      port.server_port = service_port;
      port.busid.assign(busid);
      port.loopback_server = is_loopback_host(host);
      return true;
    }

    bool
    parse_remote_bus_device(std::string_view line, usbip_imported_port &port) {
      constexpr std::string_view marker = "remote bus/dev:";
      const auto at = line.find(marker);
      if (at == std::string_view::npos) {
        return false;
      }
      auto value = trim(line.substr(at + marker.size()));
      const auto slash = value.find('/');
      if (slash == std::string_view::npos) {
        return false;
      }
      return parse_number(value.substr(0, slash), 10, port.remote_bus) &&
             parse_number(value.substr(slash + 1), 10, port.remote_device);
    }

    bool
    is_native_port(const usbip_imported_port &port, const stale_port_identity &identity) {
      return port.hub_port != 0 && port.vendor_id == identity.vendor_id &&
             port.product_id == identity.product_id && port.loopback_server &&
             port.busid == identity.busid && port.remote_bus == identity.remote_bus &&
             port.remote_device == identity.remote_device;
    }

    bool
    is_protected(const usbip_imported_port &port, std::span<const endpoint> protected_endpoints) {
      return std::any_of(protected_endpoints.begin(), protected_endpoints.end(),
        [&port](const endpoint &protected_endpoint) {
          if (protected_endpoint.port != port.server_port ||
              protected_endpoint.busid != port.busid) {
            return false;
          }
          if (protected_endpoint.address == port.server_address) {
            return true;
          }
          return port.loopback_server && is_loopback_host(protected_endpoint.address);
        });
    }

    usbip_host_status
    command_failure_status(const usbip_command_result &command) {
      if (command.cancelled) {
        return usbip_host_status::cancelled;
      }
      if (command.timed_out) {
        return usbip_host_status::timed_out;
      }
      if (command.exit_code < 0) {
        return usbip_host_status::launch_failed;
      }
      return usbip_host_status::sweep_failed;
    }

    std::string
    command_detail(const usbip_command_result &command) {
      if (!command.standard_error.empty()) {
        return command.standard_error;
      }
      if (command.timed_out) {
        return "usbip port timed out";
      }
      if (command.exit_code < 0) {
        return "unable to launch usbip helper";
      }
      return "usbip port returned a failure";
    }

  }  // namespace

  std::vector<usbip_imported_port>
  parse_usbip_port_output(std::string_view output) {
    std::vector<usbip_imported_port> ports;
    usbip_imported_port current;
    bool in_port = false;

    const auto finish = [&] {
      if (in_port && current.hub_port != 0) {
        ports.push_back(std::move(current));
      }
      current = {};
      in_port = false;
    };

    std::size_t start = 0;
    for (;;) {
      const auto end = output.find('\n', start);
      auto line = output.substr(start, end == std::string_view::npos ?
                                         std::string_view::npos :
                                         end - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      const auto cleaned = trim(line);

      std::uint16_t port = 0;
      if (parse_port_line(cleaned, port)) {
        finish();
        current.hub_port = port;
        in_port = true;
      }
      else if (in_port) {
        if (!parse_usbip_url(cleaned, current) &&
            !parse_remote_bus_device(cleaned, current) &&
            !current.vendor_id && !current.product_id) {
          parse_product_ids(cleaned, current.vendor_id, current.product_id);
        }
      }

      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    finish();
    return ports;
  }

  stale_port_identity
  ds5_stale_port_identity() {
    return { kDs5Vendor, kDs5Product, std::string { kDs5Busid }, kDs5RemoteBus, kDs5RemoteDevice };
  }

  std::vector<usbip_imported_port>
  select_stale_ports(std::span<const usbip_imported_port> ports,
    std::span<const endpoint> protected_endpoints, const stale_port_identity &identity) {
    std::vector<usbip_imported_port> stale;
    for (const auto &port : ports) {
      if (is_native_port(port, identity) && !is_protected(port, protected_endpoints)) {
        stale.push_back(port);
      }
    }
    return stale;
  }

  std::vector<usbip_imported_port>
  select_stale_ds5_ports(std::span<const usbip_imported_port> ports,
    std::span<const endpoint> protected_endpoints) {
    return select_stale_ports(ports, protected_endpoints, ds5_stale_port_identity());
  }

  void
  remember_protected_endpoint(const endpoint &value) {
    std::lock_guard lock(protected_endpoints_mutex);
    if (std::find(registered_protected_endpoints.begin(), registered_protected_endpoints.end(), value) ==
        registered_protected_endpoints.end()) {
      registered_protected_endpoints.push_back(value);
    }
  }

  void
  forget_protected_endpoint(const endpoint &value) {
    std::lock_guard lock(protected_endpoints_mutex);
    registered_protected_endpoints.erase(
      std::remove(registered_protected_endpoints.begin(), registered_protected_endpoints.end(), value),
      registered_protected_endpoints.end());
  }

  usbip_sweep_result
  usbip_host_controller::sweep_stale_ds5_ports() {
    return sweep_stale_ports(ds5_stale_port_identity());
  }

  usbip_sweep_result
  usbip_host_controller::sweep_stale_ports(const stale_port_identity &identity) {
    std::lock_guard sweep_lock(sweep_mutex_);

    usbip_sweep_result result;
    if (!backend_supported()) {
      result.status = usbip_host_status::unsupported;
      result.detail = "no compatible host-side USB/IP backend is enabled";
      return result;
    }
    if (stopped()) {
      result.status = usbip_host_status::stopped;
      result.detail = "usbip host controller is stopped";
      return result;
    }

    std::vector<endpoint> excluded_endpoints;
    {
      std::lock_guard lock(mutex_);
      excluded_endpoints.reserve(accepted_bindings_.size() + operations_.size());
      for (const auto &binding : accepted_bindings_) {
        excluded_endpoints.push_back(binding.server_endpoint);
      }
      for (const auto &operation : operations_) {
        if (operation->kind == operation_kind::attach &&
            !operation->command_finished.load(std::memory_order_acquire)) {
          excluded_endpoints.push_back(operation->request.server_endpoint);
        }
      }
      {
        std::lock_guard protected_lock(protected_endpoints_mutex);
        excluded_endpoints.insert(excluded_endpoints.end(),
          registered_protected_endpoints.begin(), registered_protected_endpoints.end());
      }
    }

    auto cancel = std::make_shared<std::atomic_bool>(false);
    usbip_command_result listing;
    try {
      listing = config_.command_runner(config_.executable, { "port" },
        config_.attach_timeout, cancel, config_.max_output_bytes);
    }
    catch (const std::exception &exception) {
      result.status = usbip_host_status::sweep_failed;
      result.detail = exception.what();
      return result;
    }
    catch (...) {
      result.status = usbip_host_status::sweep_failed;
      result.detail = "usbip port failed";
      return result;
    }

    if (listing.cancelled || listing.timed_out || listing.exit_code < 0 ||
        listing.exit_code != 0) {
      result.status = command_failure_status(listing);
      result.detail = command_detail(listing);
      return result;
    }

    const auto ports = parse_usbip_port_output(listing.standard_output);
    const auto stale = select_stale_ports(ports, excluded_endpoints, identity);
    for (const auto &port : stale) {
      auto detach_cancel = std::make_shared<std::atomic_bool>(false);
      usbip_command_result detach;
      try {
        detach = config_.command_runner(
          config_.executable,
          { "detach", "--port", std::to_string(port.hub_port) },
          config_.detach_timeout, detach_cancel, config_.max_output_bytes);
      }
      catch (const std::exception &exception) {
        result.failed_ports.push_back(port.hub_port);
        if (result.detail.empty()) {
          result.detail = exception.what();
        }
        continue;
      }
      catch (...) {
        result.failed_ports.push_back(port.hub_port);
        if (result.detail.empty()) {
          result.detail = "usbip detach failed";
        }
        continue;
      }

      if (detach.cancelled || detach.timed_out || detach.exit_code < 0 ||
          detach.exit_code != 0) {
        result.failed_ports.push_back(port.hub_port);
        if (result.detail.empty()) {
          result.detail = detach.standard_error.empty() ?
                            "usbip detach returned a failure" :
                            detach.standard_error;
        }
        continue;
      }
      result.detached_ports.push_back(port.hub_port);
    }

    result.status = result.failed_ports.empty() ? usbip_host_status::ok : usbip_host_status::detach_failed;
    return result;
  }

}  // namespace remote_usb
