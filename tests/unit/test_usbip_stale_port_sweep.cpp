/**
 * @file tests/unit/test_usbip_stale_port_sweep.cpp
 * @brief Selection and cleanup tests for stale native DualSense imports.
 */

#include <src/remote_usb/remote_usb_host_controller.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

  using namespace std::chrono_literals;

  constexpr std::string_view kPortListing = R"(Imported USB devices
====================
Port 01: device in use at High Speed
         DualSense Wireless Controller (054c:0ce6)
           -> usbip://127.0.0.1:53122/1-1
           -> remote bus/dev: 001/001
           -> serial:
           -> mode: zero_copy
Port 02: device in use at High Speed
         DualSense Wireless Controller (054c:0ce6)
           -> usbip://127.0.0.1:53123/1-2
           -> remote bus/dev: 001/003
           -> serial: ABC123
           -> mode: zero_copy
)";

  remote_usb::usbip_imported_port
  make_port(std::uint16_t hub_port, std::string busid, std::string address = "127.0.0.1",
    std::uint16_t remote_bus = 1, std::uint16_t remote_device = 1,
    std::uint16_t vendor = 0x054c, std::uint16_t product = 0x0ce6) {
    remote_usb::usbip_imported_port port;
    port.hub_port = hub_port;
    port.server_address = std::move(address);
    port.server_port = 53122;
    port.busid = std::move(busid);
    port.vendor_id = vendor;
    port.product_id = product;
    port.remote_bus = remote_bus;
    port.remote_device = remote_device;
    port.loopback_server = port.server_address == "127.0.0.1" ||
                           port.server_address == "::1" ||
                           port.server_address == "localhost";
    return port;
  }

  struct command_harness {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::vector<std::string>> calls;
    std::function<remote_usb::usbip_command_result(const std::vector<std::string> &)> behavior;

    remote_usb::usbip_command_result
    run(const std::string &, const std::vector<std::string> &arguments,
      std::chrono::milliseconds, const std::shared_ptr<std::atomic_bool> &, std::size_t) {
      {
        std::lock_guard lock(mutex);
        calls.push_back(arguments);
        condition.notify_all();
      }
      if (behavior) {
        return behavior(arguments);
      }
      remote_usb::usbip_command_result result;
      result.exit_code = 0;
      return result;
    }

    std::vector<std::vector<std::string>>
    snapshot() {
      std::lock_guard lock(mutex);
      return calls;
    }
  };

  remote_usb::usbip_host_controller_config
  make_config(command_harness &harness) {
    remote_usb::usbip_host_controller_config config;
    config.executable = "usbip-test";
    config.backend = remote_usb::usbip_host_backend::usbip_win2;
    config.attach_timeout = 2s;
    config.detach_timeout = 2s;
    config.command_runner = [&harness](
                              const std::string &executable,
                              const std::vector<std::string> &arguments,
                              std::chrono::milliseconds timeout,
                              const std::shared_ptr<std::atomic_bool> &cancel,
                              std::size_t max_output_bytes) {
      return harness.run(executable, arguments, timeout, cancel, max_output_bytes);
    };
    return config;
  }

  bool
  has_detach(const std::vector<std::vector<std::string>> &calls, std::string_view port) {
    return std::any_of(calls.begin(), calls.end(), [port](const auto &arguments) {
      return arguments.size() == 3 && arguments[0] == "detach" &&
             arguments[1] == "--port" && arguments[2] == port;
    });
  }

  struct forwarded_scope {
    explicit forwarded_scope(remote_usb::endpoint value): endpoint(std::move(value)) {
      remote_usb::remember_protected_endpoint(endpoint);
    }
    ~forwarded_scope() {
      remote_usb::forget_protected_endpoint(endpoint);
    }
    remote_usb::endpoint endpoint;
  };

}  // namespace

