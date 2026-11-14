/**
 * @file src/confighttp/diagnostics.cpp
 * @brief Web UI diagnostics handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "diagnostics.h"
#include "src/audio_devices.h"
#include "src/audio_vb_cable.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/confighttp/usbip_root_helper.h"
#include "src/display_device/session.h"
#include "src/entry_handler.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/spatial_audio/mat_live_status.h"
#ifdef _WIN32
  #include "src/spatial_audio/mat_capture_process.h"
#endif
#include <algorithm>
#include <atomic>
#include <boost/algorithm/string.hpp>
#ifdef _WIN32
  #include <boost/process/v1.hpp>
  #include <boost/process/v1/pipe.hpp>
#endif
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
  #include "src/display_device/vdd_utils.h"
  #include "src/platform/windows/mic_write.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/vulkan_hdr_bridge_session.h"
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::diagnostics {
  namespace pt = boost::property_tree;

  using namespace confighttp;

  namespace {
    /**
     * @brief Snapshot of log cache state, swapped atomically via shared_ptr.
     */
    struct LogCacheSnapshot {
      std::shared_ptr<const std::string> content;  ///< Cached log content (nullptr in offset-only mode)
      std::uintmax_t file_size = 0;  ///< Actual file size on disk when snapshot was taken
      std::intmax_t mtime_ns = 0;  ///< File mtime when snapshot was taken
      std::uintmax_t start_offset = 0;  ///< File byte position where content begins
    };

    /**
     * @brief Try to read only the new tail of the log file and append to existing content.
     * @return New content on success, nullptr on any failure (caller should fall back to full read).
     */
    static std::shared_ptr<const std::string>
    try_incremental_log_read(
      const std::filesystem::path &log_path,
      std::uintmax_t prev_size,
      std::uintmax_t current_size,
      const std::shared_ptr<const std::string> &old_content) {
      if (current_size <= prev_size || prev_size == 0 || !old_content) {
        return nullptr;
      }
      std::ifstream in(log_path, std::ios::binary);
      if (!in || !in.seekg(static_cast<std::streamoff>(prev_size))) {
        return nullptr;
      }
      const auto tail_len = current_size - prev_size;
      std::string tail(tail_len, '\0');
      if (!in.read(tail.data(), static_cast<std::streamsize>(tail_len))) {
        return nullptr;
      }
      return std::make_shared<const std::string>(*old_content + tail);
    }

    /**
     * @brief Read a specific byte range [offset, offset+length) from a file.
     * @return Content string on success, nullptr on failure.
     */
    static std::shared_ptr<const std::string>
    read_file_range(const std::filesystem::path &path, std::uintmax_t offset, std::uintmax_t length) {
      std::ifstream in(path, std::ios::binary);
      if (!in || !in.seekg(static_cast<std::streamoff>(offset))) {
        return nullptr;
      }
      std::string content(static_cast<std::size_t>(length), '\0');
      if (!in.read(content.data(), static_cast<std::streamsize>(length))) {
        return nullptr;
      }
      return std::make_shared<const std::string>(std::move(content));
    }

    /**
     * @brief Get the logs from the log file.
     * @param response The HTTP response object.
     * @param request The HTTP request object.
     *
     * Dual mode via X-Log-Offset header:
     *   - Without header: stream full log file from disk (for download)
     *   - With header:    cached or offset-based incremental support (for live viewer)
     *
     * When the log file is small (≤ 4 MB), the entire file is cached in memory for fast serving.
     * When the log file exceeds 4 MB, no content is cached; reads go directly to disk at the
     * client's offset (offset-only mode), avoiding unbounded memory growth.
     *
     * @api_examples{/api/logs| GET| null}
     */
    void
    getLogs(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) {
        return;
      }

      // print_req(request);

      const auto log_path = file_handler::path_from_utf8(config::sunshine.log_file);

      // --- Mode 1: No X-Log-Offset header → stream full file from disk (download) ---
      auto offset_it = request->header.find("X-Log-Offset");
      if (offset_it == request->header.end()) {
        web::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "text/plain; charset=utf-8");
        headers.emplace("Content-Disposition", "attachment; filename=\"sunshine.log\"");
        if (!response->write_file(web::StatusCode::ok, log_path, headers))
          response->write(web::StatusCode::internal_server_error, "Failed to open log file");
        return;
      }

      // --- Mode 2: X-Log-Offset present → cached or offset-only mode for live viewer ---

      // When file ≤ MAX_LOG_CACHE_SIZE: cached in memory.  Otherwise: disk reads only.
      static constexpr std::uintmax_t MAX_LOG_CACHE_SIZE = 4 * 1024 * 1024;  // 4 MB
      static constexpr std::uintmax_t MAX_RESPONSE_SIZE = 4 * 1024 * 1024;  // 4 MB

      static std::atomic<std::shared_ptr<const LogCacheSnapshot>> log_cache;

      // Check file status
      std::error_code ec;
      auto current_size = std::filesystem::file_size(log_path, ec);
      if (ec) {
        response->write(web::StatusCode::internal_server_error, "Failed to read log file");
        return;
      }
      auto current_mtime = std::filesystem::last_write_time(log_path, ec);
      if (ec) {
        response->write(web::StatusCode::internal_server_error, "Failed to read log file");
        return;
      }
      auto current_mtime_ns = current_mtime.time_since_epoch().count();

      // Refresh cache if file changed
      auto snapshot = log_cache.load();
      const bool cache_stale = !snapshot || current_size != snapshot->file_size || current_mtime_ns != snapshot->mtime_ns;
      if (cache_stale) {
        auto new_snap = std::make_shared<LogCacheSnapshot>();
        new_snap->file_size = current_size;
        new_snap->mtime_ns = current_mtime_ns;

        if (current_size <= MAX_LOG_CACHE_SIZE) {
          // --- Cached mode: file fits in memory ---
          std::shared_ptr<const std::string> new_content;
          if (snapshot && snapshot->content && snapshot->file_size > 0 && current_size > snapshot->file_size) {
            new_content = try_incremental_log_read(log_path, snapshot->file_size, current_size, snapshot->content);
          }
          if (!new_content) {
            // Use sampled current_size to avoid TOCTOU unsigned underflow at start_offset
            auto read_len = std::min(current_size, MAX_LOG_CACHE_SIZE);
            auto read_start = current_size - read_len;
            new_content = read_file_range(log_path, read_start, read_len);
          }
          if (!new_content) {
            if (current_size > 0) {
              response->write(web::StatusCode::internal_server_error, "Failed to read log file");
              return;
            }
            new_content = std::make_shared<const std::string>();
          }
          new_snap->content = std::move(new_content);
          new_snap->start_offset = current_size - new_snap->content->size();
        }
        else {
          // --- Offset-only mode: file too large, don't cache content ---
          new_snap->content = nullptr;
          new_snap->start_offset = 0;
        }

        // CAS publish: avoid overwriting a newer snapshot from a concurrent thread
        if (!log_cache.compare_exchange_strong(snapshot, new_snap)) {
          // CAS failed: snapshot already updated by compare_exchange_strong
        }
        else {
          snapshot = std::move(new_snap);
        }
      }

      // Parse client offset
      std::uintmax_t client_offset = 0;
      try {
        std::string offset_str(offset_it->second);
        boost::algorithm::trim(offset_str);
        if (!offset_str.empty()) {
          client_offset = std::stoull(offset_str);
        }
      }
      catch (const std::invalid_argument &) {
        client_offset = 0;
      }
      catch (const std::out_of_range &) {
        client_offset = 0;
      }

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain; charset=utf-8");
      headers.emplace("X-Log-Size", std::to_string(snapshot->file_size));
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

      // No change in logs → 304
      if (client_offset > 0 && client_offset == snapshot->file_size) {
        headers.emplace("X-Log-Range", "unchanged");
        response->write(web::StatusCode::not_modified, headers);
        return;
      }

      if (snapshot->content) {
        // === Cached mode: serve from memory ===
        if (client_offset > 0 && client_offset >= snapshot->start_offset && client_offset < snapshot->file_size) {
          auto cache_pos = client_offset - snapshot->start_offset;
          headers.emplace("X-Log-Range", "incremental");
          auto delta = snapshot->content->substr(static_cast<std::size_t>(cache_pos));
          response->write(web::StatusCode::ok, delta, headers);
        }
        else {
          headers.emplace("X-Log-Range", "full");
          response->write(web::StatusCode::ok, *snapshot->content, headers);
        }
      }
      else {
        // === Offset-only mode: read from disk, bounded by snapshot->file_size ===
        if (client_offset > 0 && client_offset < snapshot->file_size) {
          auto delta_size = snapshot->file_size - client_offset;
          if (delta_size <= MAX_RESPONSE_SIZE) {
            auto data = read_file_range(log_path, client_offset, delta_size);
            if (data) {
              headers.emplace("X-Log-Range", "incremental");
              response->write(web::StatusCode::ok, *data, headers);
              return;
            }
          }
        }
        // Delta too large, read failed, or first request: return tail up to snapshot->file_size
        auto tail_len = std::min(snapshot->file_size, MAX_RESPONSE_SIZE);
        auto tail_start = snapshot->file_size - tail_len;
        auto tail = read_file_range(log_path, tail_start, tail_len);
        if (!tail) {
          response->write(web::StatusCode::internal_server_error, "Failed to read log file");
          return;
        }
        headers.emplace("X-Log-Range", "full");
        response->write(web::StatusCode::ok, *tail, headers);
      }
    }

    void
    restart(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      // We may not return from this call
      platf::restart();
    }

    void
    boom(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);
