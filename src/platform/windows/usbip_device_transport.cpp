/**
 * @file src/platform/windows/usbip_device_transport.cpp
 * @brief USB/IP transport of the virtual USB devices (see the header).
 */
#include "usbip_device_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <cfgmgr32.h>

#include <boost/log/trivial.hpp>

#include "alkaidlab/usbip_core/usbip_core.h"
#include "src/logging_severity.h"
#include "src/platform/windows/paced_reply_queue.h"
#include "src/remote_usb/loopback_usbip_bridge.h"

namespace platf {

  namespace {
    using namespace std::chrono_literals;
    constexpr std::int32_t kEpipe = -32;
    constexpr std::int32_t kEconnreset = -104;
    constexpr std::size_t kHeaderBytes = ALK_USBIP_SUBMIT_HEADER_BYTES;

    std::uint32_t
    be32(const std::uint8_t *in) {
      return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
             (static_cast<std::uint32_t>(in[2]) << 8) | in[3];
    }

    /** True while Windows has a present USB device node with this vendor and product id. */
    bool
    device_node_present(std::uint16_t vendor, std::uint16_t product) {
      wchar_t needle[32];
      swprintf(needle, 32, L"VID_%04X&PID_%04X", vendor, product);
      constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
      ULONG size = 0;
      if (CM_Get_Device_ID_List_SizeW(&size, L"USB", flags) != CR_SUCCESS || size == 0) return false;
      std::vector<wchar_t> ids(size);
      if (CM_Get_Device_ID_ListW(L"USB", ids.data(), size, flags) != CR_SUCCESS) return false;
      for (const wchar_t *id = ids.data(); *id != L'\0'; id += wcslen(id) + 1) {
        if (wcsstr(id, needle) != nullptr) return true;
      }
      return false;
    }

    /**
     * Wait for the previous device node to be removed.  Windows removes a node
     * after the detach has returned; an attach started before that finishes can
     * stall, so both ends of a lease wait for the node to go.
     */
    std::chrono::milliseconds
    wait_device_node_gone(std::uint16_t vendor, std::uint16_t product, std::chrono::milliseconds limit) {
      const auto begun = std::chrono::steady_clock::now();
      while (device_node_present(vendor, product) && std::chrono::steady_clock::now() - begun < limit) {
        std::this_thread::sleep_for(25ms);
      }
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begun);
    }

