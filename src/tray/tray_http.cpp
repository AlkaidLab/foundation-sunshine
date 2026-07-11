#include "tray_http.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/display_device/session.h"
#include "src/globals.h"
#include "src/http_util.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/thread_pool.h"
#include "tray_state.h"

namespace tray_http {
  using namespace std::literals;

  namespace {
    std::atomic<bool> tray_vdd_action_cooldown { false };
    std::atomic<bool> tray_vdd_action_running { false };
    std::atomic<std::uint64_t> tray_vdd_confirmation_operation_id { 0 };
    std::atomic<bool> tray_app_termination_running { false };
    std::mutex tray_action_mutex;

    thread_pool_util::ThreadPool &
    tray_action_executor() {
      static thread_pool_util::ThreadPool executor { 1 };
      return executor;
    }

    struct tray_subscriber_t {
      resp_https_t response;
      std::atomic_bool alive { true };
      std::mutex send_mutex;
    };

    std::mutex tray_subscribers_mutex;
    std::vector<std::shared_ptr<tray_subscriber_t>> tray_subscribers;
    std::atomic_bool tray_keepalive_started { false };

    std::string
    tray_state_event() {
      return "event: tray-state\ndata: " + tray_state::to_json().dump() + "\n\n";
    }

    void
    send_tray_event(const std::shared_ptr<tray_subscriber_t> &subscriber, const std::string &event) {
      if (!subscriber->alive.load(std::memory_order_acquire)) {
        return;
      }
      try {
        std::lock_guard lock { subscriber->send_mutex };
        if (!subscriber->alive.load(std::memory_order_acquire)) {
          return;
        }
        *subscriber->response << event;
        subscriber->response->send([subscriber](const SimpleWeb::error_code &error) {
          if (error) {
            subscriber->alive.store(false, std::memory_order_release);
          }
        });
      }
      catch (const std::exception &e) {
        BOOST_LOG(debug) << "tray SSE write failed: "sv << e.what();
        subscriber->alive.store(false, std::memory_order_release);
      }
    }

    void
    publish_tray_event(const std::string &event) {
      std::vector<std::shared_ptr<tray_subscriber_t>> subscribers;
      {
        std::lock_guard lock { tray_subscribers_mutex };
        tray_subscribers.erase(
          std::remove_if(tray_subscribers.begin(), tray_subscribers.end(), [](const auto &subscriber) {
            return !subscriber->alive.load(std::memory_order_acquire);
          }),
          tray_subscribers.end());
        subscribers = tray_subscribers;
      }
      for (const auto &subscriber : subscribers) {
        send_tray_event(subscriber, event);
      }
    }

    void
    publish_tray_state() {
      publish_tray_event(tray_state_event());
    }

    void
    schedule_tray_keepalive() {
      task_pool.pushDelayed([]() {
        publish_tray_event(": tray-keepalive\n\n");
        schedule_tray_keepalive();
      },
        30s);
    }

    SimpleWeb::CaseInsensitiveMultimap
    json_response_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      return headers;
    }

    void
    send_json(resp_https_t response, const nlohmann::json &body) {
      auto headers = json_response_headers();
      response->write(body.dump(), headers);
    }

    void
    send_bad_request(resp_https_t response, const std::string &error) {
      auto headers = json_response_headers();
      response->write(SimpleWeb::StatusCode::client_error_bad_request, nlohmann::json {
                                                                         { "status", false },
                                                                         { "error", error },
                                                                       }
                                                                         .dump(),
        headers);
    }

    bool
    check_json_content_type(resp_https_t response, req_https_t request) {
      const auto request_content_type = request->header.find("content-type");
      if (request_content_type == request->header.end()) {
        send_bad_request(std::move(response), "Content type not provided");
        return false;
      }

      if (!http_util::content_type_matches(request_content_type->second, "application/json")) {
        send_bad_request(std::move(response), "Content type mismatch");
        return false;
      }

      return true;
    }

    nlohmann::json
    action_success(const std::string_view message, const std::optional<std::uint64_t> operation_id = std::nullopt) {
      nlohmann::json result {
        { "status", true },
        { "message", message },
      };
      if (operation_id) {
        result["operation_id"] = *operation_id;
      }
      return result;
    }

    nlohmann::json
    action_error(const std::string_view error) {
      return {
        { "status", false },
        { "error", error },
      };
    }

