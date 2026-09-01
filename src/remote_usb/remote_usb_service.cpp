/**
 * @file src/remote_usb/remote_usb_service.cpp
 * @brief Remote USB broker and usbip host lifecycle owner.
 */
#include "remote_usb_service.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "remote_usb_host_controller.h"
#include "remote_usb_tombstone.h"

namespace remote_usb {
  namespace {

    struct identity_key {
      std::uint64_t stream_generation { 0 };
      std::uint64_t session_token { 0 };
      std::uint64_t attachment_token { 0 };
      std::uint64_t lease_token { 0 };

      friend bool
      operator==(const identity_key &lhs, const identity_key &rhs) noexcept {
        return lhs.stream_generation == rhs.stream_generation &&
               lhs.session_token == rhs.session_token &&
               lhs.attachment_token == rhs.attachment_token &&
               lhs.lease_token == rhs.lease_token;
      }
    };

    struct identity_hash {
      std::size_t
      operator()(const identity_key &value) const noexcept {
        const auto mix = [](std::uint64_t input) {
          input += 0x9e3779b97f4a7c15ULL;
          input = (input ^ (input >> 30)) * 0xbf58476d1ce4e5b9ULL;
          input = (input ^ (input >> 27)) * 0x94d049bb133111ebULL;
          return input ^ (input >> 31);
        };
        const auto first = mix(value.stream_generation);
        const auto second = mix(value.session_token + first);
        const auto third = mix(value.attachment_token + second);
        const auto fourth = mix(value.lease_token + third);
        return static_cast<std::size_t>(fourth ^ (fourth >> 32));
      }
    };

    identity_key
    identity_for(const session_binding &binding) noexcept {
      return { binding.stream_generation, binding.session_token,
        binding.attachment_token, binding.lease_token };
    }

    identity_key
    identity_for(const usbip_host_binding &binding) noexcept {
      return { binding.stream_generation, binding.identity.session_token,
        binding.identity.attachment_token, binding.identity.lease_token };
    }

  }  // namespace

  struct remote_usb_service::impl {
    using binding_map = std::unordered_map<identity_key, usbip_host_binding, identity_hash>;
    using identity_set = std::unordered_set<identity_key, identity_hash>;
    using closed_set = bounded_tombstone_set<identity_key, identity_hash>;

    struct operation {
      std::uint64_t attempt { 0 };
      usbip_host_controller::operation_id id { 0 };
    };
    using operation_map = std::unordered_map<identity_key, operation, identity_hash>;

    usbip_host_controller host_controller;
    bool host_supported { false };
    bool start_attempted { false };
    capability_store capabilities;
    std::shared_ptr<broker_server> broker;
    std::mutex mutex;
    binding_map attached_bindings;
    operation_map attach_operations;
    closed_set closed_identities;
    identity_set detach_scheduled;
    std::uint64_t next_attach_attempt { 1 };

    std::uint64_t
    allocate_attempt() {
      for (;;) {
        auto candidate = next_attach_attempt++;
        if (next_attach_attempt == 0) {
          next_attach_attempt = 1;
        }
        if (candidate == 0) {
          continue;
        }
        const auto used = std::any_of(
          attach_operations.begin(), attach_operations.end(),
          [candidate](const auto &entry) { return entry.second.attempt == candidate; });
        if (!used) {
          return candidate;
        }
      }
    }

    static adapter_status
    adapter_status_for(usbip_host_status status) {
      switch (status) {
        case usbip_host_status::ok:
          return adapter_status::ok;
        case usbip_host_status::invalid_argument:
          return adapter_status::invalid_argument;
        case usbip_host_status::unsupported:
          return adapter_status::unsupported;
        case usbip_host_status::busy:
        case usbip_host_status::stopped:
        case usbip_host_status::cancelled:
          return adapter_status::invalid_state;
        case usbip_host_status::launch_failed:
        case usbip_host_status::attach_failed:
        case usbip_host_status::detach_failed:
        case usbip_host_status::timed_out:
          return adapter_status::bridge_failure;
      }
      return adapter_status::bridge_failure;
    }

