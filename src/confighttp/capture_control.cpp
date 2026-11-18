/**
 * @file src/confighttp/capture_control.cpp
 * @brief Local control API for per-session window-follow capture (Plan4).
 *
 * The endpoint lets a trusted local client (for example the Playnite extension
 * in plan4/playnite-stream-plugin) ask one streaming session to capture a
 * single window instead of the whole display.  The capture target state lives in
 * @ref streaming::capture_target_t, which is owned by the session, so the
 * request never mutates the global Sunshine configuration and other sessions
 * keep their existing behaviour.  The capture thread observes the override
 * generation and reinitializes the display backend on change.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "capture_control.h"

#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/stream.h"

#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::capture_control {

#ifdef _WIN32

  using session_control_t = stream::session::video_session_control_t;

  namespace {

    const char *
    mode_to_string(streaming::window_capture_mode_e mode) {
      switch (mode) {
        case streaming::window_capture_mode_e::title:
          return "title";
        case streaming::window_capture_mode_e::pid:
          return "pid";
        case streaming::window_capture_mode_e::foreground:
        default:
          return "foreground";
      }
    }

    json
    override_to_json(const streaming::window_capture_override_t &override) {
      json node;
      node["enabled"] = override.enabled;
      node["mode"] = mode_to_string(override.mode);
      node["window_title"] = override.window_title;
      node["process_id"] = override.process_id;
      return node;
    }

    json
    observed_to_json(const streaming::window_capture_observed_t &observed) {
      json node;
      node["active"] = observed.active;
      node["fallback_to_display"] = observed.fallback_to_display;
      node["window_title"] = observed.window_title;
      node["process_id"] = observed.process_id;
      if (observed.hwnd != 0) {
        std::ostringstream hwnd;
        hwnd << "0x" << std::hex << observed.hwnd;
        node["hwnd"] = hwnd.str();
      }
      else {
        node["hwnd"] = nullptr;
      }
      node["fallback_reason"] = observed.fallback_reason;
      return node;
    }

    json
    session_to_json(const session_control_t &session) {
      json node;
      node["session_id"] = session.session_id;
      node["client_uuid"] = session.client_cert_uuid;
      node["client_name"] = session.client_name;
      node["app_name"] = session.app_name;
      node["running"] = session.running;
      node["control_only"] = session.control_only;

      if (session.capture_target) {
        node["override"] = override_to_json(session.capture_target->window_override());
        node["observed"] = observed_to_json(session.capture_target->observed_window());
      }
      else {
        node["override"] = nullptr;
        node["observed"] = nullptr;
      }
      return node;
    }

    /**
     * @brief Window-follow capture requires Windows WGC and therefore the
     *        user-mode Sunshine process (not the SYSTEM service).
     */
    bool
    window_follow_supported(std::string &reason) {
      if (is_running_as_system_user) {
        reason = "wgc_requires_user_mode";
        return false;
      }
      return true;
    }

    std::vector<session_control_t>
    active_video_sessions() {
      std::vector<session_control_t> result;
      for (auto &session : stream::session::list_video_sessions_for_control()) {
        if (session.running && !session.control_only) {
          result.push_back(std::move(session));
        }
      }
      return result;
    }

    json
    sessions_to_json() {
      json array = json::array();
      for (const auto &session : stream::session::list_video_sessions_for_control()) {
        if (!session.running) {
          continue;
        }
        array.push_back(session_to_json(session));
      }
      return array;
    }

    json
    base_response() {
      json result;
      result["ok"] = true;
      result["feature"] = "window-follow";

      std::string reason;
      const bool supported = window_follow_supported(reason);
      result["supported"] = supported;
      if (!supported) {
        result["reason"] = reason;
      }
      return result;
    }

    /**
     * @brief Resolve which session the request targets.
     *
     * Selection order: explicit `session_id`, then explicit `client_uuid`, then
     * the single active video session.  Multiple active sessions without an
     * explicit selector are rejected instead of being changed silently.
     *
     * Enabling is stricter: while more than one video session is active it is
     * rejected even when the caller explicitly selects a session.  The capture
     * thread and its display backend may be shared by multiple sessions, so an
     * enable/reinit transition could disturb the other sessions on that thread.
     * Disable and status remain targetable so an existing override can still be
     * inspected or rolled back during a multi-session transition.
     */
    void
    get_window_follow(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "reading window-follow capture state")) {
        return;
      }

      auto result = base_response();
      result["sessions"] = sessions_to_json();
      write_json(response, web::StatusCode::ok, result);
    }

    void
    set_window_follow(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      if (!require_localhost(response, request, "changing window-follow capture state")) {
        return;
      }

      json body;
      if (!read_json_body(response, request, body)) return;

      if (!body.is_object()) {
        write_json_error(response, web::StatusCode::bad_request, "request_body_must_be_a_json_object");
        return;
      }

      std::string action;
      std::string requested_mode;
      std::string requested_title;
      std::uint32_t requested_pid = 0;
      std::uint32_t requested_session_id = 0;
      std::string requested_client_uuid;
      try {
        action = body.value("action", std::string { "enable" });
        requested_mode = body.value("mode", std::string { "foreground" });
        requested_title = body.value("window_title", std::string {});
        requested_pid = body.value("process_id", 0u);
        requested_session_id = body.value("session_id", 0u);
        requested_client_uuid = body.value("client_uuid", std::string {});
      }
      catch (const std::exception &e) {
        write_json_error(response, web::StatusCode::bad_request, std::string { "invalid_request_field: " } + e.what());
        return;
      }

      if (action != "enable" && action != "disable" && action != "status") {
        write_json_error(response, web::StatusCode::bad_request, "invalid_action");
        return;
      }

      std::string unsupported_reason;
      const bool supported = window_follow_supported(unsupported_reason);

      auto sessions = active_video_sessions();

      // Enabling while multiple video sessions are active is rejected for all
      // callers, including callers that name a session explicitly.  The
      // capture thread/display backend can be shared, so an enable transition
      // would not be isolated to the selected session.
      if (action == "enable" && sessions.size() > 1) {
        write_json_error(response, web::StatusCode::conflict, "multiple_video_sessions_enable_not_supported");
        return;
      }

      const session_control_t *selected = nullptr;

      if (requested_session_id != 0) {
        for (const auto &session : sessions) {
          if (session.session_id == requested_session_id) {
            selected = &session;
            break;
          }
        }
        if (!selected) {
          write_json_error(response, web::StatusCode::not_found, "session_not_found");
          return;
        }
      }
      else if (!requested_client_uuid.empty()) {
        for (const auto &session : sessions) {
          if (session.client_cert_uuid != requested_client_uuid) {
            continue;
          }
          if (selected) {
            write_json_error(response, web::StatusCode::conflict, "multiple_client_sessions_specify_session_id");
            return;
          }
          selected = &session;
        }
        if (!selected) {
          write_json_error(response, web::StatusCode::not_found, "client_session_not_found");
          return;
        }
      }
      else if (sessions.size() == 1) {
        selected = &sessions.front();
      }
      else if (sessions.empty()) {
        write_json_error(response, web::StatusCode::not_found, "no_active_video_session");
        return;
      }
      else {
        write_json_error(response, web::StatusCode::conflict, "multiple_video_sessions_specify_session_id");
        return;
      }

      if (!selected->capture_target) {
        write_json_error(response, web::StatusCode::internal_server_error, "session_capture_target_unavailable");
        return;
      }

      const auto selected_session_id = selected->session_id;
      const auto selected_client_uuid = selected->client_cert_uuid;

      if (action == "disable") {
        selected->capture_target->clear_window_override();
        BOOST_LOG(info) << "Window-follow: disabled from local control request (session "sv
                        << selected_session_id << ", client="sv << selected_client_uuid << ')';
      }
      else if (action == "enable") {
        if (!supported) {
          write_json_error(response, web::StatusCode::conflict, unsupported_reason);
          return;
        }

        streaming::window_capture_override_t override_request;
        override_request.enabled = true;
        override_request.process_id = requested_pid;
        override_request.window_title = requested_title;

        if (requested_mode == "foreground") {
          override_request.mode = streaming::window_capture_mode_e::foreground;
          // Foreground mode ignores the title/pid filters.
          override_request.window_title.clear();
          override_request.process_id = 0;
        }
        else if (requested_mode == "title") {
          if (requested_title.empty()) {
            write_json_error(response, web::StatusCode::bad_request, "window_title_required_for_title_mode");
            return;
          }
          override_request.mode = streaming::window_capture_mode_e::title;
          override_request.process_id = 0;
        }
        else if (requested_mode == "pid") {
          if (requested_pid == 0) {
            write_json_error(response, web::StatusCode::bad_request, "process_id_required_for_pid_mode");
            return;
          }
          override_request.mode = streaming::window_capture_mode_e::pid;
          override_request.window_title.clear();
        }
        else {
          write_json_error(response, web::StatusCode::bad_request, "invalid_mode");
          return;
        }

        selected->capture_target->set_window_override(override_request);
        BOOST_LOG(info) << "Window-follow: enabled from local control request (session "sv
                        << selected_session_id << ", client="sv << selected_client_uuid
                        << ", mode="sv << mode_to_string(override_request.mode) << ')';
      }

      auto result = base_response();
      result["selected_session_id"] = selected_session_id;
      result["selected_client_uuid"] = selected_client_uuid;
      result["sessions"] = sessions_to_json();
      write_json(response, web::StatusCode::ok, result);
    }

  }  // namespace

#endif  // _WIN32

  void
  register_routes(https_server_t &server) {
#ifdef _WIN32
    server.resource["^/api/capture/window-follow$"]["GET"] = get_window_follow;
    server.resource["^/api/capture/window-follow$"]["POST"] = set_window_follow;
#else
    (void) server;
#endif
  }

}  // namespace confighttp::capture_control