TEST(UsbipPortParser, ReadsImportedDeviceIdentity) {
  const auto ports = remote_usb::parse_usbip_port_output(kPortListing);
  ASSERT_EQ(ports.size(), 2U);

  EXPECT_EQ(ports[0].hub_port, 1);
  EXPECT_EQ(ports[0].server_address, "127.0.0.1");
  EXPECT_EQ(ports[0].server_port, 53122);
  EXPECT_EQ(ports[0].busid, "1-1");
  EXPECT_EQ(ports[0].vendor_id, 0x054c);
  EXPECT_EQ(ports[0].product_id, 0x0ce6);
  EXPECT_EQ(ports[0].remote_bus, 1);
  EXPECT_EQ(ports[0].remote_device, 1);
  EXPECT_TRUE(ports[0].loopback_server);

  EXPECT_EQ(ports[1].hub_port, 2);
  EXPECT_EQ(ports[1].busid, "1-2");
  EXPECT_EQ(ports[1].remote_device, 3);
  EXPECT_TRUE(ports[1].loopback_server);
}

TEST(UsbipPortParser, EmptyAndUnknownOutputAreEmpty) {
  EXPECT_TRUE(remote_usb::parse_usbip_port_output({}).empty());
  EXPECT_TRUE(remote_usb::parse_usbip_port_output("no imported devices\n").empty());
}

TEST(UsbipStalePortSelection, SelectsOnlyNativeLoopbackIdentity) {
  const auto ports = remote_usb::parse_usbip_port_output(kPortListing);
  const auto stale = remote_usb::select_stale_ds5_ports(ports, {});

  ASSERT_EQ(stale.size(), 1U);
  EXPECT_EQ(stale[0].hub_port, 1);
}

TEST(UsbipStalePortSelection, NeverSelectsForwardedRealDevice) {
  const std::vector<remote_usb::usbip_imported_port> ports {
    make_port(1, "1-2", "127.0.0.1", 1, 4),
    make_port(2, "1-1", "127.0.0.1", 1, 2),
    make_port(3, "1-1", "192.0.2.10", 1, 1),
    make_port(4, "1-1", "127.0.0.1", 1, 1, 0x054c, 0x05c4),
  };

  EXPECT_TRUE(remote_usb::select_stale_ds5_ports(ports, {}).empty());
}

TEST(UsbipStalePortSelection, ExcludesControllerOwnedEndpoint) {
  const std::vector<remote_usb::usbip_imported_port> ports {
    make_port(7, "1-1"),
  };
  const std::vector<remote_usb::endpoint> protected_endpoints {
    remote_usb::endpoint { "127.0.0.1", 53122, "1-1" },
  };

  EXPECT_TRUE(remote_usb::select_stale_ds5_ports(ports, protected_endpoints).empty());
}

TEST(UsbipStalePortSweep, DetachesOnlyNativeBridgePort) {
  command_harness commands;
  commands.behavior = [](const std::vector<std::string> &arguments) {
    remote_usb::usbip_command_result result;
    result.exit_code = 0;
    if (arguments == std::vector<std::string> { "port" }) {
      result.standard_output = std::string { kPortListing };
    }
    return result;
  };
  remote_usb::usbip_host_controller controller(make_config(commands));

  const auto result = controller.sweep_stale_ds5_ports();
  EXPECT_TRUE(result.ok()) << result.detail;
  ASSERT_EQ(result.detached_ports.size(), 1U);
  EXPECT_EQ(result.detached_ports[0], 1);
  EXPECT_TRUE(result.failed_ports.empty());

  const auto calls = commands.snapshot();
  EXPECT_TRUE(has_detach(calls, "1"));
  EXPECT_FALSE(has_detach(calls, "2"));
}

TEST(UsbipStalePortSweep, SkipsEndpointOwnedByAcceptedAttach) {
  command_harness commands;
  commands.behavior = [](const std::vector<std::string> &arguments) {
    remote_usb::usbip_command_result result;
    result.exit_code = 0;
    if (arguments == std::vector<std::string> { "port" }) {
      result.standard_output = std::string { kPortListing };
    }
    else if (std::find(arguments.begin(), arguments.end(), "attach") != arguments.end()) {
      result.standard_output = "1\n";
    }
    return result;
  };
  remote_usb::usbip_host_controller controller(make_config(commands));

  std::mutex mutex;
  std::condition_variable condition;
  bool attached = false;
  remote_usb::usbip_host_request request {
    remote_usb::endpoint { "127.0.0.1", 53122, "1-1" },
  };
  ASSERT_NE(controller.attach(std::move(request), [&](remote_usb::usbip_host_result result) {
    std::lock_guard lock(mutex);
    attached = result.ok();
    condition.notify_all();
  }),
    0U);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return attached; }));
  }

  const auto result = controller.sweep_stale_ds5_ports();
  EXPECT_TRUE(result.ok()) << result.detail;
  EXPECT_TRUE(result.detached_ports.empty());
  EXPECT_TRUE(result.failed_ports.empty());
  EXPECT_FALSE(has_detach(commands.snapshot(), "1"));
}

