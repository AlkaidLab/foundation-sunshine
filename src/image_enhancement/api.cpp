/**
 * @file src/image_enhancement/api.cpp
 * @brief Authenticated HDR configuration and component coordination.
 */
#include "api.h"
#include "config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/video.h"
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/atomic.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>
#include <utility>

namespace image_enhancement::api {
  namespace {
    using json = nlohmann::json;
    boost::atomic<bool> save_pending { false };
    boost::mutex executor_mutex;
    std::unique_ptr<boost::asio::thread_pool> executor;
    bool executor_stopping = false;

    boost::asio::thread_pool &
    config_executor() {
      // 全局 task_pool 还承担输入定时任务；大文件校验不能占用它的唯一线程。
      // 仅首次保存时创建，不在未使用 HDR 的普通启动路径上创建工作线程。
      if (!executor) executor = std::make_unique<boost::asio::thread_pool>(1);
      return *executor;
    }

    void
    write(response_t response, int status, const json &body, const std::string &etag = {}) noexcept {
      try {
        SimpleWeb::CaseInsensitiveMultimap headers { { "Content-Type", "application/json" }, { "Cache-Control", "no-store" } };
        if (!etag.empty()) headers.emplace("ETag", etag);
        response->write(static_cast<SimpleWeb::StatusCode>(status), body.dump(), headers);
      }
      catch (...) { /* 连接退出不能让可选管理模块终止主进程。 */
      }
    }

    void
    write_result(response_t response, const result_t &result) {
      if (result.status != 200) {
        write(response, result.status, { { "status", false }, { "error_code", result.error } });
      }
      else {
        write(response, 200, { { "status", true }, { "config", settings_json(result.settings) }, { "changed", result.changed } }, result.etag);
      }
    }

    std::optional<std::string_view>
    one_header(const request_t &request, const char *name) {
      const auto [first, last] = request->header.equal_range(name);
      if (first == last) return std::nullopt;
      auto next = first;
      if (++next != last) return std::string_view {};
      return first->second;
    }

    json
    request_json(const request_t &request) {
      if (request->content.size() > 64 * 1024) throw std::length_error("body_too_large");
      return json::parse(request->content.string());
    }
  }  // namespace

