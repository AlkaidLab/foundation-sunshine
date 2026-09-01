/**
 * @file src/remote_usb/remote_usb_host_controller.cpp
 * @brief Structured usbip-win2 process orchestration.
 */

#include "remote_usb_host_controller.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <limits>
#include <system_error>
#include <utility>

#include <boost/asio/ip/address.hpp>
#include <boost/process/v1.hpp>
#include <boost/process/v1/pipe.hpp>

namespace remote_usb {
namespace {

namespace asio = boost::asio;
namespace bp = boost::process::v1;
using namespace std::chrono_literals;

constexpr std::uint16_t kMaxHubPort = 255;

std::string
platform_default_executable() {
#ifdef _WIN32
  return "usbip.exe";
#else
  return "usbip";
#endif
}

usbip_host_backend
platform_default_backend() noexcept {
#ifdef _WIN32
  return usbip_host_backend::usbip_win2;
#else
  /* The generic Linux usbip CLI has a different protocol/argument contract
   * from usbip-win2.  Keep it disabled until a native profile is implemented
   * instead of reporting a misleading attach success. */
  return usbip_host_backend::unsupported;
#endif
}

void
append_bounded(std::string &destination,
               std::string_view value,
               std::size_t maximum) {
  if (destination.size() >= maximum) {
    return;
  }
  const auto remaining = maximum - destination.size();
  destination.append(value.data(), std::min(remaining, value.size()));
}

/*
 * Drain both child pipes concurrently.  usbip-win2 normally prints a single
 * line, but draining rather than relying on a fixed pipe buffer keeps a broken
 * helper from deadlocking the controller while its output is being bounded.
 */
usbip_command_result
run_process(const std::string &executable,
            const std::vector<std::string> &arguments,
            std::chrono::milliseconds timeout,
            const std::shared_ptr<std::atomic_bool> &cancel,
            std::size_t max_output_bytes,
            const usbip_reader_thread_factory &reader_thread_factory) {
  usbip_command_result result;
  bp::ipstream standard_output;
  bp::ipstream standard_error;
  std::error_code launch_error;
  bp::child child;

  try {
    child = bp::child(executable,
                      bp::args(arguments),
                      bp::std_in < bp::null,
                      bp::std_out > standard_output,
                      bp::std_err > standard_error,
                      launch_error);
  }
  catch (const std::exception &exception) {
    result.standard_error = exception.what();
    return result;
  }
  if (launch_error || !child.valid()) {
    result.standard_error = launch_error ? launch_error.message() : "process is invalid";
    return result;
  }

  std::thread output_reader;
  std::thread error_reader;
  try {
    output_reader = reader_thread_factory([&]() {
      std::string line;
      while (std::getline(standard_output, line)) {
        append_bounded(result.standard_output, line, max_output_bytes);
        if (result.standard_output.size() < max_output_bytes) {
          append_bounded(result.standard_output, "\n", max_output_bytes);
        }
      }
    });
    error_reader = reader_thread_factory([&]() {
      std::string line;
      while (std::getline(standard_error, line)) {
        append_bounded(result.standard_error, line, max_output_bytes);
        if (result.standard_error.size() < max_output_bytes) {
          append_bounded(result.standard_error, "\n", max_output_bytes);
        }
      }
    });
  }
  catch (const std::exception &exception) {
    std::error_code ignored;
    child.terminate(ignored);
    child.wait(ignored);
    if (output_reader.joinable()) {
      output_reader.join();
    }
    if (error_reader.joinable()) {
      error_reader.join();
    }
    result.exit_code = child.exit_code();
    result.standard_error = exception.what();
    return result;
  }
  catch (...) {
    std::error_code ignored;
    child.terminate(ignored);
    child.wait(ignored);
    if (output_reader.joinable()) {
      output_reader.join();
    }
    if (error_reader.joinable()) {
      error_reader.join();
    }
    result.exit_code = child.exit_code();
    result.standard_error = "unable to create process output reader";
    return result;
  }

  const auto bounded_timeout = timeout <= 0ms ? 1ms : timeout;
  const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;
  bool terminated = false;
  while (child.running()) {
    if (cancel && cancel->load(std::memory_order_acquire)) {
      result.cancelled = true;
      terminated = true;
      std::error_code ignored;
      child.terminate(ignored);
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      terminated = true;
      std::error_code ignored;
      child.terminate(ignored);
      break;
    }
    std::this_thread::sleep_for(5ms);
  }

  std::error_code wait_error;
  child.wait(wait_error);
  output_reader.join();
  error_reader.join();
  if (wait_error && !terminated) {
    result.standard_error = wait_error.message();
  }
  result.exit_code = child.exit_code();
  return result;
}

usbip_host_result
map_command_failure(const usbip_command_result &command,
                    usbip_host_status ordinary_failure,
                    std::uint64_t operation_id) {
  usbip_host_result result;
  result.operation_id = operation_id;
  if (command.cancelled) {
    result.status = usbip_host_status::cancelled;
    result.detail = "usbip operation cancelled";
  }
  else if (command.timed_out) {
    result.status = usbip_host_status::timed_out;
    result.detail = "usbip operation timed out";
  }
  else if (command.exit_code < 0) {
    result.status = usbip_host_status::launch_failed;
    result.detail = command.standard_error.empty() ?
                      "unable to launch usbip helper" : command.standard_error;
  }
  else {
    result.status = ordinary_failure;
    result.detail = command.standard_error.empty() ?
                      "usbip helper returned a failure" : command.standard_error;
  }
  return result;
}

}  // namespace

usbip_host_controller::usbip_host_controller(usbip_host_controller_config config):
    config_(std::move(config)) {
  /* An injected runner is intentionally treated as an explicit backend for
   * tests and compatibility layers.  The production path has no runner and
   * therefore follows the compile-time platform default above. */
  const bool injected_runner = static_cast<bool>(config_.command_runner);
  backend_ = config_.backend == usbip_host_backend::automatic
               ? (injected_runner ? usbip_host_backend::usbip_win2
                                  : platform_default_backend())
               : config_.backend;
  if (config_.executable.empty()) {
    config_.executable = default_executable();
  }
  if (config_.attach_timeout <= std::chrono::milliseconds::zero()) {
    config_.attach_timeout = 1ms;
  }
  if (config_.detach_timeout <= std::chrono::milliseconds::zero()) {
    config_.detach_timeout = 1ms;
  }
  if (config_.max_output_bytes == 0) {
    config_.max_output_bytes = 1024;
  }
  if (config_.max_concurrent_operations == 0) {
    config_.max_concurrent_operations = 1;
  }
  if (!config_.reader_thread_factory) {
    config_.reader_thread_factory = [](std::function<void()> function) {
      return std::thread(std::move(function));
    };
  }
  if (!config_.command_runner) {
    config_.command_runner =
      [reader_thread_factory = config_.reader_thread_factory](
        const std::string &executable,
        const std::vector<std::string> &arguments,
        std::chrono::milliseconds timeout,
        const std::shared_ptr<std::atomic_bool> &cancel,
        std::size_t max_output_bytes) {
        return run_process(executable, arguments, timeout, cancel,
                           max_output_bytes, reader_thread_factory);
      };
  }
}

usbip_host_controller::~usbip_host_controller() {
  stop();
}

usbip_host_controller::operation_id
usbip_host_controller::attach(usbip_host_request request,
                              usbip_host_completion completion) {
  return dispatch(operation_kind::attach,
                  std::move(request),
                  usbip_host_binding {},
                  std::move(completion));
}

usbip_host_controller::operation_id
usbip_host_controller::detach(usbip_host_binding binding,
                              usbip_host_completion completion) {
  return dispatch(operation_kind::detach,
                  usbip_host_request {},
                  std::move(binding),
                  std::move(completion));
}

usbip_host_controller::operation_id
usbip_host_controller::dispatch(operation_kind kind,
                                usbip_host_request request,
                                usbip_host_binding binding,
                                usbip_host_completion completion) {
  if (!completion) {
    return 0;
  }

  const bool valid_request = kind == operation_kind::attach
                               ? valid_endpoint(request.server_endpoint) &&
                                   valid_identity(request.identity)
                               : valid_binding(binding);
  if (!valid_request) {
    usbip_host_result result;
    result.status = usbip_host_status::invalid_argument;
    result.detail = "invalid usbip host endpoint or lease identity";
    try {
      completion(std::move(result));
    }
    catch (...) {
      /* User callbacks must not escape the controller API. */
    }
    return 0;
  }

  std::shared_ptr<operation> op;
  std::shared_ptr<usbip_host_completion> completion_holder;
  std::optional<usbip_host_result> immediate_result;
  /* Reap outside controller mutex_.  A worker's completion callback is user
   * code and may call back into this controller; joining while holding the
   * mutex can deadlock that callback even after the operation is marked done. */
  reap_finished();
  {
    std::lock_guard lock(mutex_);
    if (stopped_) {
      immediate_result = invalid_result(usbip_host_status::stopped,
                                         0,
                                         "usbip host controller is stopped");
    }
    else if (!backend_supported()) {
      immediate_result = invalid_result(
        usbip_host_status::unsupported,
        0,
        "no compatible host-side USB/IP backend is enabled");
    }
    else {
      std::size_t active = 0;
      for (const auto &candidate : operations_) {
        const bool candidate_active =
          !candidate->command_finished.load(std::memory_order_acquire);
        if (candidate_active) {
          ++active;
        }
        if (candidate_active &&
            ((kind == operation_kind::attach &&
              candidate->request.identity == request.identity &&
              candidate->request.stream_generation == request.stream_generation) ||
             (kind == operation_kind::detach &&
              candidate->binding.identity == binding.identity &&
              candidate->binding.stream_generation == binding.stream_generation))) {
          immediate_result = invalid_result(usbip_host_status::busy,
                                             0,
                                             "the lease already has an operation");
          break;
        }
      }
      if (!immediate_result && kind == operation_kind::attach) {
        const auto accepted = std::find_if(
          accepted_bindings_.begin(), accepted_bindings_.end(),
          [&request](const usbip_host_binding &candidate) {
            return candidate.identity == request.identity &&
                   candidate.stream_generation == request.stream_generation;
          });
        if (accepted != accepted_bindings_.end()) {
          immediate_result = invalid_result(
            usbip_host_status::busy, 0,
            "the lease is already attached");
        }
      }
      if (!immediate_result && kind == operation_kind::detach) {
        const auto accepted = std::find_if(
          accepted_bindings_.begin(), accepted_bindings_.end(),
          [&binding](const usbip_host_binding &candidate) {
            return candidate.identity == binding.identity &&
                   candidate.server_endpoint.address == binding.server_endpoint.address &&
                   candidate.server_endpoint.port == binding.server_endpoint.port &&
                   candidate.server_endpoint.busid == binding.server_endpoint.busid &&
                   candidate.hub_port == binding.hub_port &&
                   candidate.stream_generation == binding.stream_generation;
          });
        if (accepted == accepted_bindings_.end()) {
          immediate_result = invalid_result(
            usbip_host_status::invalid_argument, 0,
            "the lease is not attached by this controller");
        }
      }
      if (!immediate_result && active >= config_.max_concurrent_operations) {
        immediate_result = invalid_result(usbip_host_status::busy,
                                           0,
                                           "too many concurrent usbip operations");
      }
      if (!immediate_result) {
        op = std::make_shared<usbip_host_controller::operation>();
        /* IDs are never zero and must not collide with an operation that is
         * still retained for joining when the counter wraps. */
        const auto allocate_id = [this]() {
          for (;;) {
            auto candidate = next_operation_id_++;
            if (next_operation_id_ == 0) {
              next_operation_id_ = 1;
            }
            if (candidate == 0) {
              continue;
            }
            const auto collision = std::find_if(
              operations_.begin(), operations_.end(),
              [candidate](const std::shared_ptr<operation> &existing) {
                return existing->id == candidate;
              });
            if (collision == operations_.end()) {
              return candidate;
            }
          }
        };
        op->id = allocate_id();
        op->kind = kind;
        op->request = std::move(request);
        op->binding = std::move(binding);
        operations_.push_back(op);
        completion_holder = std::make_shared<usbip_host_completion>(
          std::move(completion));
        try {
          op->worker = std::thread(
            [this, op, completion_holder]() mutable {
              {
                std::lock_guard lock(op->join_mutex);
                op->worker_id = std::this_thread::get_id();
              }
              usbip_host_result result;
              result.operation_id = op->id;
              bool cleanup_attempted = false;

              /* Keep the compensating path exception-free.  A runner supplied
               * by an integration layer is allowed to throw, and a failed
               * bookkeeping allocation must not strand a virtual hub port or
               * escape the worker thread. */
              const auto compensate_attach = [&]() noexcept {
                if (cleanup_attempted || op->kind != operation_kind::attach ||
                    !result.binding) {
                  return;
                }
                cleanup_attempted = true;
                try {
                  const auto leaked_binding = *result.binding;
                  {
                    std::lock_guard lock(mutex_);
                    forget_binding_locked(leaked_binding);
                  }
                  auto cleanup = std::make_shared<operation>();
                  cleanup->id = op->id;
                  cleanup->kind = operation_kind::detach;
                  cleanup->binding = leaked_binding;
                  cleanup->cancel = std::make_shared<std::atomic_bool>(false);
                  const auto cleanup_result = run_detach(cleanup);
                  if (!cleanup_result.ok()) {
                    std::lock_guard lock(mutex_);
                    remember_binding_locked(leaked_binding);
                  }
                }
                catch (...) {
                  /* Best effort only: preserve the terminal operation result
                   * and let the owner-level cleanup retry if possible. */
                }
              };

              const auto set_failure_detail = [&result](std::string_view detail) noexcept {
                result.detail.clear();
                try {
                  result.detail.assign(detail.data(), detail.size());
                }
                catch (...) {
                  /* An allocation failure in diagnostics must not terminate a
                   * worker that is already reporting an operation failure. */
                }
              };

              try {
                {
                  std::unique_lock start_lock(op->start_mutex);
                  op->start_condition.wait(start_lock, [&op]() {
                    return op->start_released;
                  });
                }
                result = op->kind == operation_kind::attach
                              ? run_attach(op)
                              : run_detach(op);
                result.operation_id = op->id;

                bool detach_after_cancel = false;
                usbip_host_binding attached_binding;
                if (op->kind == operation_kind::attach && result.ok() && result.binding) {
                  attached_binding = *result.binding;
                  {
                    std::lock_guard lock(mutex_);
                    if (stopped_ || op->cancel->load(std::memory_order_acquire)) {
                      detach_after_cancel = true;
                    }
                    else {
                      remember_binding_locked(attached_binding);
                    }
                  }
                  if (detach_after_cancel) {
                    /* A stop/cancel can race the final attach acknowledgement.
                     * Do not leak a virtual device when that happens. */
                    cleanup_attempted = true;
                    auto cleanup = std::make_shared<operation>();
                    cleanup->id = op->id;
                    cleanup->kind = operation_kind::detach;
                    cleanup->binding = attached_binding;
                    /* The attach token is already cancelled at this point.  A
                     * compensating detach must get its own live cancellation
                     * token; otherwise the default runner would terminate the
                     * cleanup helper before it can release the virtual port. */
                    cleanup->cancel = std::make_shared<std::atomic_bool>(false);
                    const auto cleanup_result = run_detach(cleanup);
                    if (!cleanup_result.ok()) {
                      /* stop() snapshots accepted bindings after workers join.
                       * Retaining a failed compensation here gives that final
                       * cleanup pass a binding it can retry. */
                      std::lock_guard lock(mutex_);
                      remember_binding_locked(attached_binding);
                    }
                    result.status = op->cancel->load(std::memory_order_acquire)
                                      ? usbip_host_status::cancelled
                                      : usbip_host_status::stopped;
                    result.detail = "attach completed after controller shutdown";
                  }
                }

                if (op->kind == operation_kind::detach && result.ok()) {
                  std::lock_guard lock(mutex_);
                  forget_binding_locked(op->binding);
                }
              }
              catch (const std::exception &exception) {
                /* A custom runner or an allocation failure must never escape
                 * the worker thread.  If attach returned a hub port before a
                 * bookkeeping exception, make one best-effort detach attempt
                 * with a fresh live cancellation token. */
                compensate_attach();
                result.status = op->kind == operation_kind::attach
                                  ? usbip_host_status::attach_failed
                                  : usbip_host_status::detach_failed;
                result.binding.reset();
                set_failure_detail(exception.what());
              }
              catch (...) {
                compensate_attach();
                result.status = op->kind == operation_kind::attach
                                  ? usbip_host_status::attach_failed
                                  : usbip_host_status::detach_failed;
                result.binding.reset();
                set_failure_detail("usbip host operation failed");
              }

              /* The process and lease bookkeeping are complete before user
               * code runs.  Expose that fact to dispatch so a completion
               * callback can synchronously schedule the matching detach (for
               * example, when the broker closed while attach was in flight).
               * `finished` remains false until the callback returns, which
               * prevents another thread from joining a worker whose callback
               * may re-enter this controller. */
              op->command_finished.store(true, std::memory_order_release);
              try {
                (*completion_holder)(std::move(result));
              }
              catch (...) {
                /* A callback is an observation boundary, never a worker failure. */
              }
              op->finished.store(true, std::memory_order_release);
            });
        }
        catch (...) {
          operations_.pop_back();
          op.reset();
          immediate_result = invalid_result(usbip_host_status::launch_failed,
                                             0,
                                             "unable to create usbip worker");
        }
      }
    }
  }

  if (immediate_result) {
    try {
      if (completion_holder) {
        (*completion_holder)(std::move(*immediate_result));
      }
      else {
        completion(std::move(*immediate_result));
      }
    }
    catch (...) {
    }
    return 0;
  }

  const auto id = op->id;
  {
    std::lock_guard start_lock(op->start_mutex);
    op->start_released = true;
  }
  op->start_condition.notify_one();
  return id;
}

bool
usbip_host_controller::cancel(operation_id id) {
  if (id == 0) {
    return false;
  }
  std::lock_guard lock(mutex_);
  for (const auto &operation : operations_) {
    if (operation->id == id &&
        !operation->command_finished.load(std::memory_order_acquire)) {
      operation->cancel->store(true, std::memory_order_release);
      return true;
    }
  }
  return false;
}

void
usbip_host_controller::stop() noexcept {
  std::vector<std::shared_ptr<operation>> operations;
  std::vector<usbip_host_binding> bindings;
  bool owns_stop = false;
  const auto release_operation = [](const std::shared_ptr<operation> &operation) noexcept {
    if (!operation) {
      return;
    }
    operation->cancel->store(true, std::memory_order_release);
    try {
      std::lock_guard start_lock(operation->start_mutex);
      operation->start_released = true;
    }
    catch (...) {
      /* A start mutex failure is exceptionally rare, but cancellation is
       * already visible and the worker remains retained for a later retry. */
    }
    operation->start_condition.notify_one();
  };
  const auto release_registered = [this, &release_operation]() noexcept {
    try {
      std::lock_guard lock(mutex_);
      for (const auto &operation : operations_) {
        release_operation(operation);
      }
    }
    catch (...) {
      /* stop() is a destructor-safe boundary. */
    }
  };
  const auto detach_binding = [this](const usbip_host_binding &binding) noexcept {
    try {
      auto cleanup = std::make_shared<operation>();
      cleanup->kind = operation_kind::detach;
      cleanup->binding = binding;
      cleanup->cancel = std::make_shared<std::atomic_bool>(false);
      const auto result = run_detach(cleanup);
      if (!result.ok()) {
        std::lock_guard lock(mutex_);
        remember_binding_locked(binding);
      }
    }
    catch (...) {
      /* Best effort only.  Never let cleanup tear down the owner. */
    }
  };
  const auto erase_joined = [this]() noexcept {
    try {
      std::lock_guard lock(mutex_);
      operations_.erase(
        std::remove_if(operations_.begin(), operations_.end(),
          [](const std::shared_ptr<operation> &operation) {
            return operation_joined(operation);
          }),
        operations_.end());
    }
    catch (...) {
      /* Keep any still-joinable operation registered for a future stop(). */
    }
  };
  const auto finish_stop = [this]() noexcept {
    try {
      {
        std::lock_guard lock(mutex_);
        stop_in_progress_ = false;
      }
      stop_condition_.notify_all();
    }
    catch (...) {
      /* No exception may escape a destructor-safe stop path. */
    }
  };

  try {
    {
      std::unique_lock lock(mutex_);
      if (stop_in_progress_) {
        /* A completion callback may call stop() from one of our worker threads
         * while an external caller is already joining it.  Waiting in that
         * callback would deadlock the joiner, so identify worker callers via
         * the per-operation ID protected by join_mutex.  Non-worker callers
         * wait for the first stop to finish; returning early would let a
         * destructor race the still-running cleanup sequence. */
        const auto caller = std::this_thread::get_id();
        bool called_from_worker = false;
        for (const auto &operation : operations_) {
          std::lock_guard operation_lock(operation->join_mutex);
          if (operation->worker_id == caller) {
            called_from_worker = true;
            break;
          }
        }
        if (called_from_worker) {
          return;
        }
        stop_condition_.wait(lock, [this]() { return !stop_in_progress_; });
        return;
      }
      if (!stopped_) {
        stop_in_progress_ = true;
        stopped_ = true;
        owns_stop = true;
      }
      else {
        /* A previous stop may have had to leave a worker registered after a
         * platform-level join/detach failure.  Re-entering stop() must retry
         * that worker instead of returning into its joinable destructor. */
        stop_in_progress_ = true;
        owns_stop = true;
      }
      operations = operations_;
      for (const auto &operation : operations) {
        release_operation(operation);
      }
    }

    for (const auto &operation : operations) {
      join_operation(operation);
    }

    /* Take the accepted snapshot only after in-flight detach workers have
     * joined.  Otherwise a successful detach racing stop() would be issued a
     * second time from the stale pre-join snapshot. */
    {
      std::lock_guard lock(mutex_);
      bindings = std::move(accepted_bindings_);
      accepted_bindings_.clear();
    }

    /* Detach anything that remains accepted after all workers have quiesced. */
    for (const auto &binding : bindings) {
      detach_binding(binding);
    }

    /* Keep an operation registered if a self-detach or join failed.  The
     * controller may be stopped again after the worker has returned, at which
     * point a normal external join can complete it. */
    erase_joined();
  }
  catch (...) {
    if (!owns_stop) {
      /* A concurrent stop owns the cleanup and will publish completion via
       * stop_condition_.  Do not clear its in-progress marker here. */
      return;
    }
    /* Snapshot allocation, a condition-variable failure, or an unexpected
     * platform exception must not leave a joinable std::thread behind.  Use
     * the partial snapshot first, then walk the live registry without another
     * allocation. */
    release_registered();
    for (const auto &operation : operations) {
      join_operation(operation);
    }
    std::size_t index = 0;
    for (;;) {
      std::shared_ptr<operation> candidate;
      try {
        {
          std::lock_guard lock(mutex_);
          if (index >= operations_.size()) {
            break;
          }
          candidate = operations_[index];
        }
      }
      catch (...) {
        break;
      }
      join_operation(candidate);
      ++index;
    }
    if (bindings.empty()) {
      try {
        std::lock_guard lock(mutex_);
        bindings = std::move(accepted_bindings_);
        accepted_bindings_.clear();
      }
      catch (...) {
        /* Leave the registry intact for a subsequent stop() attempt. */
      }
    }
    for (const auto &binding : bindings) {
      detach_binding(binding);
    }
    finish_stop();
    erase_joined();
    return;
  }
  finish_stop();
}

bool
usbip_host_controller::stopped() const noexcept {
  std::lock_guard lock(mutex_);
  return stopped_;
}

std::size_t
usbip_host_controller::active_operations() const noexcept {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  for (const auto &operation : operations_) {
    if (!operation->command_finished.load(std::memory_order_acquire)) {
      ++count;
    }
  }
  return count;
}

std::string
usbip_host_controller::default_executable() {
  return platform_default_executable();
}

usbip_host_backend
usbip_host_controller::backend() const noexcept {
  return backend_;
}

bool
usbip_host_controller::backend_supported() const noexcept {
  return backend_ == usbip_host_backend::usbip_win2;
}

usbip_host_result
usbip_host_controller::run_attach(const std::shared_ptr<operation> &operation) {
  usbip_host_result result;
  result.operation_id = operation->id;
  if (!backend_supported()) {
    result.status = usbip_host_status::unsupported;
    result.detail = "no compatible host-side USB/IP backend is enabled";
    return result;
  }
  const auto &request = operation->request;
  if (!valid_endpoint(request.server_endpoint) || !valid_identity(request.identity)) {
    result.status = usbip_host_status::invalid_argument;
    result.detail = "invalid usbip attach request";
    return result;
  }

  /* usbip-win2 parses --tcp-port as a global option, before the subcommand.
   * `-t` belongs to the attach command's terse switch and treating it as a
   * port silently changes the argument stream (and is rejected by newer
   * clients).  Keep the complete argv explicit so no shell quoting is ever
   * involved. */
  const std::vector<std::string> arguments {
    "--tcp-port", std::to_string(request.server_endpoint.port),
    "attach", "--remote", request.server_endpoint.address,
    "--bus-id", request.server_endpoint.busid,
    "--once", "--terse", "--receive-mode", "zero-copy"
  };
  usbip_command_result command;
  try {
    command = config_.command_runner(
      config_.executable, arguments, config_.attach_timeout,
      operation->cancel, config_.max_output_bytes);
  }
  catch (const std::exception &exception) {
    result.status = usbip_host_status::attach_failed;
    try {
      result.detail = exception.what();
    }
    catch (...) {
      result.detail.clear();
    }
    return result;
  }
  catch (...) {
    result.status = usbip_host_status::attach_failed;
    result.detail.clear();
    return result;
  }
  if (command.cancelled || command.timed_out || command.exit_code < 0 || command.exit_code != 0) {
    return map_command_failure(command, usbip_host_status::attach_failed, operation->id);
  }

  const auto hub_port = parse_hub_port(command.standard_output);
  if (!hub_port) {
    result.status = usbip_host_status::attach_failed;
    result.detail = "usbip attach did not return a valid hub port";
    return result;
  }
  result.status = usbip_host_status::ok;
  result.binding = usbip_host_binding {
    request.server_endpoint,
    request.identity,
    *hub_port,
    request.stream_generation,
  };
  return result;
}

usbip_host_result
usbip_host_controller::run_detach(const std::shared_ptr<operation> &operation) {
  usbip_host_result result;
  result.operation_id = operation->id;
  if (!backend_supported()) {
    result.status = usbip_host_status::unsupported;
    result.detail = "no compatible host-side USB/IP backend is enabled";
    return result;
  }
  if (!valid_binding(operation->binding)) {
    result.status = usbip_host_status::invalid_argument;
    result.detail = "invalid usbip detach binding";
    return result;
  }
  const std::vector<std::string> arguments {
    "detach", "--port", std::to_string(operation->binding.hub_port)
  };
  usbip_command_result command;
  try {
    command = config_.command_runner(
      config_.executable, arguments, config_.detach_timeout,
      operation->cancel, config_.max_output_bytes);
  }
  catch (const std::exception &exception) {
    result.status = usbip_host_status::detach_failed;
    try {
      result.detail = exception.what();
    }
    catch (...) {
      result.detail.clear();
    }
    return result;
  }
  catch (...) {
    result.status = usbip_host_status::detach_failed;
    result.detail.clear();
    return result;
  }
  if (command.cancelled || command.timed_out || command.exit_code < 0 || command.exit_code != 0) {
    return map_command_failure(command, usbip_host_status::detach_failed, operation->id);
  }
  result.status = usbip_host_status::ok;
  result.binding = operation->binding;
  return result;
}

usbip_host_result
usbip_host_controller::invalid_result(usbip_host_status status,
                                      operation_id id,
                                      std::string detail) const {
  usbip_host_result result;
  result.status = status;
  result.operation_id = id;
  result.detail = std::move(detail);
  return result;
}

bool
usbip_host_controller::valid_endpoint(const endpoint &value) noexcept {
  if (value.port == 0 || value.busid.empty() || value.busid.size() > 31) {
    return false;
  }
  boost::system::error_code error;
  const auto address = asio::ip::make_address(value.address, error);
  if (error || !address.is_loopback()) {
    return false;
  }
  return std::all_of(value.busid.begin(), value.busid.end(), [](unsigned char byte) {
    return byte >= 0x21u && byte <= 0x7eu;
  });
}

bool
usbip_host_controller::valid_identity(const usbip_host_identity &value) noexcept {
  return value.session_token != 0 && value.attachment_token != 0 && value.lease_token != 0;
}

bool
usbip_host_controller::valid_binding(const usbip_host_binding &value) noexcept {
  return valid_endpoint(value.server_endpoint) && valid_identity(value.identity) &&
         value.hub_port >= 1 && value.hub_port <= kMaxHubPort;
}

std::optional<std::uint16_t>
usbip_host_controller::parse_hub_port(std::string_view output) noexcept {
  const auto value = trim_ascii(output);
  if (value.empty() || value.size() > 3) {
    return std::nullopt;
  }
  std::uint32_t port = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port, 10);
  if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() ||
      port == 0 || port > kMaxHubPort) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(port);
}

