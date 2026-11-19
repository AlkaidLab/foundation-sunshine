/**
 * @file src/platform/windows/usbip_device_transport.h
 * @brief Presents a virtual USB device to Windows through usbip-win2 on loopback.
 *
 * This is the only place that knows the USB/IP wire and the usbip-win2 helper.
 * It owns a loopback bridge for the device, hands the bridge's endpoint to the
 * host controller (`usbip attach`), translates each request PDU into a call on
 * the device model, and holds isochronous replies until the packets of the
 * transfer have gone by on the bus clock (the shared paced reply queue).
 *
 * Sources: the USB/IP protocol (Linux kernel documentation
 * Documentation/usb/usbip_protocol.rst) for the PDU layouts, and the
 * usbip-win2 0.9.8.1 sources recorded for the stale port sweep for the attach
 * and detach commands.
 */
#pragma once

#include <memory>
#include <string>

#include "src/remote_usb/remote_usb_host_controller.h"
#include "src/virtual_usb/device.h"

namespace platf {

  /** The bus address the bridge advertises; also the identity of a stale import. */
  struct usbip_bus_address {
    std::string busid { "2-1" };
    std::string path { "/sys/devices/platform/sunshine-vusb/usb2/2-1" };
    std::uint32_t busnum { 2 };
    std::uint32_t devnum { 1 };
  };

  struct usbip_transport_counters {
    std::uint64_t control_requests { 0 };
    std::uint64_t control_stalls { 0 };
    std::uint64_t iso_in_urbs { 0 };
    std::uint64_t iso_in_packets { 0 };
    std::uint64_t unlinks { 0 };
    std::uint64_t unlinks_cancelled { 0 };
    std::uint64_t protocol_errors { 0 };
  };

  /** Transport error codes (virtual_usb::transport_status::error). */
  enum class usbip_transport_error : std::int32_t {
    none = 0,
    unsupported = 1,
    bridge_failed = 2,
    attach_failed = 3,
    attach_timed_out = 4,
    connection_lost = 5,
  };

  class usbip_device_transport final: public virtual_usb::transport {
  public:
    explicit usbip_device_transport(usbip_bus_address address = {});
    ~usbip_device_transport() override;

    usbip_device_transport(const usbip_device_transport &) = delete;
    usbip_device_transport &
    operator=(const usbip_device_transport &) = delete;

    bool
    attach(virtual_usb::device &model, std::chrono::milliseconds timeout) override;
    void
    detach() override;
    virtual_usb::transport_status
    status() const override;

    usbip_transport_counters
    counters() const;

    /** True when this build and machine can attach devices (usbip-win2 present). */
    bool
    available() const;

    /**
     * Detach imports a previous process left behind for this device identity.
     * Called by attach(); exposed so a service can sweep at start-up.
     */
    remote_usb::usbip_sweep_result
    sweep_stale(const virtual_usb::device_identity &identity);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

}  // namespace platf