#ifdef _WIN32
      if (GetConsoleWindow() == NULL) {
        lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
        return;
      }
#endif
      lifetime::exit_sunshine(0, true);
    }

    void
    resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree outputTree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      display_device::session_t::get().reset_persistence();
      outputTree.put("status", true);
    }

    void
    testMicrophone(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

#ifdef _WIN32
      const auto result = platf::audio::test_mic_redirect();
      send_response(response, nlohmann::json {
                                { "success", result.success },
                                { "error_code", result.error_code },
                                { "backend", result.backend },
                                { "mode", result.mode.empty() ? "tone" : result.mode },
                                { "input_peak", result.input_peak },
                                { "input_active_ms", result.input_active_ms },
                                { "heard_seconds", result.heard_seconds },
                                { "windows_total", result.windows_total },
                                { "windows_good", result.windows_good },
                                { "heard_amplitude", result.heard_amplitude },
                              });
#else
      send_response(response, nlohmann::json {
                                { "success", false },
                                { "error_code", "MIC_TEST_UNSUPPORTED" },
                              });
#endif
    }

    /**
     * GET /api/audio/devices: the render and capture endpoints of the host, the known virtual devices flagged,
     * and whether VB-CABLE is installed. Windows only (MMDevice); other systems answer `supported: false`.
     */
    void
    getAudioDevices(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

#ifdef _WIN32
      std::vector<audio_devices::endpoint_t> endpoints;
      if (!audio_devices::enumerate_endpoints(endpoints)) {
        send_response(response, nlohmann::json {
                                  { "success", false },
                                  { "version", audio_devices::JSON_VERSION },
                                  { "supported", true },
                                  { "error_code", "AUDIO_ENUMERATOR_UNAVAILABLE" },
                                });
        return;
      }
      // the device tree tells a disabled driver from a missing one; when it cannot be read the endpoints decide alone
      std::vector<audio_devices::driver_t> drivers;
      const bool drivers_read = audio_devices::enumerate_vb_cable_drivers(drivers);
      auto json = audio_devices::to_json(endpoints, drivers_read ? &drivers : nullptr);
      json["vb_cable"]["job"] = audio_devices::vb_cable::status_json(audio_devices::vb_cable::runner().status());
      send_response(response, json);
#else
      send_response(response, audio_devices::unsupported_json());
#endif
    }

    /**
     * POST /api/audio/vb-cable/install and /enable, body {"confirm": true}: download and install VB-CABLE, or enable its
     * disabled device. The job runs on a thread of its own; its progress is `vb_cable.job` of GET /api/audio/devices.
     * Nothing starts without the confirmation of the person (the UI asks first); the answer says whether a job started.
     */
    void
    startVbCableJob(resp_https_t response, req_https_t request, bool install) {
      if (!authenticate(response, request)) return;
      nlohmann::json body;
      if (!read_json_body(response, request, body)) return;
#ifdef _WIN32
      if (!body.is_object() || body.value("confirm", false) != true) {
        send_response(response, nlohmann::json { { "success", false }, { "error_code", "VB_CONFIRM_REQUIRED" } });
        return;
      }
      using namespace audio_devices::vb_cable;
      const bool started = runner().start(install ? "install" : "enable", [install](const progress_t &progress) {
        auto environment = make_system_environment();
        const auto result = install ? audio_devices::vb_cable::install(*environment, progress) : audio_devices::vb_cable::enable(*environment, progress);
        BOOST_LOG(info) << "VB-CABLE "sv << (install ? "install"sv : "enable"sv) << (result.ok ? " finished"sv : " failed: "sv) << result.error_code << ' ' << result.detail;
        return result;
      });
      send_response(response, nlohmann::json { { "success", started }, { "started", started }, { "error_code", started ? "" : std::string(error::busy) } });
#else
      send_response(response, nlohmann::json { { "success", false }, { "error_code", "VB_UNSUPPORTED" } });
#endif
    }

    void
    installVbCable(resp_https_t response, req_https_t request) {
      startVbCableJob(response, request, true);
    }

    void
    enableVbCable(resp_https_t response, req_https_t request) {
      startVbCableJob(response, request, false);
    }

    void
    getMicrophoneStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