    void
    schedule_detach(usbip_host_binding binding) {
      const auto identity = identity_for(binding);
      const auto retry_binding = binding;
      {
        std::lock_guard lock(mutex);
        if (!detach_scheduled.insert(identity).second) {
          return;
        }
      }
      usbip_host_controller::operation_id operation = 0;
      try {
        operation = host_controller.detach(
          std::move(binding),
          [this, identity, retry_binding](usbip_host_result result) {
            {
              std::lock_guard lock(mutex);
              detach_scheduled.erase(identity);
              if (!result.ok()) {
                attached_bindings.try_emplace(identity, retry_binding);
              }
            }
          });
      }
      catch (...) {
        std::lock_guard lock(mutex);
        detach_scheduled.erase(identity);
        return;
      }
      if (operation == 0) {
        std::lock_guard lock(mutex);
        detach_scheduled.erase(identity);
      }
    }

    void
    on_endpoint_ready(const endpoint &local_endpoint,
      const session_binding &session,
      local_endpoint_ready_completion completion) {
      const auto identity = identity_for(session);
      std::uint64_t attempt = 0;
      {
        std::lock_guard lock(mutex);
        if (attach_operations.contains(identity) || attached_bindings.contains(identity) ||
            detach_scheduled.contains(identity)) {
          completion(false, adapter_status::invalid_state);
          return;
        }
        closed_identities.erase(identity);
        attempt = allocate_attempt();
        attach_operations.emplace(identity, operation { attempt, 0 });
      }

      usbip_host_request request;
      request.server_endpoint = local_endpoint;
      request.identity = { session.session_token, session.attachment_token, session.lease_token };
      request.stream_generation = session.stream_generation;
      usbip_host_controller::operation_id operation_id = 0;
      try {
        operation_id = host_controller.attach(
          std::move(request),
          [this, identity, attempt, completion = std::move(completion)](usbip_host_result result) mutable {
            bool abandoned = false;
            {
              std::lock_guard lock(mutex);
              const auto it = attach_operations.find(identity);
              const bool current = it != attach_operations.end() && it->second.attempt == attempt;
              if (current) {
                attach_operations.erase(it);
              }
              if (result.ok() && result.binding && current && !closed_identities.contains(identity)) {
                attached_bindings[identity] = *result.binding;
              }
              else {
                abandoned = !current || closed_identities.contains(identity);
              }
            }
            if (abandoned) {
              if (result.binding) {
                schedule_detach(*result.binding);
              }
              return;
            }
            completion(result.ok(), result.ok() ? adapter_status::ok : adapter_status_for(result.status));
          });
      }
      catch (...) {
        std::lock_guard lock(mutex);
        const auto it = attach_operations.find(identity);
        if (it != attach_operations.end() && it->second.attempt == attempt) {
          attach_operations.erase(it);
        }
        throw;
      }

      if (operation_id == 0) {
        std::lock_guard lock(mutex);
        const auto it = attach_operations.find(identity);
        if (it != attach_operations.end() && it->second.attempt == attempt) {
          attach_operations.erase(it);
        }
        return;
      }
      bool cancel_now = false;
      {
        std::lock_guard lock(mutex);
        const auto it = attach_operations.find(identity);
        if (closed_identities.contains(identity) || attached_bindings.contains(identity) ||
            it == attach_operations.end() || it->second.attempt != attempt) {
          cancel_now = true;
        }
        else {
          it->second.id = operation_id;
        }
      }
      if (cancel_now) {
        (void) host_controller.cancel(operation_id);
      }
    }

    void
    on_session_closed(const session_binding &session, close_reason) {
      const auto identity = identity_for(session);
      usbip_host_controller::operation_id operation = 0;
      std::optional<usbip_host_binding> binding;
      {
        std::lock_guard lock(mutex);
        closed_identities.insert(identity);
        if (const auto it = attach_operations.find(identity); it != attach_operations.end()) {
          operation = it->second.id;
          attach_operations.erase(it);
        }
        if (const auto it = attached_bindings.find(identity); it != attached_bindings.end()) {
          binding = it->second;
          attached_bindings.erase(it);
        }
      }
      if (operation != 0) {
        (void) host_controller.cancel(operation);
      }
      if (binding) {
        schedule_detach(std::move(*binding));
      }
    }

