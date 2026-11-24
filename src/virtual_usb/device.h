/**
 * @file src/virtual_usb/device.h
 * @brief The seam between a virtual USB device model and the bus transport.
 *
 * A device model owns everything a real device owns: its descriptors, its
 * answers to control requests and the data of its endpoints.  A transport owns
 * everything between the device and the operating system's host controller:
 * the wire format, the attach and detach of the device and the wall-clock
 * that makes isochronous transfers complete at the speed of the bus.
 *
 * Nothing in this header names a wire protocol or a host-side helper.  The
 * models are written against `device` and `transport` only, so a different
 * transport (for example a native virtual bus) can be added as a second
 * implementation of `transport` without touching a model.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace virtual_usb {

  enum class bus_speed : std::uint8_t {
    low = 1,
    full = 2,
    high = 3,
  };

  struct interface_class_triple {
    std::uint8_t interface_class { 0 };
    std::uint8_t interface_subclass { 0 };
    std::uint8_t interface_protocol { 0 };
  };

  /** What the bus needs to announce the device before any descriptor is read. */
  struct device_identity {
    std::uint16_t vendor_id { 0 };
    std::uint16_t product_id { 0 };
    std::uint16_t device_bcd { 0 };
    std::uint8_t device_class { 0 };
    std::uint8_t device_subclass { 0 };
    std::uint8_t device_protocol { 0 };
    std::uint8_t configuration_value { 1 };
    std::uint8_t num_configurations { 1 };
    bus_speed speed { bus_speed::high };
    std::vector<interface_class_triple> interfaces;
  };

  /** The eight bytes of a control transfer's setup stage, decoded. */
  struct setup_packet {
    std::uint8_t request_type { 0 };
    std::uint8_t request { 0 };
    std::uint16_t value { 0 };
    std::uint16_t index { 0 };
    std::uint16_t length { 0 };

    bool
    device_to_host() const noexcept { return (request_type & 0x80u) != 0; }
    /** 0 standard, 1 class, 2 vendor. */
    std::uint8_t
    type() const noexcept { return (request_type >> 5) & 3u; }
    /** 0 device, 1 interface, 2 endpoint, 3 other. */
    std::uint8_t
    recipient() const noexcept { return request_type & 0x1fu; }
  };

  /** The device accepted the request, or stalled the pipe (the bus reports EPIPE). */
  struct control_result {
    bool stall { false };
    /** Data stage of an IN request; ignored for OUT requests. */
    std::vector<std::uint8_t> data;

    static control_result
    ok(std::vector<std::uint8_t> bytes = {}) { return { false, std::move(bytes) }; }
    static control_result
    stalled() { return { true, {} }; }
  };

  /**
   * An isochronous IN transfer of several packets.  `max_length[i]` is the room
   * the host reserved for packet i; the device writes `actual_length[i]` bytes
   * of it into `payload`, packet after packet.
   */
  struct iso_in_request {
    std::uint8_t endpoint { 0 };
    std::span<const std::uint32_t> max_length;
  };
  struct iso_in_reply {
    std::vector<std::uint8_t> payload;
    std::vector<std::uint32_t> actual_length;
  };

  /**
   * A virtual device.  All members are called by the transport, never by two
   * threads at once for the same endpoint; `control` and the iso members may
   * run on different threads.
   */
  class device {
  public:
    virtual ~device() = default;

    virtual const device_identity &
    identity() const = 0;

    /** Answer one control request on endpoint zero. */
    virtual control_result
    control(const setup_packet &setup, std::span<const std::uint8_t> out_data) = 0;

    /**
     * Duration of one isochronous packet slot on `endpoint` (1 ms for a high
     * speed endpoint with bInterval 4).  Zero if the endpoint does not exist.
     */
    virtual std::chrono::nanoseconds
    iso_packet_period(std::uint8_t endpoint) const = 0;

    /**
     * Produce the packets of an isochronous IN transfer.  Called when the
     * slot of the transfer has ended, so the data is as fresh as the bus clock
     * allows.  Returns false if the endpoint is not served.
     */
    virtual bool
    iso_in(const iso_in_request &request, iso_in_reply &reply) = 0;

    /** The bus was reset (or the host re-imported the device): return to the powered state. */
    virtual void
    bus_reset() = 0;
  };

  /** Where a transport is on its way to and from the operating system. */
  enum class transport_state : std::uint8_t {
    detached,
    attaching,
    attached,
    failed,
  };

  struct transport_status {
    transport_state state { transport_state::detached };
    /** Zero when none; otherwise a transport specific code for diagnostics. */
    std::int32_t error { 0 };
    std::string detail;
  };

  /** Presents one device to the operating system. */
  class transport {
  public:
    virtual ~transport() = default;

    /**
     * Attach `model` and return once the operating system has the device or
     * the attempt failed.  The model must outlive the attachment.
     */
    virtual bool
    attach(device &model, std::chrono::milliseconds timeout) = 0;

    /** Remove the device.  Safe to call repeatedly and when not attached. */
    virtual void
    detach() = 0;

    virtual transport_status
    status() const = 0;
  };

}  // namespace virtual_usb