#ifdef _WIN32
      const auto status = platf::audio::mic_redirect_status();
      send_response(response, nlohmann::json {
                                { "success", true },
                                { "configured_backend", status.configured_backend },
                                { "active_backend", status.active_backend },
                                { "fallback_reason", status.fallback_reason },
                                { "component_available", status.component_available },
                                { "online", status.online },
                                { "device_created", status.device_created },
                                { "host_streaming", status.host_streaming },
                                { "generation", status.generation },
                                { "state", status.state },
                                { "buffered_bytes", status.buffered_bytes },
                                { "underruns", status.underruns },
                                { "dropped_frames", status.dropped_frames },
                                { "submit_errors", status.submit_errors },
                                { "last_error", status.last_error },
                                { "error_code", status.error_code },
                                { "input_peak", status.input_peak },
                                { "input_active_ms", status.input_active_ms },
                                { "input_frames", status.input_frames },
                              });
#else
      send_response(response, nlohmann::json {
                                { "success", true },
                                { "configured_backend", "disabled" },
                                { "active_backend", "" },
                                { "state", "unsupported" },
                                { "component_available", false },
                              });
#endif
    }

#ifdef _WIN32
    /**
     * Fixed arguments only. The time limit, the single repair, and the status
     * cache live in usbip_root_helper.h. A timeout stops this child with
     * TerminateProcess and does not touch any other process.
     */
    json
    failure_response(const usbip_root::failure_note &note) {
      json body {
        { "success", false },
        { "error", note.error },
        { "ds4_fallback", note.ds4_fallback },
      };
      if (note.busy) body["busy"] = true;
      if (note.timed_out) body["timed_out"] = true;
      return body;
    }

    struct helper_output {
      boost::process::v1::ipstream pipe;
    };

    json
    launch_usbip_root_helper(usbip_root::action action) {
      namespace bp = boost::process::v1;
      const auto script = platf::appdata().parent_path() / "tools" / "usbip-win2-root.ps1";
      if (!std::filesystem::is_regular_file(script)) {
        return json {
          { "success", false },
          { "error", "usbip-win2 root helper is not installed" },
          { "ds4_fallback", "DS4 fallback remains active because the usbip-win2 helper is unavailable." },
        };
      }

      const auto powershell = bp::search_path("powershell.exe");
      if (powershell.empty()) {
        return json {
          { "success", false },
          { "error", "Windows PowerShell is unavailable" },
          { "ds4_fallback", "DS4 fallback remains active because Windows PowerShell is unavailable." },
        };
      }

      auto captured = std::make_shared<helper_output>();
      std::error_code launch_error;
      bp::child process(
        powershell,
        usbip_root::fixed_powershell_arguments[0],
        usbip_root::fixed_powershell_arguments[1],
        usbip_root::fixed_powershell_arguments[2],
        usbip_root::fixed_powershell_arguments[3],
        script.string(),
        "-Action",
        usbip_root::action_argument(action),
        bp::std_out > captured->pipe,
        bp::std_err > bp::null,
        launch_error);
      if (launch_error || !process.valid()) {
        return json {
          { "success", false },
          { "error", launch_error ? launch_error.message() : "helper process is invalid" },
          { "ds4_fallback", "DS4 fallback remains active because the usbip-win2 helper could not start." },
        };
      }

      int exit_code = 1;
      bool have_exit = false;
      const auto ran = usbip_root::read_bounded(
        captured->pipe,
        captured,
        usbip_root::budget_for(action),
        [&](unsigned long milliseconds) {
          const auto handle = process.native_handle();
          if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return usbip_root::wait_outcome::failed;
          const auto code = ::WaitForSingleObject(handle, static_cast<::DWORD>(milliseconds));
          if (code == WAIT_OBJECT_0) {
            std::error_code wait_error;
            process.wait(wait_error);
            if (!wait_error) {
              exit_code = process.exit_code();
              have_exit = true;
            }
            return usbip_root::wait_outcome::exited;
          }
          if (code == WAIT_TIMEOUT) return usbip_root::wait_outcome::timed_out;
          return usbip_root::wait_outcome::failed;
        },
        [&] {
          const auto handle = process.native_handle();
          if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
          ::TerminateProcess(handle, 1);
          const auto reaped = ::WaitForSingleObject(handle, static_cast<::DWORD>(usbip_root::wait_milliseconds(usbip_root::reap_budget)));
          if (reaped == WAIT_OBJECT_0) {
            std::error_code wait_error;
            process.wait(wait_error);
          }
          else {
            process.detach();
          }
        },
        usbip_root::reader_grace);

      if (usbip_root::should_terminate(ran.outcome)) return failure_response(usbip_root::timeout_failure());
      if (ran.last_line.empty()) {
        return json {
          { "success", have_exit && exit_code == 0 },
          { "exit_code", exit_code },
          { "error", "usbip-win2 root helper returned no status" },
          { "ds4_fallback", "DS4 fallback remains active because the usbip-win2 helper returned no status." },
        };
      }
      try {
        auto result = json::parse(ran.last_line);
        result["success"] = have_exit && exit_code == 0 && result.value("root_present", false);
        result["exit_code"] = exit_code;
        return result;
      }
      catch (const json::exception &exception) {
        return json {
          { "success", false },
          { "exit_code", exit_code },
          { "error", std::string("invalid helper status: ") + exception.what() },
          { "ds4_fallback", "DS4 fallback remains active because the usbip-win2 helper status was invalid." },
        };
      }
    }

    json
    run_usbip_root_helper(std::string_view action_text) {
      const auto action = usbip_root::parse_action(action_text);
      if (action == usbip_root::action::unknown) {
        return json {
          { "success", false },
          { "error", "usbip-win2 root helper action is not allowed" },
          { "ds4_fallback", "DS4 fallback remains active because the usbip-win2 helper action is not allowed." },
        };
      }

      auto &gate = usbip_root::shared_gate();
      const bool repair = action == usbip_root::action::repair;
      if (repair) {
        if (gate.begin_repair(usbip_root::status_budget) != usbip_root::gate<>::repair_take::granted) {
          return failure_response(usbip_root::busy_failure());
        }
      }
      else {
        const auto ticket = gate.acquire_status(usbip_root::status_budget + usbip_root::reader_grace);
        if (ticket.take == usbip_root::gate<>::status_take::reuse) {
          try {
            return json::parse(ticket.body);
          }
          catch (const json::exception &) {
            gate.drop_status_cache();
          }
        }
        else if (ticket.take == usbip_root::gate<>::status_take::gave_up) {
          return failure_response(usbip_root::timeout_failure());
        }
        if (ticket.take != usbip_root::gate<>::status_take::start) {
          const auto again = gate.acquire_status(usbip_root::status_budget);
          if (again.take == usbip_root::gate<>::status_take::reuse) {
            try {
              return json::parse(again.body);
            }
            catch (const json::exception &) {
              gate.drop_status_cache();
            }
          }
          if (again.take != usbip_root::gate<>::status_take::start) {
            return failure_response(usbip_root::timeout_failure());
          }
        }
      }

      struct running_t {
        bool repair;
        bool open { true };
        ~running_t() {
          if (!open) return;
          if (repair)
            usbip_root::shared_gate().cancel_repair();
          else
            usbip_root::shared_gate().cancel_status();
        }
      } running { repair };

      json result = launch_usbip_root_helper(action);
      if (repair)
        gate.finish_repair(result.dump());
      else
        gate.finish_status(result.dump());
      running.open = false;
      return result;
    }

    void
    getVddStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      const auto status = display_device::vdd_utils::get_vdd_status();
      send_response(response, nlohmann::json {
                                { "status", true },
                                { "state", status.state },
                                { "installed", status.installed },
                                { "running", status.running },
                                { "control_available", status.control_available },
                                { "monitor_active", status.monitor_active },
                                { "problem_code_valid", status.problem_code_valid },
                                { "problem_code", status.problem_code },
                                { "version_supported", false },
                              });
    }
