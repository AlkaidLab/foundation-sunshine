/**
 * @file src/remote_usb/remote_usb_host_controller.h
 * @brief Structured host-side USB/IP attach and detach orchestration.
 *
 * The tunnel exposes a short-lived loopback USB/IP endpoint.  This controller
 * is the only layer allowed to turn that endpoint into a usbip-win2 command;
 * callers never have to concatenate a shell command or parse an arbitrary
 * process lifetime.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "usbip_endpoint.h"

namespace remote_usb {

/** The tunnel authenticates the peer before requesting a local USB/IP attach.
 *  The endpoint identifies an active device, but ports may be reused later. */
struct usbip_host_request {
  endpoint server_endpoint;
};

struct usbip_host_binding {
  endpoint server_endpoint;
  /** usbip-win2 virtual hub port returned by `attach --terse`. */
  std::uint16_t hub_port { 0 };
  /** Controller-issued attach operation ID; rejects stale detach after reuse. */
  std::uint64_t binding_id { 0 };

  friend bool operator==(const usbip_host_binding &, const usbip_host_binding &) = default;
};

enum class usbip_host_status : std::uint8_t {
  ok,
  invalid_argument,
  busy,
  stopped,
  launch_failed,
  attach_failed,
  detach_failed,
  timed_out,
  cancelled,
  /** This build has no host-side USB/IP backend enabled. */
  unsupported,
  /** Listing the host's imported ports failed. */
  sweep_failed,
};

/**
 * Host-side command dialect selected by the controller.
 *
 * `automatic` is the production default: it enables usbip-win2 only on
 * Windows.  Other platforms deliberately resolve to `unsupported` until a
 * native host backend is implemented, so a generic `usbip` executable cannot
 * be mistaken for a compatible implementation.  A caller may explicitly
 * select `usbip_win2` (for example, an integration test or a packaged
 * compatibility layer).
 */
enum class usbip_host_backend : std::uint8_t {
  automatic,
  usbip_win2,
  unsupported,
};

struct usbip_host_result {
  usbip_host_status status { usbip_host_status::invalid_argument };
  std::uint64_t operation_id { 0 };
  std::optional<usbip_host_binding> binding;
  std::string detail;

  bool ok() const noexcept { return status == usbip_host_status::ok; }
};

/** One device listed by usbip-win2's `port` command. */
struct usbip_imported_port {
  std::uint16_t hub_port { 0 };
  std::string server_address;
  std::uint16_t server_port { 0 };
  std::string busid;
  std::uint16_t vendor_id { 0 };
  std::uint16_t product_id { 0 };
  std::uint16_t remote_bus { 0 };
  std::uint16_t remote_device { 0 };
  bool loopback_server { false };
};

/** Outcome of one best-effort stale-port sweep. */
struct usbip_sweep_result {
  usbip_host_status status { usbip_host_status::invalid_argument };
  std::string detail;
  std::vector<std::uint16_t> detached_ports;
  std::vector<std::uint16_t> failed_ports;

  bool ok() const noexcept { return status == usbip_host_status::ok; }
};

/**
 * Parse the text form of usbip-win2's `port` command.  Unknown lines are
 * ignored, and an empty listing is a successful empty result.
 */
std::vector<usbip_imported_port>
parse_usbip_port_output(std::string_view output);

/**
 * Identity of a loopback import that this product's own bridge creates.  A
 * sweep recognizes a stale import only by all of these fields together, so a
 * real device forwarded from a client (different remote device number) and the
 * imports of other native devices are never touched.
 */
struct stale_port_identity {
  std::uint16_t vendor_id { 0 };
  std::uint16_t product_id { 0 };
  std::string busid;
  std::uint16_t remote_bus { 0 };
  std::uint16_t remote_device { 0 };
};

/** The identity the native DualSense bridge advertises. */
stale_port_identity
ds5_stale_port_identity();

/**
 * Select the loopback imports that match `identity` and are not owned by this
 * process (`protected_endpoints`).
 */
std::vector<usbip_imported_port>
select_stale_ports(std::span<const usbip_imported_port> ports,
                   std::span<const endpoint> protected_endpoints,
                   const stale_port_identity &identity);

/**
 * Select imports left by this product's native DualSense bridge.
 *
 * The bridge advertises 054c:0ce6 on loopback with busid 1-1 and remote
 * bus/device 001/001.  A real controller forwarded from a client has a
 * different remote device number, and endpoints still owned by this process
 * are excluded explicitly so a forwarded device is never detached.
 */
std::vector<usbip_imported_port>
select_stale_ds5_ports(std::span<const usbip_imported_port> ports,
                       std::span<const endpoint> protected_endpoints);

/**
 * Remember a live USB/IP lease for the lifetime of its owner.  Every stale-
 * port sweep in this process excludes remembered endpoints, even when they
 * belong to a different controller instance.
 */
void remember_protected_endpoint(const endpoint &value);
void forget_protected_endpoint(const endpoint &value);

/** Result of one bounded, argument-vector process invocation. */
struct usbip_command_result {
  int exit_code { -1 };
  bool timed_out { false };
  bool cancelled { false };
  std::string standard_output;
  std::string standard_error;
};

/**
 * Injectable command runner.  `cancel` is shared with the controller and must
 * be observed while the process is running.  The executable and arguments are
 * already separated; implementations must not pass them through a shell.
 */
using usbip_command_runner = std::function<usbip_command_result(
  const std::string &executable,
  const std::vector<std::string> &arguments,
  std::chrono::milliseconds timeout,
  const std::shared_ptr<std::atomic_bool> &cancel,
  std::size_t max_output_bytes)>;

