/**
 * @file src/confighttp/dualsense.cpp
 * @brief DualSense tuning profile API and the short test rumble of the gamepad test tool.
 *
 * The route follows the Web UI authentication and browser-origin rules in
 * confighttp::authenticate(): every request passes authorize_browser_request()
 * first, so a cross-site page cannot mutate the profile.  The body parser is
 * the same nlohmann/json helper used by the other configuration mutations.
 *
 * Sources: ds5-rfc8259-json-20261031 (JSON object and value grammar) and
 * ds5-rfc9110-http-20261031 sections 8.3, 15.3.1, 15.5.1 and 15.5.10
 * (Content-Type and the 200/400/409 outcomes).
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "dualsense.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/ds5/rumble_test.h"
#include "src/ds5/tuning_profile.h"
#include "src/ds5/tuning_runtime.h"
#include "src/logging.h"
#include "src/platform/common.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::dualsense {
  namespace {
    std::filesystem::path
    tuning_path() {
      return ds5::tuning::profile_path_for(config::sunshine.config_file, platf::appdata());
    }

    ds5::tuning::store_t &
    store() {
      static ds5::tuning::store_t instance(tuning_path());
      static std::once_flag loaded;
      std::call_once(loaded, [] {
        std::string error;
        if (!instance.load(&error)) {
          BOOST_LOG(warning) << "DualSense tuning profile was not loaded: "sv << error;
        }
        // A missing or invalid file still has to publish the descriptor
        // defaults, otherwise the first request leaves stale process state.
        instance.publish();
      });
      return instance;
    }

    json
    decorated_snapshot() {
      auto snapshot = store().snapshot_json();
#ifdef SUNSHINE_PLATFORM
      snapshot["platform"] = SUNSHINE_PLATFORM;
#else
      snapshot["platform"] = "unknown";
#endif
      snapshot["backend"] = {
        { "native_path_compiled", ds5::tuning::runtime::native_path_compiled() },
        { "native_path_active", ds5::tuning::runtime::native_path_active() },
        { "active_controllers", ds5::tuning::runtime::state().active_controllers.load(std::memory_order_acquire) },
        { "persistence", "config-directory" },
      };
      return snapshot;
    }

    void
    getTuning(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);
      write_json(response, web::StatusCode::ok, decorated_snapshot());
    }

    void
    postTuning(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);
      if (request->content.size() > 64 * 1024) {
        write_json_error(response, web::StatusCode::payload_too_large, "tuning request body is too large");
        return;
      }

      json body;
      if (!read_json_body(response, request, body)) return;
      auto result = store().apply_json(body);
      json output;
      if (result.body.is_object()) {
        output = std::move(result.body);
      }
      else {
        output = {
          { "schema", "ALK-DS5T/1" },
          { "status", false },
          { "code", result.code },
          { "error", result.error },
        };
      }
#ifdef SUNSHINE_PLATFORM
      output["platform"] = SUNSHINE_PLATFORM;
#else
      output["platform"] = "unknown";
#endif
      output["backend"] = {
        { "native_path_compiled", ds5::tuning::runtime::native_path_compiled() },
        { "native_path_active", ds5::tuning::runtime::native_path_active() },
        { "active_controllers", ds5::tuning::runtime::state().active_controllers.load(std::memory_order_acquire) },
        { "persistence", "config-directory" },
      };
      if (!result.ok) {
        BOOST_LOG(warning) << "DualSense tuning update rejected: "sv << result.error;
      }
      write_json(response, static_cast<web::StatusCode>(result.http_status), output);
    }

    /* POST /api/dualsense/rumble-test: one short pulse (body {"duration_ms": 100..1000}, optional) through the path of a
     * game's rumble, on every live virtual pad. The answer says what the host did and, honestly, that it cannot know
     * whether the client's pad moved: result "sent" (reached_client "unknown"), "no_pad", "unsupported" or "busy". */
    void
    postRumbleTest(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);
      long long asked = 0;
      bool given = false;
      if (request->content.size() > 0) {
        if (!check_content_type(response, request, "application/json")) return;
        if (request->content.size() > 1024) {
          write_json_error(response, web::StatusCode::payload_too_large, "rumble test request body is too large");
          return;
        }
        json body;
        if (!read_json_body(response, request, body)) return;
        if (body.is_object() && body.contains("duration_ms") && body["duration_ms"].is_number()) {
          asked = body["duration_ms"].get<long long>();
          given = true;
        }
      }
      const auto ms = ds5::rumble_test::clamp_duration(asked, given);
      const auto pulse = ds5::rumble_test::run(ms, [](std::chrono::milliseconds wait) { std::this_thread::sleep_for(wait); });
      BOOST_LOG(info) << "DualSense rumble test: "sv << ds5::rumble_test::to_string(pulse.outcome) << ", "sv << pulse.pads << " pad(s), "sv << ms << " ms"sv;
      json output = {
        { "status", pulse.outcome == ds5::rumble_test::outcome_e::sent },
        { "result", ds5::rumble_test::to_string(pulse.outcome) },
        { "pads", pulse.pads },
        { "duration_ms", ms },
        { "reached_client", "unknown" },
      };
      write_json(response, web::StatusCode::ok, output);
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    // Load and publish the saved profile now: the native feedback path reads the
    // runtime values, and without this they stay at the defaults after a restart
    // until the first request reaches the page.
    (void) store();
    server.resource["^/api/dualsense/tuning$"]["GET"] = getTuning;
    server.resource["^/api/dualsense/tuning$"]["POST"] = postTuning;
    server.resource["^/api/dualsense/rumble-test$"]["POST"] = postRumbleTest;
  }
}  // namespace confighttp::dualsense