  void
  get_config(response_t response) noexcept {
    try {
      write_result(response, manager().query());
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "hdr_config_unavailable" } });
    }
  }

  void
  save_config_impl(response_t response, request_t request) noexcept {
    try {
      settings_t settings;
      if (!parse_settings(request_json(request), settings)) {
        write(response, 400, { { "status", false }, { "error_code", "hdr_config_invalid" } });
        return;
      }
      const auto result = manager().update(settings, one_header(request, "If-Match"), one_header(request, "X-HDR-Operation").value_or(""));
      if (result.status == 200 && result.changed) BOOST_LOG(info) << "HDR enhancement settings saved; new streaming pipelines will use the updated selection";
      write_result(response, result);
    }
    catch (const std::length_error &) {
      write(response, 413, { { "status", false }, { "error_code", "hdr_request_too_large" } });
    }
    catch (const json::exception &) {
      write(response, 400, { { "status", false }, { "error_code", "hdr_config_invalid" } });
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "hdr_save_failed" } });
    }
  }

  void
  queue_operation(response_t response, request_t request,
    void (*operation)(response_t, request_t) noexcept) noexcept {
    if (request->content.size() > 64 * 1024) {
      write(response, 413, { { "status", false }, { "error_code", "hdr_request_too_large" } });
      return;
    }
    if (save_pending.exchange(true)) {
      write(response, 409, { { "status", false }, { "error_code", "hdr_save_busy" } });
      return;
    }
    try {
      boost::lock_guard lock(executor_mutex);
      if (executor_stopping) {
        save_pending.store(false);
        write(response, 503, { { "status", false }, { "error_code", "hdr_shutdown" } });
        return;
      }
      boost::asio::post(config_executor(), [response, request, operation]() {
        struct reset_t {
          ~reset_t() { save_pending.store(false); }
        } reset;
        operation(response, request);
      });
    }
    catch (...) {
      save_pending.store(false);
      write(response, 500, { { "status", false }, { "error_code", "hdr_save_failed" } });
    }
  }

  void
  save_config(response_t response, request_t request) noexcept {
    queue_operation(std::move(response), std::move(request), save_config_impl);
  }

  void
  shutdown() noexcept {
    std::unique_ptr<boost::asio::thread_pool> draining;
    {
      boost::lock_guard lock(executor_mutex);
      executor_stopping = true;
      draining = std::move(executor);
    }
    // 在 confighttp 的 server 和响应对象仍存活时排空唯一的配置任务。
    if (draining) draining->join();
  }

  static json
  session_pipelines() {
    auto pipelines = json::array();
    for (const auto &pipeline : video::get_hdr_pipeline_statuses()) {
      pipelines.push_back({ { "id", pipeline.id }, { "backend", pipeline.synthetic_hdr_backend },
        { "state", pipeline.synthetic_hdr_state }, { "reason", pipeline.synthetic_hdr_failure_reason },
        { "hdr_mode", pipeline.hdr_mode }, { "dv_profile", pipeline.dv_profile }, { "dv_state", pipeline.dv_state }, { "nr_toggle_supported", pipeline.nr_toggle_supported },
        { "nr_live_controls_version", 3 },
        { "nr_requested_style", pipeline.nr_requested_style }, { "nr_style", pipeline.nr_style },
        { "nr_requested_skin_structure_strength", pipeline.nr_requested_skin_structure_strength }, { "nr_skin_structure_strength", pipeline.nr_skin_structure_strength },
        { "nr_requested_auto_mask", pipeline.nr_requested_auto_mask }, { "nr_auto_mask", pipeline.nr_auto_mask },
        { "nr_requested_intensity", pipeline.nr_requested_intensity }, { "nr_intensity", pipeline.nr_intensity },
        { "nr_requested_ui_correction", pipeline.nr_requested_ui_correction }, { "nr_ui_correction", pipeline.nr_ui_correction },
        { "nr_requested_motion_quality", pipeline.nr_requested_motion_quality }, { "nr_motion_quality", pipeline.nr_motion_quality },
        { "nr_requested_scale_percent", pipeline.nr_requested_scale_percent },
        { "nr_scale_percent", pipeline.nr_scale_percent }, { "nr_settings_failure_reason", pipeline.nr_settings_failure_reason },
        { "nr_source_width", pipeline.nr_source_width }, { "nr_source_height", pipeline.nr_source_height },
        { "nr_requested_enabled", pipeline.nr_requested_enabled }, { "nr_backend", pipeline.nr_backend }, { "nr_state", pipeline.nr_state }, { "nr_reason", pipeline.nr_failure_reason } });
    }
    return pipelines;
  }

  void
  get_sessions(response_t response) noexcept {
    try { write(response, 200, { { "status", true }, { "pipelines", session_pipelines() } }); }
    catch (...) { write(response, 500, { { "status", false }, { "error_code", "nr_status_unavailable" } }); }
  }

  void
  get_status(response_t response) noexcept {
    try {
      auto runtime = manager().status();
      runtime["pipelines"] = session_pipelines();
      write(response, 200, { { "status", true }, { "runtime", runtime } });
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "hdr_status_unavailable" } });
    }
  }

  void
  set_session_nr(response_t response, request_t request) noexcept {
    try {
      const auto input = request_json(request);
      if (!input.at("id").is_number_unsigned() || !input.at("enabled").is_boolean() ||
          (input.contains("scale_percent") && (!input["scale_percent"].is_number_integer() ||
            input["scale_percent"] < 20 || input["scale_percent"] > 100)) ||
          (input.contains("intensity") && (!input["intensity"].is_number() || input["intensity"] < 0 || input["intensity"] > 1)) ||
          (input.contains("ui_correction") && !input["ui_correction"].is_boolean()) ||
          (input.contains("style") && (!input["style"].is_number_integer() || input["style"] < 0 || input["style"] > 4)) ||
          (input.contains("skin_structure_strength") && (!input["skin_structure_strength"].is_number() || input["skin_structure_strength"] < 0 || input["skin_structure_strength"] > 1)) ||
          (input.contains("auto_mask") && !input["auto_mask"].is_boolean()) ||
          (input.contains("motion_quality") && (!input["motion_quality"].is_number_integer() || input["motion_quality"] < 0 || input["motion_quality"] > 3))) {
        write(response, 400, { { "status", false }, { "error_code", "nr_request_invalid" } });
        return;
      }
      const auto result = video::request_nr_enabled(input.at("id").get<std::uint64_t>(), input.at("enabled").get<bool>(),
        input.contains("scale_percent") ? std::optional<int>(input["scale_percent"].get<int>()) : std::nullopt,
        input.contains("intensity") ? std::optional<float>(input["intensity"].get<float>()) : std::nullopt,
        input.contains("ui_correction") ? std::optional<bool>(input["ui_correction"].get<bool>()) : std::nullopt,
        input.contains("motion_quality") ? std::optional<int>(input["motion_quality"].get<int>()) : std::nullopt,
        input.contains("style") ? std::optional<int>(input["style"].get<int>()) : std::nullopt,
        input.contains("skin_structure_strength") ? std::optional<float>(input["skin_structure_strength"].get<float>()) : std::nullopt,
        input.contains("auto_mask") ? std::optional<bool>(input["auto_mask"].get<bool>()) : std::nullopt);
      write(response, result, { { "status", result == 202 },
        { "error_code", result == 400 ? "nr_request_invalid" : result == 404 ? "nr_session_ended" : result == 409 ? "nr_toggle_unsupported" : "" } });
    }
    catch (const std::length_error &) {
      write(response, 413, { { "status", false }, { "error_code", "nr_request_too_large" } });
    }
    catch (const json::exception &) {
      write(response, 400, { { "status", false }, { "error_code", "nr_request_invalid" } });
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "nr_request_failed" } });
    }
  }

  void
  maintenance_impl(response_t response, request_t request) noexcept {
    try {
      const std::string backend = request->path_match[1].str();
      if (backend != NVIDIA_RTX_VIDEO_BACKEND && backend != NVIDIA_DLSSNR_BACKEND) {
        write(response, 404, { { "status", false }, { "error_code", "hdr_backend_unknown" } });
        return;
      }
      const auto input = request_json(request);
      const auto action = input.at("action").get<std::string>();
      result_t result;
      std::string operation_id;
      if (action == "begin")
        result = manager().begin_maintenance(backend, operation_id);
      else if (action == "inspect")
        result = manager().inspect_maintenance(backend, operation_id);
      else if (action == "verify") {
        operation_id = input.at("operation_id").get<std::string>();
        result = manager().verify_maintenance(backend, operation_id);
      }
      else if (action == "defer_remove") {
        operation_id = input.at("operation_id").get<std::string>();
        result = manager().defer_removal(backend, operation_id);
      }
      else if (action == "recover")
        result = manager().recover_maintenance(backend);
      else if (action == "commit" || action == "cancel") {
        operation_id = input.at("operation_id").get<std::string>();
        result = manager().finish_maintenance(backend, operation_id);
      }
      else { result = { 400, "hdr_maintenance_invalid" }; }
      if (result.status == 200)
        write(response, 200, { { "status", true }, { "operation_id", operation_id }, { "journal_path", file_handler::path_to_utf8(manager().maintenance_path()) } });
      else
        write_result(response, result);
    }
    catch (const std::length_error &) {
      write(response, 413, { { "status", false }, { "error_code", "hdr_request_too_large" } });
    }
    catch (const json::exception &) {
      write(response, 400, { { "status", false }, { "error_code", "hdr_maintenance_invalid" } });
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "hdr_maintenance_failed" } });
    }
  }

  void
  maintenance(response_t response, request_t request) noexcept {
    // 结束/恢复维护需要重新校验文件，同样不能占用 HTTPS 服务线程。
    queue_operation(std::move(response), std::move(request), maintenance_impl);
  }
}  // namespace image_enhancement::api