/** Injectable only to exercise process reader allocation failures in tests. */
using usbip_reader_thread_factory =
  std::function<std::thread(std::function<void()>)>;

struct usbip_host_controller_config {
  /** Empty selects the platform default (`usbip.exe` on Windows). */
  std::string executable;
  usbip_host_backend backend { usbip_host_backend::automatic };
  std::chrono::milliseconds attach_timeout { std::chrono::seconds(10) };
  std::chrono::milliseconds detach_timeout { std::chrono::seconds(5) };
  std::size_t max_output_bytes { 64u * 1024u };
  std::size_t max_concurrent_operations { 4 };
  usbip_command_runner command_runner;
  usbip_reader_thread_factory reader_thread_factory;
};

using usbip_host_completion = std::function<void(usbip_host_result)>;

/**
 * Runs one-shot usbip-win2 attach/detach operations without shell parsing.
 * Operations complete on worker threads.  `stop()` cancels and joins all
 * workers and attempts to detach every binding which this controller accepted.
 */
class usbip_host_controller final {
public:
  using operation_id = std::uint64_t;

  explicit usbip_host_controller(usbip_host_controller_config config = {});
  ~usbip_host_controller();

  usbip_host_controller(const usbip_host_controller &) = delete;
  usbip_host_controller &operator=(const usbip_host_controller &) = delete;

  /** Start `usbip ... attach`; returns zero when rejected before dispatch. */
  operation_id attach(usbip_host_request request, usbip_host_completion completion);

  /** Start `usbip detach -p`; returns zero when rejected before dispatch. */
  operation_id detach(usbip_host_binding binding, usbip_host_completion completion);

  /** Request cancellation of one in-flight operation. */
  bool cancel(operation_id id);

  /** Cancel, detach accepted bindings, and synchronously join workers. */
  void stop() noexcept;

  /**
   * Detach virtual hub ports left by a previous native DualSense lease.
   * This is best effort: an unavailable helper or a failed detach is reported
   * in the result but never prevents a later attach.
   */
  usbip_sweep_result sweep_stale_ds5_ports();

  /** The same sweep for any native bridge identity (see stale_port_identity). */
  usbip_sweep_result sweep_stale_ports(const stale_port_identity &identity);

  bool stopped() const noexcept;
  std::size_t active_operations() const noexcept;

  /** Exposed for platform/bootstrap code and tests. */
  static std::string default_executable();

  /** The backend resolved from configuration and the current build target. */
  usbip_host_backend backend() const noexcept;

  /** Whether attach/detach can be dispatched on this controller. */
  bool backend_supported() const noexcept;

private:
  enum class operation_kind : std::uint8_t { attach, detach };

  struct operation {
    operation_id id { 0 };
    operation_kind kind { operation_kind::attach };
    usbip_host_request request;
    usbip_host_binding binding;
    std::shared_ptr<std::atomic_bool> cancel { std::make_shared<std::atomic_bool>(false) };
    /* The helper process and binding bookkeeping can finish before the user
     * completion returns.  Keep this separate from `finished`: dispatch uses
     * command_finished to permit a completion callback to schedule a
     * compensating detach, while reaping/joining waits for the callback stack
     * itself to unwind. */
    std::atomic_bool command_finished { false };
    std::atomic_bool finished { false };
    /* Joining is serialized separately from controller mutex_.  A completed
     * worker may be reaped by one API call while stop() races from another
     * thread; holding controller mutex_ across std::thread::join would let a
     * completion callback deadlock on that same mutex. */
    std::mutex join_mutex;
    std::condition_variable join_condition;
    bool join_claimed { false };
    bool joined { false };
    std::mutex start_mutex;
    std::condition_variable start_condition;
    bool start_released { false };
    /* Set before the worker waits on start_condition.  stop() uses this
     * instead of touching std::thread concurrently when a completion callback
     * re-enters stop from a worker thread. */
    std::thread::id worker_id;
    std::thread worker;
  };

  operation_id dispatch(operation_kind kind,
                        usbip_host_request request,
                        usbip_host_binding binding,
                        usbip_host_completion completion);
  usbip_host_result run_attach(const std::shared_ptr<operation> &operation);
  usbip_host_result run_detach(const std::shared_ptr<operation> &operation);
  usbip_host_result invalid_result(usbip_host_status status,
                                   operation_id id,
                                   std::string detail) const;

  static bool valid_endpoint(const endpoint &value) noexcept;
  static bool valid_binding(const usbip_host_binding &value) noexcept;
  static std::optional<std::uint16_t> parse_hub_port(std::string_view output) noexcept;
  static std::string trim_ascii(std::string_view value);

  void reap_finished();
  static void join_operation(const std::shared_ptr<operation> &operation) noexcept;
  static bool operation_joined(const std::shared_ptr<operation> &operation);
  void remember_binding_locked(const usbip_host_binding &binding);
  void forget_binding_locked(const usbip_host_binding &binding);

  usbip_host_controller_config config_;
  usbip_host_backend backend_ { usbip_host_backend::unsupported };
  mutable std::mutex mutex_;
  std::condition_variable stop_condition_;
  std::vector<std::shared_ptr<operation>> operations_;
  std::vector<usbip_host_binding> accepted_bindings_;
  operation_id next_operation_id_ { 1 };
  bool stopped_ { false };
  bool stop_in_progress_ { false };
  std::mutex sweep_mutex_;
};

}  // namespace remote_usb