    remote_usb::stale_port_identity
    stale_identity(const virtual_usb::device_identity &identity, const usbip_bus_address &address) {
      return { identity.vendor_id, identity.product_id, address.busid,
        static_cast<std::uint16_t>(address.busnum), static_cast<std::uint16_t>(address.devnum) };
    }
  }  // namespace

  struct usbip_device_transport::impl {
    explicit impl(usbip_bus_address bus):
        address(std::move(bus)) {}

    usbip_bus_address address;
    remote_usb::loopback_usbip_bridge bridge;
    /* A controller is single use (stop() is final), so each attach gets its own. */
    std::unique_ptr<remote_usb::usbip_host_controller> host;
    virtual_usb::device *model { nullptr };
    virtual_usb::device_identity identity;

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::optional<remote_usb::endpoint> live_endpoint;
    std::optional<remote_usb::usbip_host_binding> binding;
    virtual_usb::transport_status current;
    bool imported { false };
    bool attach_completed { false };
    /* Written by the attach completion, which may run after bring_up() returned. */
    bool attach_ok { false };
    std::string attach_detail;
    bool stopping { false };

    std::atomic<std::uint64_t> control_requests { 0 };
    std::atomic<std::uint64_t> control_stalls { 0 };
    std::atomic<std::uint64_t> iso_in_urbs { 0 };
    std::atomic<std::uint64_t> iso_in_packets { 0 };
    std::atomic<std::uint64_t> unlinks { 0 };
    std::atomic<std::uint64_t> unlinks_cancelled { 0 };
    std::atomic<std::uint64_t> protocol_errors { 0 };

    /* Scratch for parsing; request() runs on the bridge's single I/O thread. */
    std::vector<AlkUsbIpIsoPacket> iso_scratch = std::vector<AlkUsbIpIsoPacket>(ALK_USBIP_MAX_ISO_PACKETS);

    /* Declared last: the worker stops before the bridge goes away. */
    paced_reply_queue replies { [this](std::vector<std::uint8_t> wire) { return bridge.send_reply(std::move(wire)); } };

    void
    set_state(virtual_usb::transport_state state, usbip_transport_error error = usbip_transport_error::none,
      std::string detail = {}) {
      std::lock_guard lock(mutex);
      current = { state, static_cast<std::int32_t>(error), std::move(detail) };
      condition.notify_all();
    }

    struct attempt_result;
    attempt_result
    bring_up(std::chrono::milliseconds budget);
    void
    tear_down();

    static std::vector<std::uint8_t>
    ret_submit(std::uint32_t sequence, std::int32_t status, std::span<const std::uint8_t> payload,
      std::uint32_t start_frame = 0, std::span<AlkUsbIpIsoPacket> packets = {}) {
      AlkUsbIpCompletion completion {};
      completion.struct_size = sizeof(completion);
      completion.abi = ALK_USBIP_CORE_ABI_VERSION;
      completion.sequence = sequence;
      completion.status = status;
      completion.actual_length = static_cast<std::uint32_t>(payload.size());
      completion.number_of_packets = static_cast<std::uint32_t>(packets.size());
      completion.iso_packets = packets.data();
      completion.iso_packet_capacity = static_cast<std::uint32_t>(packets.size());
      completion.iso_packet_count = static_cast<std::uint32_t>(packets.size());
      completion.start_frame = start_frame;  // echoed from the request
      std::vector<std::uint8_t> wire(kHeaderBytes + payload.size() + packets.size() * ALK_USBIP_ISO_DESCRIPTOR_BYTES);
      std::size_t length = 0;
      if (alk_usbip_build_ret_submit(&completion, payload.data(), payload.size(), wire.data(), wire.size(), &length) != ALK_USBIP_OK) {
        return {};
      }
      wire.resize(length);
      return wire;
    }

    bool
    reply(std::vector<std::uint8_t> wire) {
      return !wire.empty() && bridge.send_reply(std::move(wire));
    }

    bool
    request(std::vector<std::uint8_t> wire) {
      if (wire.size() < kHeaderBytes || model == nullptr) return fail_protocol();
      const auto command = be32(wire.data());
      if (command == ALK_USBIP_CMD_UNLINK) return unlink(wire);
      if (command != ALK_USBIP_CMD_SUBMIT) return fail_protocol();

      AlkUsbIpUrb urb {};
      urb.struct_size = sizeof(urb);
      urb.abi = ALK_USBIP_CORE_ABI_VERSION;
      urb.iso_packets = iso_scratch.data();
      urb.iso_packet_capacity = static_cast<std::uint32_t>(iso_scratch.size());
      if (const auto parsed = alk_usbip_parse_submit(wire.data(), wire.size(), &urb); parsed != ALK_USBIP_OK) {
        return fail_protocol("submit", static_cast<int>(parsed), &wire);
      }

      if (urb.endpoint == 0) return control(urb, wire);
      const bool iso = urb.number_of_packets != 0xffffffffu && urb.number_of_packets > 0;
      if (urb.direction == 1 && iso) return iso_in(urb);
      /* Nothing else exists on this device: report a stalled pipe. */
      return reply(ret_submit(urb.sequence, kEpipe, {}));
    }

    bool
    fail_protocol(const char *what = "malformed request", int code = 0, const std::vector<std::uint8_t> *wire = nullptr) {
      protocol_errors.fetch_add(1, std::memory_order_relaxed);
      std::string head;
      if (wire != nullptr) {
        static constexpr char digits[] = "0123456789abcdef";
        for (std::size_t i = 0; i < wire->size() && i < 48; ++i) {
          head.push_back(digits[(*wire)[i] >> 4]);
          head.push_back(digits[(*wire)[i] & 15]);
        }
      }
      BOOST_LOG(warning) << "USB/IP device request rejected: " << what << " (core error " << code << ") head=" << head;
      return false;
    }

    bool
    unlink(const std::vector<std::uint8_t> &wire) {
      std::uint32_t target = 0;
      if (const auto parsed = alk_usbip_parse_unlink(wire.data(), wire.size(), &target); parsed != ALK_USBIP_OK) {
        return fail_protocol("unlink", static_cast<int>(parsed), &wire);
      }
      unlinks.fetch_add(1, std::memory_order_relaxed);
      /* A held reply is withdrawn (-ECONNRESET); one already on the wire
       * finished first and the unlink reports 0.  The bridge drops a racing
       * RET_SUBMIT for a cancelled target. */
      const bool cancelled = replies.cancel(target);
      if (cancelled) unlinks_cancelled.fetch_add(1, std::memory_order_relaxed);
      std::array<std::uint8_t, 48> out {};
      std::size_t length = 0;
      if (alk_usbip_build_ret_unlink(be32(wire.data() + 4), cancelled ? kEconnreset : 0, out.data(), out.size(), &length) != ALK_USBIP_OK) {
        return fail_protocol();
      }
      return reply({ out.begin(), out.begin() + static_cast<std::ptrdiff_t>(length) });
    }

    bool
    control(const AlkUsbIpUrb &urb, const std::vector<std::uint8_t> &wire) {
      control_requests.fetch_add(1, std::memory_order_relaxed);
      virtual_usb::setup_packet setup;
      setup.request_type = urb.setup[0];
      setup.request = urb.setup[1];
      setup.value = static_cast<std::uint16_t>(urb.setup[2] | (urb.setup[3] << 8));
      setup.index = static_cast<std::uint16_t>(urb.setup[4] | (urb.setup[5] << 8));
      setup.length = static_cast<std::uint16_t>(urb.setup[6] | (urb.setup[7] << 8));
      const std::span<const std::uint8_t> out_data = urb.direction == 0 ?
                                                       std::span<const std::uint8_t>(wire.data() + kHeaderBytes, urb.transfer_buffer_length) :
                                                       std::span<const std::uint8_t> {};
      auto result = model->control(setup, out_data);
      if (result.stall) {
        control_stalls.fetch_add(1, std::memory_order_relaxed);
        return reply(ret_submit(urb.sequence, kEpipe, {}));
      }
      if (urb.direction == 1) {
        if (result.data.size() > urb.transfer_buffer_length) result.data.resize(urb.transfer_buffer_length);
        return reply(ret_submit(urb.sequence, 0, result.data));
      }
      /* OUT: acknowledge the bytes received. */
      auto ack = ret_submit(urb.sequence, 0, {});
      if (ack.size() >= 28) {
        for (int i = 0; i < 4; ++i) ack[24 + i] = static_cast<std::uint8_t>(urb.transfer_buffer_length >> (24 - 8 * i));
      }
      return reply(std::move(ack));
    }

    bool
    iso_in(const AlkUsbIpUrb &urb) {
      const auto endpoint = static_cast<std::uint8_t>(urb.endpoint & 0x0f);
      const auto period = model->iso_packet_period(endpoint);
      if (period.count() <= 0) return reply(ret_submit(urb.sequence, kEpipe, {}));
      iso_in_urbs.fetch_add(1, std::memory_order_relaxed);
      iso_in_packets.fetch_add(urb.number_of_packets, std::memory_order_relaxed);

      std::vector<std::uint32_t> max_length(urb.number_of_packets);
      std::vector<std::uint32_t> offset(urb.number_of_packets);
      for (std::uint32_t i = 0; i < urb.number_of_packets; ++i) {
        max_length[i] = iso_scratch[i].length;
        offset[i] = iso_scratch[i].offset;
      }
      const auto sequence = urb.sequence;
      const auto start_frame = urb.start_frame;
      replies.schedule_iso_lazy(paced_reply_queue::lane::iso_in, sequence, urb.number_of_packets, period,
        [this, endpoint, sequence, start_frame, max_length = std::move(max_length), offset = std::move(offset)] {
          virtual_usb::iso_in_reply data;
          if (!model->iso_in({ endpoint, max_length }, data) || data.actual_length.size() != max_length.size()) {
            return ret_submit(sequence, kEpipe, {});
          }
          std::vector<AlkUsbIpIsoPacket> packets(max_length.size());
          for (std::size_t i = 0; i < packets.size(); ++i) {
            packets[i] = { offset[i], max_length[i], data.actual_length[i], 0 };
          }
          return ret_submit(sequence, 0, data.payload, start_frame, packets);
        });
      return true;
    }
  };

  usbip_device_transport::usbip_device_transport(usbip_bus_address address):
      impl_(std::make_unique<impl>(std::move(address))) {}

  usbip_device_transport::~usbip_device_transport() { detach(); }

  bool
  usbip_device_transport::available() const { return remote_usb::usbip_host_controller().backend_supported(); }

  remote_usb::usbip_sweep_result
  usbip_device_transport::sweep_stale(const virtual_usb::device_identity &identity) {
    remote_usb::usbip_host_controller sweeper;
    return sweeper.sweep_stale_ports(stale_identity(identity, impl_->address));
  }

  struct usbip_device_transport::impl::attempt_result {
    bool ok { false };
    usbip_transport_error error { usbip_transport_error::attach_failed };
    std::string detail;
  };

  usbip_device_transport::impl::attempt_result
  usbip_device_transport::impl::bring_up(std::chrono::milliseconds budget) {
    host = std::make_unique<remote_usb::usbip_host_controller>();
    if (const auto waited = wait_device_node_gone(identity.vendor_id, identity.product_id, 3000ms); waited > 200ms) {
      BOOST_LOG(info) << "waited " << waited.count() << " ms for the previous device node to go";
    }
    if (!host->backend_supported()) return { false, usbip_transport_error::unsupported, "no usbip-win2 host backend" };
    {
      remote_usb::usbip_host_controller sweeper;
      (void) sweeper.sweep_stale_ports({ identity.vendor_id, identity.product_id, address.busid,
        static_cast<std::uint16_t>(address.busnum), static_cast<std::uint16_t>(address.devnum) });
    }

    remote_usb::device_info info;
    info.busid = address.busid;
    info.path = address.path;
    info.busnum = address.busnum;
    info.devnum = address.devnum;
    info.speed = static_cast<std::uint32_t>(identity.speed);
    info.vendor_id = identity.vendor_id;
    info.product_id = identity.product_id;
    info.device_bcd = identity.device_bcd;
    info.device_class = identity.device_class;
    info.device_subclass = identity.device_subclass;
    info.device_protocol = identity.device_protocol;
    info.configuration_value = identity.configuration_value;
    info.num_configurations = identity.num_configurations;
    for (const auto &triple : identity.interfaces) {
      info.interfaces.push_back({ triple.interface_class, triple.interface_subclass, triple.interface_protocol });
    }

    remote_usb::callbacks callbacks;
    callbacks.on_request = [this](std::vector<std::uint8_t> wire) { return request(std::move(wire)); };
    callbacks.on_imported = [this] {
      std::lock_guard lock(mutex);
      imported = true;
      condition.notify_all();
    };
    callbacks.on_closed = [this](remote_usb::close_reason) {
      /* Drop held replies before taking the state lock. */
      replies.clear();
      virtual_usb::device *model_ptr = nullptr;
      bool expected = false;
      {
        std::lock_guard lock(mutex);
        imported = false;
        model_ptr = model;
        expected = stopping;
        if (!stopping && current.state == virtual_usb::transport_state::attached) {
          current = { virtual_usb::transport_state::failed,
            static_cast<std::int32_t>(usbip_transport_error::connection_lost), "the USB/IP connection closed" };
        }
        condition.notify_all();
      }
      if (model_ptr != nullptr && !expected) model_ptr->bus_reset();
    };

    boost::system::error_code error;
    const auto endpoint = bridge.start(std::move(info), std::move(callbacks), error);
    if (!endpoint) return { false, usbip_transport_error::bridge_failed, error.message() };
    {
      std::lock_guard lock(mutex);
      live_endpoint = *endpoint;
      attach_completed = false;
    }
    remote_usb::remember_protected_endpoint(*endpoint);

    {
      std::lock_guard lock(mutex);
      attach_ok = false;
      attach_detail.clear();
    }
    const auto operation = host->attach(remote_usb::usbip_host_request { *endpoint },
      [this](remote_usb::usbip_host_result result) {
        std::lock_guard lock(mutex);
        attach_ok = result.ok();
        if (!result.ok()) attach_detail = result.detail;
        if (result.ok()) binding = result.binding;
        attach_completed = true;
        condition.notify_all();
      });
    std::unique_lock lock(mutex);
    if (operation == 0) {
      attach_detail = "usbip attach was rejected before it started";
      attach_completed = true;
    }
    const bool done = condition.wait_for(lock, budget, [this] { return attach_completed || stopping; });
    if (!done) {
      lock.unlock();
      /* The helper may still be talking to the listener; end it before anything else reuses the bus. */
      host->cancel(operation);
      return { false, usbip_transport_error::attach_timed_out, "usbip attach timed out" };
    }
    if (!attach_ok) return { false, usbip_transport_error::attach_failed, attach_detail };
    return { true, usbip_transport_error::none, {} };
  }

  void
  usbip_device_transport::impl::tear_down() {
    std::optional<remote_usb::endpoint> endpoint;
    std::optional<remote_usb::usbip_host_binding> held_binding;
    {
      std::lock_guard lock(mutex);
      held_binding = std::exchange(binding, std::nullopt);
      endpoint = live_endpoint;
    }
    /* Detach while the device still answers, then close the listener: a vanished
     * server makes usbip-win2 treat the device as lost and retry the connection. */
    if (held_binding && host) {
      const auto begun = std::chrono::steady_clock::now();
      std::mutex wait_mutex;
      std::condition_variable wait_condition;
      std::optional<remote_usb::usbip_host_result> outcome;
      const auto operation = host->detach(*held_binding, [&](remote_usb::usbip_host_result result) {
        std::lock_guard lock(wait_mutex);
        outcome = std::move(result);
        wait_condition.notify_all();
      });
      if (operation != 0) {
        std::unique_lock lock(wait_mutex);
        wait_condition.wait_for(lock, 8s, [&] { return outcome.has_value(); });
      }
      const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begun).count();
      BOOST_LOG(info) << "usbip detach took " << took << " ms: "
                      << (outcome ? (outcome->ok() ? "ok" : "failed: " + outcome->detail) : std::string("no answer"));
    }
    bridge.stop();
    if (host) host->stop();
    replies.clear();
    if (endpoint) remote_usb::forget_protected_endpoint(*endpoint);
    {
      std::lock_guard lock(mutex);
      live_endpoint.reset();
      host.reset();
      imported = false;
    }
    const auto waited = wait_device_node_gone(identity.vendor_id, identity.product_id, 5000ms);
    BOOST_LOG(info) << "device node removal took " << waited.count() << " ms";
  }

  bool
  usbip_device_transport::attach(virtual_usb::device &model, std::chrono::milliseconds timeout) {
    auto &s = *impl_;
    {
      std::lock_guard lock(s.mutex);
      if (s.model != nullptr) return false;
      s.model = &model;
      s.identity = model.identity();
      s.stopping = false;
      s.imported = false;
    }
    s.set_state(virtual_usb::transport_state::attaching);

    /* A normal attach takes tens of milliseconds, a slow one a few seconds.  An attempt
     * that is still silent after six seconds is abandoned with its listener and its
     * helper process, and the whole bring-up starts again on a fresh port. */
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    impl::attempt_result last;
    for (int attempt = 0; attempt != 3; ++attempt) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (attempt > 0 && remaining <= 500ms) break;
      last = s.bring_up(attempt == 2 ? std::max(remaining, 1ms) : std::min(std::max(remaining, 1ms), 6000ms));
      if (last.ok) {
        s.set_state(virtual_usb::transport_state::attached);
        return true;
      }
      BOOST_LOG(warning) << "USB/IP device attach attempt " << attempt + 1 << " failed: " << last.detail;
      s.tear_down();
      if (last.error == usbip_transport_error::unsupported || last.error == usbip_transport_error::bridge_failed) break;
      std::this_thread::sleep_for(250ms);
    }
    /* Leave nothing imported: the abandoned attempt's port is swept by identity. */
    {
      remote_usb::usbip_host_controller sweeper;
      (void) sweeper.sweep_stale_ports({ s.identity.vendor_id, s.identity.product_id, s.address.busid,
        static_cast<std::uint16_t>(s.address.busnum), static_cast<std::uint16_t>(s.address.devnum) });
    }
    s.set_state(virtual_usb::transport_state::failed, last.error, last.detail);
    detach();
    return false;
  }

  void
  usbip_device_transport::detach() {
    auto &s = *impl_;
    {
      std::lock_guard lock(s.mutex);
      if (s.model == nullptr && !s.live_endpoint) return;
      s.stopping = true;
      s.condition.notify_all();
    }
    s.tear_down();
    std::lock_guard lock(s.mutex);
    s.model = nullptr;
    if (s.current.state != virtual_usb::transport_state::failed) s.current = {};
    s.stopping = false;
  }

  virtual_usb::transport_status
  usbip_device_transport::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current;
  }

  usbip_transport_counters
  usbip_device_transport::counters() const {
    const auto &s = *impl_;
    return { s.control_requests, s.control_stalls, s.iso_in_urbs, s.iso_in_packets, s.unlinks,
      s.unlinks_cancelled, s.protocol_errors };
  }

}  // namespace platf