std::string
usbip_host_controller::trim_ascii(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return std::string { value };
}

void
usbip_host_controller::join_operation(
  const std::shared_ptr<operation> &operation) noexcept {
  if (!operation) {
    return;
  }

  const auto caller = std::this_thread::get_id();
  for (;;) {
    bool self = false;
    {
      std::unique_lock lock(operation->join_mutex);
      if (operation->joined) {
        return;
      }

      /* `worker_id` is written by the worker before it waits for dispatch.
       * It remains set through the completion callback, so a callback can
       * safely call stop() without touching std::thread concurrently. */
      self = operation->worker_id == caller ||
             (operation->worker.joinable() && operation->worker.get_id() == caller);
      if (self) {
        if (operation->join_claimed) {
          /* An external reaper owns the join.  Waiting here would deadlock
           * that reaper while this callback is still on its stack. */
          return;
        }
        operation->join_claimed = true;
      }
      else {
        if (operation->join_claimed) {
          /* A failed join/detach releases the claim and lets another caller
           * retry.  Include that state in the predicate so a waiter cannot
           * sleep forever after a platform-level std::system_error. */
          try {
            operation->join_condition.wait(lock, [&operation]() {
              return operation->joined || !operation->join_claimed;
            });
          }
          catch (...) {
            /* A condition-variable failure must not escape stop().  The
             * owner of the claim will still complete or release it. */
            return;
          }
          if (operation->joined) {
            return;
          }
          continue;
        }
        operation->join_claimed = true;
      }
    }

    bool completed = false;
    if (self) {
      /* A worker cannot join itself.  Detach is normally infallible once the
       * joinable check succeeded, but retain the operation for an external
       * retry if the platform reports an error. */
      try {
        if (operation->worker.joinable()) {
          operation->worker.detach();
        }
        completed = true;
      }
      catch (...) {
        completed = false;
      }
    }
    else {
      /* Never hold join_mutex while join() runs.  A worker completion can
       * re-enter stop(), observe the claim, and return without blocking. */
      try {
        if (operation->worker.joinable()) {
          operation->worker.join();
        }
        completed = true;
      }
      catch (...) {
        /* Detaching is the only safe fallback once ownership was claimed.
         * If that also fails, release the claim and leave the operation in
         * the registry so a later stop/reaper can retry instead of allowing a
         * joinable std::thread destructor to call terminate. */
        try {
          if (operation->worker.joinable()) {
            operation->worker.detach();
          }
          completed = true;
        }
        catch (...) {
          completed = false;
        }
      }
    }

    {
      std::lock_guard lock(operation->join_mutex);
      if (completed) {
        operation->joined = true;
      }
      else {
        operation->join_claimed = false;
      }
    }
    operation->join_condition.notify_all();
    return;
  }
}