#endif

#ifdef _WIN32
    void
    getUsbipRootStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      send_response(response, run_usbip_root_helper("status"));
    }

    void
    repairUsbipRoot(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      send_response(response, run_usbip_root_helper("repair"));
    }
#endif

#ifdef _WIN32
    void
    writeVulkanHdrBridgeStatus(resp_https_t response, bool operation_status) {
      const auto bridge_status = platf::vulkan_hdr_bridge::status();
      nlohmann::json output {
        { "status", operation_status },
        { "state", bridge_status.state },
        { "message", bridge_status.message },
        { "registered", bridge_status.registered },
        { "artifacts_installed", bridge_status.artifacts_installed },
        { "display_available", bridge_status.display_available },
      };
      send_response(std::move(response), output);
    }
#endif

#ifdef _WIN32
    void
    getVulkanHdrBridgeStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      writeVulkanHdrBridgeStatus(response, true);
    }
#endif

#ifdef _WIN32
    void
    validateVulkanHdrBridge(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      writeVulkanHdrBridgeStatus(response, platf::vulkan_hdr_bridge::validate_now());
    }
#endif

    void
    getMatLiveStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      const auto snapshot = spatial_audio::mat_live_status.snapshot();
      const char *state = snapshot.state();
      std::string error = snapshot.last_error;