    void
    shutdown() noexcept {
      if (broker) {
        broker->stop();
      }
      std::vector<usbip_host_binding> cleanup;
      try {
        std::lock_guard lock(mutex);
        cleanup.reserve(attached_bindings.size());
        for (auto it = attached_bindings.begin(); it != attached_bindings.end();) {
          if (detach_scheduled.contains(it->first)) {
            ++it;
            continue;
          }
          cleanup.push_back(it->second);
          it = attached_bindings.erase(it);
        }
      }
      catch (...) {
      }
      for (auto &binding : cleanup) {
        schedule_detach(std::move(binding));
      }
      host_controller.stop();
      std::lock_guard lock(mutex);
      attach_operations.clear();
      attached_bindings.clear();
      closed_identities.clear();
      detach_scheduled.clear();
      capabilities.clear();
      broker.reset();
    }
  };

  remote_usb_service::remote_usb_service(): impl_(std::make_unique<impl>()) {}
  remote_usb_service::~remote_usb_service() { stop(); }

  broker_server_result
  remote_usb_service::start(service_config config) {
    if (impl_->broker) {
      return { false, "already_started" };
    }
    impl_->start_attempted = true;
    impl_->host_supported = impl_->host_controller.backend_supported();
    if (!impl_->host_supported) {
      return { false, "unsupported_platform" };
    }

    impl_->broker = std::make_shared<broker_server>(broker_server_config {
      .bind_address = config.bind_address.empty() ? "0.0.0.0" : std::move(config.bind_address),
      .port = 0,
      .certificate_file = std::move(config.certificate_file),
      .private_key_file = std::move(config.private_key_file),
      .capabilities = &impl_->capabilities,
      .client_certificate_uuid = std::move(config.client_certificate_uuid),
      .authorize_client = [](std::string_view, const broker_hello &hello,
                            const capability &capability) { return capability_store::matches_wire_identity(capability, hello.wire_client_uuid); },
      .on_local_endpoint_ready = [state = impl_.get()](const endpoint &endpoint,
                                   const session_binding &session,
                                   local_endpoint_ready_completion completion) { state->on_endpoint_ready(endpoint, session, std::move(completion)); },
      .on_session_closed = [state = impl_.get()](const session_binding &session, close_reason reason) { state->on_session_closed(session, reason); },
    });
    const auto result = impl_->broker->start();
    if (!result) {
      impl_->broker->stop();
      impl_->broker.reset();
    }
    return result;
  }

  capability_issue_result
  remote_usb_service::issue_capability(capability_issue_request request) {
    if (!impl_->broker) {
      return { impl_->start_attempted && !impl_->host_supported ? capability_issue_status::unsupported : capability_issue_status::unavailable,
        std::nullopt };
    }
    if (!impl_->host_supported) {
      return { capability_issue_status::unsupported, std::nullopt };
    }
    if (!impl_->broker->running() || impl_->broker->bound_port() == 0 ||
        request.endpoint_host.empty()) {
      return { capability_issue_status::unavailable, std::nullopt };
    }
    auto issued = impl_->capabilities.issue(
      std::move(request.client_uuid), request.stream_generation,
      capability_endpoint { std::move(request.endpoint_host), impl_->broker->bound_port() },
      std::move(request.wire_client_uuid), request.session_token,
      request.attachment_token, request.lease_token);
    if (!issued) {
      return { capability_issue_status::limit_exceeded, std::nullopt };
    }
    return { capability_issue_status::ok, std::move(issued) };
  }

  void
  remote_usb_service::stop() noexcept {
    if (impl_) {
      impl_->shutdown();
    }
  }

  bool
  remote_usb_service::available() const noexcept {
    return impl_ && impl_->host_supported && impl_->broker && impl_->broker->running();
  }

  std::uint16_t
  remote_usb_service::bound_port() const noexcept {
    return impl_ && impl_->broker ? impl_->broker->bound_port() : 0;
  }

}  // namespace remote_usb