    bool
    tray_vdd_active() {
      return !display_device::find_device_by_friendlyname(ZAKO_NAME).empty();
    }

    void
    update_tray_vdd_state() {
      tray_state::set_vdd_state(
        tray_vdd_active(),
        config::video.vdd_keep_enabled,
        config::video.vdd_headless_create_enabled,
        tray_vdd_action_running.load() || tray_vdd_action_cooldown.load());
    }

    void
    finish_tray_vdd_action(const std::uint64_t operation_id, bool success, std::string_view error) {
      tray_vdd_action_cooldown = true;
      tray_vdd_action_running = false;
      update_tray_vdd_state();

      if (!success) {
        tray_state::set_notification("Virtual display", std::string { error }, "default");
      }
      tray_state::complete_operation(
        operation_id,
        success,
        success ? "Virtual display action completed" : "",
        success ? "" : std::string { error });

      task_pool.pushDelayed([]() {
        tray_vdd_action_cooldown = false;
        update_tray_vdd_state();
      },
        10s);
    }

    void
    schedule_vdd_confirmation_timeout(std::uint64_t operation_id);

    bool
    dispatch_tray_vdd_action(bool create, const std::uint64_t operation_id) {
      try {
        tray_action_executor().push([create, operation_id]() {
          const auto action_name = create ? "create"sv : "close"sv;
          BOOST_LOG(info) << (create ? "Creating"sv : "Closing"sv) << " VDD from GUI tray"sv;

          try {
            const bool success = create ?
                                   display_device::session_t::get().create_vdd_monitor_noninteractive() :
                                   display_device::session_t::get().destroy_vdd_monitor();
            if (create && success) {
              tray_vdd_action_running = false;
              tray_vdd_confirmation_operation_id = operation_id;
              tray_state::set_vdd_confirmation(true, operation_id);
              update_tray_vdd_state();
              tray_state::complete_operation(operation_id, true, "Virtual display created; awaiting confirmation");
              schedule_vdd_confirmation_timeout(operation_id);
            }
            else {
              finish_tray_vdd_action(operation_id, success, create ? "Failed to create virtual display"sv : "Failed to close virtual display"sv);
            }
          }
          catch (const std::exception &e) {
            BOOST_LOG(error) << "VDD "sv << action_name << " action failed: "sv << e.what();
            finish_tray_vdd_action(operation_id, false, create ? "Failed to create virtual display"sv : "Failed to close virtual display"sv);
          }
        });
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to dispatch VDD action: "sv << e.what();
        return false;
      }
    }

    void
    schedule_vdd_confirmation_timeout(const std::uint64_t operation_id) {
      task_pool.pushDelayed([operation_id]() {
        std::unique_lock lock { tray_action_mutex };
        auto expected_operation_id = operation_id;
        if (!tray_vdd_confirmation_operation_id.compare_exchange_strong(expected_operation_id, 0)) {
          return;
        }

        tray_state::set_vdd_confirmation(false);
        tray_vdd_action_running = true;
        lock.unlock();
        update_tray_vdd_state();
        tray_state::set_notification("Virtual display", "Virtual display was not confirmed and was closed", "default");
        if (!dispatch_tray_vdd_action(false, operation_id)) {
          tray_vdd_action_running = false;
          update_tray_vdd_state();
        }
      },
        20s);
    }

    bool
    dispatch_app_termination(const std::uint64_t operation_id) {
      try {
        tray_action_executor().push([operation_id]() {
          BOOST_LOG(info) << "Clearing cache by terminating application from GUI tray"sv;
          bool success = true;
          std::string operation_error;
          try {
            proc::proc.terminate();
          }
          catch (const std::exception &e) {
            BOOST_LOG(error) << "Application termination failed: "sv << e.what();
            tray_state::set_notification("Sunshine", "Failed to terminate the running application", "default");
            success = false;
            operation_error = "Failed to terminate the running application";
          }
          tray_app_termination_running = false;
          tray_state::complete_operation(operation_id, success, success ? "Application terminated" : "", operation_error);
        });
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to dispatch application termination: "sv << e.what();
        return false;
      }
    }