TEST(UsbipStalePortSweep, SkipsRegisteredLiveLeaseAcrossControllers) {
  const forwarded_scope forwarded { remote_usb::endpoint { "127.0.0.1", 53122, "1-1" } };
  command_harness commands;
  commands.behavior = [](const std::vector<std::string> &arguments) {
    remote_usb::usbip_command_result result;
    result.exit_code = 0;
    if (arguments == std::vector<std::string> { "port" }) {
      result.standard_output = std::string { kPortListing };
    }
    return result;
  };
  remote_usb::usbip_host_controller controller(make_config(commands));

  const auto result = controller.sweep_stale_ds5_ports();
  EXPECT_TRUE(result.ok()) << result.detail;
  EXPECT_TRUE(result.detached_ports.empty());
  EXPECT_TRUE(result.failed_ports.empty());
  EXPECT_FALSE(has_detach(commands.snapshot(), "1"));
}

namespace {
  /* The virtual microphone's bridge identity (src/virtual_usb/uac1_microphone.h, usbip_device_transport.h). */
  const remote_usb::stale_port_identity kMicrophoneIdentity { 0x1209, 0xa1c0, "2-1", 2, 1 };

  constexpr std::string_view kMixedListing = R"(Imported USB devices
====================
Port 01: device in use at High Speed
         DualSense Wireless Controller (054c:0ce6)
           -> usbip://127.0.0.1:53122/1-1
           -> remote bus/dev: 001/001
Port 02: device in use at High Speed
         Sunshine Virtual Microphone (1209:a1c0)
           -> usbip://127.0.0.1:53130/2-1
           -> remote bus/dev: 002/001
Port 03: device in use at High Speed
         Sunshine Virtual Microphone (1209:a1c0)
           -> usbip://192.0.2.10:3240/2-1
           -> remote bus/dev: 002/001
)";
}  // namespace

TEST(UsbipStalePortSelection, EachNativeDeviceIsSelectedOnlyByItsOwnIdentity) {
  const auto ports = remote_usb::parse_usbip_port_output(kMixedListing);
  ASSERT_EQ(ports.size(), 3U);

  const auto microphone = remote_usb::select_stale_ports(ports, {}, kMicrophoneIdentity);
  ASSERT_EQ(microphone.size(), 1U);
  EXPECT_EQ(microphone[0].hub_port, 2);  // not the remote-address twin on port 3

  const auto controller = remote_usb::select_stale_ds5_ports(ports, {});
  ASSERT_EQ(controller.size(), 1U);
  EXPECT_EQ(controller[0].hub_port, 1);
}

TEST(UsbipStalePortSelection, MicrophoneImportOfThisProcessIsProtected) {
  const auto ports = remote_usb::parse_usbip_port_output(kMixedListing);
  const std::vector<remote_usb::endpoint> protected_endpoints {
    remote_usb::endpoint { "127.0.0.1", 53130, "2-1" },
  };
  EXPECT_TRUE(remote_usb::select_stale_ports(ports, protected_endpoints, kMicrophoneIdentity).empty());
}

TEST(UsbipStalePortSweep, DetachesOnlyTheMicrophonePortForItsIdentity) {
  command_harness commands;
  commands.behavior = [](const std::vector<std::string> &arguments) {
    remote_usb::usbip_command_result result;
    result.exit_code = 0;
    if (arguments == std::vector<std::string> { "port" }) {
      result.standard_output = std::string { kMixedListing };
    }
    return result;
  };
  remote_usb::usbip_host_controller controller(make_config(commands));

  const auto result = controller.sweep_stale_ports(kMicrophoneIdentity);
  EXPECT_TRUE(result.ok()) << result.detail;
  ASSERT_EQ(result.detached_ports.size(), 1U);
  EXPECT_EQ(result.detached_ports[0], 2);

  const auto calls = commands.snapshot();
  EXPECT_TRUE(has_detach(calls, "2"));
  EXPECT_FALSE(has_detach(calls, "1"));
  EXPECT_FALSE(has_detach(calls, "3"));
}