#ifdef _WIN32
      if (const auto capture_error = spatial_audio::mat_capture_process.error(); !snapshot.leases && !capture_error.empty()) {
        state = "error";
        error = capture_error;
      }
#endif
      if (!config::audio.mat_live) {
        state = "disabled";
      }
      else if (!config::audio.stream) {
        state = "audio_disabled";
      }
#ifndef _WIN32
      state = "unsupported";
#endif
      // Observe session activity and the owned helper; never probe the capture pipe here.
      const json output {
        { "state", state },
        { "last_error", error },
      };
      const web::CaseInsensitiveMultimap headers {
        { "Content-Type", "application/json" },
        { "Cache-Control", "no-store" },
        { "X-Frame-Options", "DENY" },
        { "Content-Security-Policy", "frame-ancestors 'none';" },
      };
      response->write(output.dump(), headers);
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/restart$"]["GET"] = restart;
    server.resource["^/api/boom$"]["GET"] = boom;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/microphone/test$"]["POST"] = testMicrophone;
    server.resource["^/api/microphone/status$"]["GET"] = getMicrophoneStatus;
    server.resource["^/api/audio/devices$"]["GET"] = getAudioDevices;
    server.resource["^/api/audio/vb-cable/install$"]["POST"] = installVbCable;
    server.resource["^/api/audio/vb-cable/enable$"]["POST"] = enableVbCable;
    server.resource["^/api/mat/status$"]["GET"] = getMatLiveStatus;
#ifdef _WIN32
    server.resource["^/api/vdd/status$"]["GET"] = getVddStatus;
    server.resource["^/api/usbip-root/status$"]["GET"] = getUsbipRootStatus;
    server.resource["^/api/usbip-root/repair$"]["POST"] = repairUsbipRoot;
#endif
#ifdef _WIN32
    server.resource["^/api/vulkan-hdr-bridge$"]["GET"] = getVulkanHdrBridgeStatus;
#endif
#ifdef _WIN32
    server.resource["^/api/vulkan-hdr-bridge/validate$"]["POST"] = validateVulkanHdrBridge;
#endif
  }

}  // namespace confighttp::diagnostics