    nlohmann::json
    run_tray_action(
      const std::string &action,
      const std::optional<bool> enabled,
      const std::optional<std::uint64_t> notification_id,
      const std::optional<std::uint64_t> operation_id) {
      std::unique_lock lock { tray_action_mutex };

      const auto start_vdd_action = [&](const bool create, const std::string_view operation_action) {
        const auto dispatch_error = create ? "Failed to start VDD create action"sv : "Failed to start VDD close action"sv;
        const auto requested_message = create ? "VDD create requested"sv : "VDD close requested"sv;
        tray_vdd_action_running = true;
        const auto operation_id = tray_state::begin_operation(std::string { operation_action });
        lock.unlock();
        update_tray_vdd_state();
        if (!dispatch_tray_vdd_action(create, operation_id)) {
          tray_vdd_action_running = false;
          tray_state::complete_operation(operation_id, false, {}, std::string { dispatch_error });
          update_tray_vdd_state();
          return action_error(dispatch_error);
        }
        return action_success(requested_message, operation_id);
      };

      if (action == "vdd_confirm_keep") {
        if (!enabled || !operation_id || *operation_id == 0) {
          return action_error("enabled and operation_id are required");
        }
        auto expected_operation_id = *operation_id;
        if (!tray_vdd_confirmation_operation_id.compare_exchange_strong(expected_operation_id, 0)) {
          return action_error("Virtual display confirmation is stale");
        }

        tray_state::set_vdd_confirmation(false);
        if (*enabled) {
          return action_success("Virtual display retained");
        }
        if (!tray_vdd_active()) {
          update_tray_vdd_state();
          return action_success("Virtual display is already closed");
        }
        tray_vdd_action_running = true;
        lock.unlock();
        update_tray_vdd_state();
        if (!dispatch_tray_vdd_action(false, *operation_id)) {
          tray_vdd_action_running = false;
          update_tray_vdd_state();
          return action_error("Failed to start virtual display rollback");
        }
        return action_success("Virtual display rollback requested");
      }

      if (action == "vdd_create") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load() || tray_app_termination_running.load()) {
          return action_error("Another tray action is already in progress or VDD is cooling down");
        }
        if (tray_vdd_active()) {
          update_tray_vdd_state();
          return action_error("VDD is already active");
        }
        return start_vdd_action(true, action);
      }

      if (action == "vdd_destroy") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load() || tray_app_termination_running.load()) {
          return action_error("Another tray action is already in progress or VDD is cooling down");
        }
        if (!tray_vdd_active()) {
          update_tray_vdd_state();
          return action_error("VDD is not active");
        }
        if (config::video.vdd_keep_enabled) {
          update_tray_vdd_state();
          return action_error("VDD keep-enabled mode is active");
        }
        if (tray_vdd_confirmation_operation_id.exchange(0) != 0) {
          tray_state::set_vdd_confirmation(false);
        }
        return start_vdd_action(false, action);
      }

      if (action == "vdd_toggle_keep_enabled") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load()) {
          return action_error("VDD settings cannot change while an action is in progress or cooling down");
        }
        config::video.vdd_keep_enabled = enabled.value_or(!config::video.vdd_keep_enabled);
        config::update_config({ { "vdd_keep_enabled", config::video.vdd_keep_enabled ? "true" : "false" } });
        update_tray_vdd_state();
        return action_success(config::video.vdd_keep_enabled ? "VDD keep-enabled mode enabled" : "VDD keep-enabled mode disabled");
      }

      if (action == "vdd_toggle_headless_create") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load()) {
          return action_error("VDD settings cannot change while an action is in progress or cooling down");
        }
        config::video.vdd_headless_create_enabled = enabled.value_or(!config::video.vdd_headless_create_enabled);
        config::update_config({ { "vdd_headless_create", config::video.vdd_headless_create_enabled ? "true" : "false" } });
        update_tray_vdd_state();
        return action_success(config::video.vdd_headless_create_enabled ? "Headless VDD auto-create enabled" : "Headless VDD auto-create disabled");
      }

      if (action == "clear_app") {
        if (tray_vdd_action_running.load()) {
          return action_error("Another asynchronous tray action is already in progress");
        }
        if (tray_app_termination_running.exchange(true)) {
          return action_error("Application termination is already in progress");
        }

        const auto operation_id = tray_state::begin_operation(action);
        lock.unlock();
        if (!dispatch_app_termination(operation_id)) {
          tray_app_termination_running = false;
          tray_state::complete_operation(operation_id, false, {}, "Failed to start application termination");
          return action_error("Failed to start application termination");
        }
        return action_success("Application termination requested", operation_id);
      }

      if (action == "reset_display_device_config") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load()) {
          return action_error("Display device config cannot reset while a VDD action is in progress or cooling down");
        }
        BOOST_LOG(info) << "Resetting display device config from GUI tray"sv;
        display_device::session_t::get().reset_persistence();
        update_tray_vdd_state();
        return action_success("Display device config reset");
      }

      if (action == "restart") {
        BOOST_LOG(info) << "Restarting from GUI tray"sv;
        platf::restart();
        return action_success("Restart requested");
      }

      if (action == "notification_ack") {
        if (!notification_id || *notification_id == 0) {
          return action_error("notification_id is required");
        }
        if (!tray_state::acknowledge_notification(*notification_id)) {
          return action_error("Notification is stale or still actionable");
        }
        return action_success("Notification acknowledged");
      }

      return action_error("Unknown tray action");
    }

    void
    get_tray_state(resp_https_t response, req_https_t request, const auth_fn &auth) {
      if (!auth(response, request)) return;

      send_json(std::move(response), tray_state::to_json());
    }

    void
    subscribe_tray_state(resp_https_t response, req_https_t request, const auth_fn &auth) {
      if (!auth(response, request)) return;

      auto subscriber = std::make_shared<tray_subscriber_t>();
      subscriber->response = std::move(response);
      subscriber->response->close_connection_after_response = true;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/event-stream");
      headers.emplace("Cache-Control", "no-cache");
      headers.emplace("Connection", "keep-alive");
      headers.emplace("X-Accel-Buffering", "no");
      subscriber->response->write(headers);
      subscriber->response->send();

      {
        std::lock_guard lock { tray_subscribers_mutex };
        tray_subscribers.push_back(subscriber);
      }
      send_tray_event(subscriber, tray_state_event());
    }

    void
    tray_action(resp_https_t response, req_https_t request, const auth_fn &auth) {
      if (!check_json_content_type(response, request)) return;
      if (!auth(response, request)) return;

      std::stringstream ss;
      ss << request->content.rdbuf();

      try {
        const auto body = nlohmann::json::parse(ss.str());
        const auto action = body.value("action", ""s);
        std::optional<bool> enabled;
        if (body.contains("enabled")) {
          if (!body.at("enabled").is_boolean()) {
            throw std::invalid_argument("enabled must be a boolean");
          }
          enabled = body.at("enabled").get<bool>();
        }
        std::optional<std::uint64_t> notification_id;
        if (body.contains("notification_id")) {
          if (!body.at("notification_id").is_number_unsigned()) {
            throw std::invalid_argument("notification_id must be an unsigned integer");
          }
          notification_id = body.at("notification_id").get<std::uint64_t>();
        }
        std::optional<std::uint64_t> operation_id;
        if (body.contains("operation_id")) {
          if (!body.at("operation_id").is_number_unsigned()) {
            throw std::invalid_argument("operation_id must be an unsigned integer");
          }
          operation_id = body.at("operation_id").get<std::uint64_t>();
        }
        auto result = run_tray_action(action, enabled, notification_id, operation_id);
        result["action"] = action;
        result["tray_state"] = tray_state::to_json();
        send_json(std::move(response), result);
      }
      catch (const std::exception &e) {
        send_json(std::move(response), {
                                         { "status", false },
                                         { "error", e.what() },
                                         { "tray_state", tray_state::to_json() },
                                       });
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server, auth_fn state_auth, auth_fn action_auth) {
    update_tray_vdd_state();
    tray_state::set_change_sink(&publish_tray_state);
    if (!tray_keepalive_started.exchange(true)) {
      schedule_tray_keepalive();
    }
    server.resource["^/api/tray/state$"]["GET"] = [state_auth](resp_https_t response, req_https_t request) {
      get_tray_state(std::move(response), std::move(request), state_auth);
    };
    server.resource["^/api/tray/events$"]["GET"] = [state_auth](resp_https_t response, req_https_t request) {
      subscribe_tray_state(std::move(response), std::move(request), state_auth);
    };
    server.resource["^/api/tray/action$"]["POST"] = [action_auth](resp_https_t response, req_https_t request) {
      tray_action(std::move(response), std::move(request), action_auth);
    };
  }

}  // namespace tray_http