bool
usbip_host_controller::operation_joined(
  const std::shared_ptr<operation> &operation) {
  if (!operation) {
    return true;
  }
  std::lock_guard lock(operation->join_mutex);
  return operation->joined;
}

void
usbip_host_controller::reap_finished() {
  std::vector<std::shared_ptr<operation>> finished;
  {
    std::lock_guard lock(mutex_);
    for (const auto &operation : operations_) {
      if (operation->finished.load(std::memory_order_acquire)) {
        finished.push_back(operation);
      }
    }
  }

  /* Joining happens without controller mutex_, so a completion that re-enters
   * active_operations()/attach()/stop() cannot wait on a lock held by its
   * reaper. */
  for (const auto &operation : finished) {
    join_operation(operation);
  }

  std::lock_guard lock(mutex_);
  operations_.erase(
    std::remove_if(operations_.begin(), operations_.end(),
      [](const std::shared_ptr<operation> &operation) {
        return operation->finished.load(std::memory_order_acquire) &&
               operation_joined(operation);
      }),
    operations_.end());
}

void
usbip_host_controller::remember_binding_locked(const usbip_host_binding &binding) {
  const auto exists = std::find_if(
    accepted_bindings_.begin(), accepted_bindings_.end(),
    [&binding](const usbip_host_binding &candidate) {
      return candidate.identity == binding.identity &&
             candidate.stream_generation == binding.stream_generation;
    });
  if (exists == accepted_bindings_.end()) {
    accepted_bindings_.push_back(binding);
  }
}

void
usbip_host_controller::forget_binding_locked(const usbip_host_binding &binding) {
  accepted_bindings_.erase(
    std::remove_if(accepted_bindings_.begin(), accepted_bindings_.end(),
      [&binding](const usbip_host_binding &candidate) {
        return candidate.identity == binding.identity &&
               candidate.hub_port == binding.hub_port &&
               candidate.stream_generation == binding.stream_generation;
      }),
    accepted_bindings_.end());
}

}  // namespace remote_usb
