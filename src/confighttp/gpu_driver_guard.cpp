/**
 * @file src/confighttp/gpu_driver_guard.cpp
 * @brief The state the Web UI, the Panel and the tray read about the graphics driver, and the two things a person can do about it.
 *
 *   GET  /api/gpu-driver-guard  { v, probe, safe_mode: { active, since, auto_exit_s, skipped }, driver: null | { show, vendor, module, driver, phase, kind, time, key } }
 *   POST /api/gpu-driver-guard  { action: "dismiss" }  the notice of the driver that is on record is not shown again (until another driver version fails)
 *                               { action: "retry" }    leave the safe mode and check the encoders now, in the background; answers with the state
 *
 * The contract is described in docs/gpu-driver-guard.md; the state itself lives in src/gpu_driver_guard.h.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "gpu_driver_guard.h"

#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/crash/crash.h"
#include "src/gpu_driver_guard.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

#include <atomic>
#include <thread>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::gpu_driver_guard {
  namespace {
    /// The notice store of the running host, or one over the same folder (the tools and the tests start no host).
    ::gpu_guard::notice_store_t &
    notices() {
      if (auto *store = ::gpu_guard::g_notices.load()) return *store;
      static ::gpu_guard::notice_store_t fallback { platf::appdata() };
      return fallback;
    }

    json
    current_status() {
      const auto mark = ::gpu_guard::read_mark(platf::appdata());
      const std::string installed = mark ? std::string { crash::gpu_driver::driver_version_of(mark->vendor) } : std::string {};
      return ::gpu_guard::status_json(::gpu_guard::g_guard.load(), ::gpu_guard::get_probe(), mark, installed, notices());
    }

    /// The encoder check the person asked for, on a thread of its own (a driver call that does not return must not hold the request or the exit).
    void
    start_retry_probe() {
      static std::atomic<bool> running { false };
      // the check of the start is still going on: it ends with its own state, and a second one would only wait for the same lock; leave the state as it is
      if (const auto now = ::gpu_guard::get_probe(); now == ::gpu_guard::probe_e::pending || now == ::gpu_guard::probe_e::running) return;
      if (running.exchange(true)) return;
      ::gpu_guard::set_probe(::gpu_guard::probe_e::running);
      crash::gpu_driver::set_phase(crash::gpu_driver::phase_e::probe);
      try {
        std::thread([] {
          crash::watched_thread_t watch("encoder-probe", 120000);
          crash::busy_scope_t busy(watch.slot);
          bool failed = true;
          try {
            BOOST_LOG(info) << "Encoder check asked for from the interface"sv;
            failed = video::probe_encoders() != 0;
          }
          catch (...) {
            BOOST_LOG(error) << "The encoder check ended with an exception"sv;
          }
          ::gpu_guard::set_probe(failed ? ::gpu_guard::probe_e::failed : ::gpu_guard::probe_e::done);
          crash::gpu_driver::set_phase(crash::gpu_driver::phase_e::idle);
          running.store(false);
        }).detach();
      }
      catch (...) {
        ::gpu_guard::set_probe(::gpu_guard::probe_e::failed);
        running.store(false);
      }
    }

    void
    getStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      write_json(response, web::StatusCode::ok, current_status());
    }

    void
    postAction(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      const auto action = body.is_object() && body.contains("action") && body["action"].is_string() ? body["action"].get<std::string>() : std::string {};
      if (action == "dismiss") {
        // the key names the driver the page showed; a mark that was replaced meanwhile (another driver failed) is not dismissed by it, and a request without a key dismisses nothing
        const auto mark = ::gpu_guard::read_mark(platf::appdata());
        const auto key = body.contains("key") && body["key"].is_string() ? body["key"].get<std::string>() : std::string {};
        if (mark && !key.empty() && key == mark->key()) notices().dismiss(mark->key());
      }
      else if (action == "retry") {
        if (auto *guard = ::gpu_guard::g_guard.load()) guard->request_retry();
        start_retry_probe();
      }
      else {
        write_json_error(response, web::StatusCode::bad_request, "expected {action: dismiss|retry}");
        return;
      }
      write_json(response, web::StatusCode::ok, current_status());
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/gpu-driver-guard$"]["GET"] = getStatus;
    server.resource["^/api/gpu-driver-guard$"]["POST"] = postAction;
  }
}  // namespace confighttp::gpu_driver_guard
