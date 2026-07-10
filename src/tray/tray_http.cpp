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
#include <system_error>
#include <thread>
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
#include "tray_state.h"

namespace tray_http {
  using namespace std::literals;

  namespace {
    std::atomic<bool> tray_vdd_action_cooldown { false };
    std::atomic<bool> tray_vdd_action_running { false };
    std::atomic<bool> tray_app_termination_running { false };
    std::mutex tray_action_mutex;

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

    void
    send_json(resp_https_t response, const nlohmann::json &body) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(body.dump(), headers);
    }

    void
    send_bad_request(resp_https_t response, const std::string &error) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
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
    finish_tray_vdd_action(bool success, std::string_view error) {
      tray_vdd_action_cooldown = true;
      tray_vdd_action_running = false;
      update_tray_vdd_state();

      if (!success) {
        tray_state::set_notification("Virtual display", std::string { error }, "default");
      }

      task_pool.pushDelayed([]() {
        tray_vdd_action_cooldown = false;
        update_tray_vdd_state();
      },
        10s);
    }

    bool
    dispatch_tray_vdd_action(bool create) {
      try {
        std::thread([create]() {
          const auto action_name = create ? "create"sv : "close"sv;
          BOOST_LOG(info) << (create ? "Creating"sv : "Closing"sv) << " VDD from GUI tray"sv;

          try {
            const bool success = create ?
                                   display_device::session_t::get().toggle_display_power() :
                                   display_device::session_t::get().destroy_vdd_monitor();
            finish_tray_vdd_action(success, create ? "Failed to create virtual display"sv : "Failed to close virtual display"sv);
          }
          catch (const std::exception &e) {
            BOOST_LOG(error) << "VDD "sv << action_name << " action failed: "sv << e.what();
            finish_tray_vdd_action(false, create ? "Failed to create virtual display"sv : "Failed to close virtual display"sv);
          }
        }).detach();
        return true;
      }
      catch (const std::system_error &e) {
        BOOST_LOG(error) << "Failed to dispatch VDD action: "sv << e.what();
        return false;
      }
    }

    bool
    dispatch_app_termination() {
      try {
        std::thread([]() {
          BOOST_LOG(info) << "Clearing cache by terminating application from GUI tray"sv;
          try {
            proc::proc.terminate();
          }
          catch (const std::exception &e) {
            BOOST_LOG(error) << "Application termination failed: "sv << e.what();
            tray_state::set_notification("Sunshine", "Failed to terminate the running application", "default");
          }
          tray_app_termination_running = false;
        }).detach();
        return true;
      }
      catch (const std::system_error &e) {
        BOOST_LOG(error) << "Failed to dispatch application termination: "sv << e.what();
        return false;
      }
    }

    nlohmann::json
    run_tray_action(const std::string &action, const std::optional<bool> enabled, const std::optional<std::uint64_t> notification_id) {
      std::unique_lock lock { tray_action_mutex };

      if (action == "vdd_create") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load()) {
          return { { "status", false }, { "error", "VDD action is already in progress or cooling down" } };
        }
        if (tray_vdd_active()) {
          update_tray_vdd_state();
          return { { "status", false }, { "error", "VDD is already active" } };
        }

        tray_vdd_action_running = true;
        lock.unlock();
        update_tray_vdd_state();
        if (!dispatch_tray_vdd_action(true)) {
          tray_vdd_action_running = false;
          update_tray_vdd_state();
          return { { "status", false }, { "error", "Failed to start VDD create action" } };
        }

        return { { "status", true }, { "message", "VDD create requested" } };
      }

      if (action == "vdd_destroy") {
        if (tray_vdd_action_running.load() || tray_vdd_action_cooldown.load()) {
          return { { "status", false }, { "error", "VDD action is already in progress or cooling down" } };
        }
        if (!tray_vdd_active()) {
          update_tray_vdd_state();
          return { { "status", false }, { "error", "VDD is not active" } };
        }
        if (config::video.vdd_keep_enabled) {
          update_tray_vdd_state();
          return { { "status", false }, { "error", "VDD keep-enabled mode is active" } };
        }

        tray_vdd_action_running = true;
        lock.unlock();
        update_tray_vdd_state();
        if (!dispatch_tray_vdd_action(false)) {
          tray_vdd_action_running = false;
          update_tray_vdd_state();
          return { { "status", false }, { "error", "Failed to start VDD close action" } };
        }

        return { { "status", true }, { "message", "VDD close requested" } };
      }

      if (action == "vdd_toggle_keep_enabled") {
        config::video.vdd_keep_enabled = enabled.value_or(!config::video.vdd_keep_enabled);
        config::update_config({ { "vdd_keep_enabled", config::video.vdd_keep_enabled ? "true" : "false" } });
        update_tray_vdd_state();
        return {
          { "status", true },
          { "message", config::video.vdd_keep_enabled ? "VDD keep-enabled mode enabled" : "VDD keep-enabled mode disabled" },
        };
      }

      if (action == "vdd_toggle_headless_create") {
        config::video.vdd_headless_create_enabled = enabled.value_or(!config::video.vdd_headless_create_enabled);
        config::update_config({ { "vdd_headless_create", config::video.vdd_headless_create_enabled ? "true" : "false" } });
        update_tray_vdd_state();
        return {
          { "status", true },
          { "message", config::video.vdd_headless_create_enabled ? "Headless VDD auto-create enabled" : "Headless VDD auto-create disabled" },
        };
      }

      if (action == "clear_app") {
        if (tray_app_termination_running.exchange(true)) {
          return { { "status", false }, { "error", "Application termination is already in progress" } };
        }

        lock.unlock();
        if (!dispatch_app_termination()) {
          tray_app_termination_running = false;
          return { { "status", false }, { "error", "Failed to start application termination" } };
        }
        return { { "status", true }, { "message", "Application termination requested" } };
      }

      if (action == "reset_display_device_config") {
        BOOST_LOG(info) << "Resetting display device config from GUI tray"sv;
        display_device::session_t::get().reset_persistence();
        update_tray_vdd_state();
        return { { "status", true }, { "message", "Display device config reset" } };
      }

      if (action == "restart") {
        BOOST_LOG(info) << "Restarting from GUI tray"sv;
        platf::restart();
        return { { "status", true }, { "message", "Restart requested" } };
      }

      if (action == "notification_ack") {
        if (!notification_id || *notification_id == 0) {
          return { { "status", false }, { "error", "notification_id is required" } };
        }
        if (!tray_state::acknowledge_notification(*notification_id)) {
          return { { "status", false }, { "error", "Notification is stale or still actionable" } };
        }
        return { { "status", true }, { "message", "Notification acknowledged" } };
      }

      return { { "status", false }, { "error", "Unknown tray action" } };
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
        auto result = run_tray_action(action, enabled, notification_id);
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
