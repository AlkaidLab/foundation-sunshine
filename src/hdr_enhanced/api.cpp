/**
 * @file src/hdr_enhanced/api.cpp
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

namespace hdr_enhanced::api {
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

  void
  get_status(response_t response) noexcept {
    try {
      auto runtime = manager().status();
      runtime["pipelines"] = json::array();
      for (const auto &pipeline : video::get_hdr_pipeline_statuses()) {
        runtime["pipelines"].push_back({ { "id", pipeline.id }, { "backend", pipeline.synthetic_hdr_backend },
          { "state", pipeline.synthetic_hdr_state }, { "reason", pipeline.synthetic_hdr_failure_reason } });
      }
      write(response, 200, { { "status", true }, { "runtime", runtime } });
    }
    catch (...) {
      write(response, 500, { { "status", false }, { "error_code", "hdr_status_unavailable" } });
    }
  }

  void
  maintenance_impl(response_t response, request_t request) noexcept {
    try {
      const auto input = request_json(request);
      const auto action = input.at("action").get<std::string>();
      result_t result;
      std::string operation_id;
      if (action == "begin")
        result = manager().begin_maintenance(NVIDIA_RTX_VIDEO_BACKEND, operation_id);
      else if (action == "inspect")
        result = manager().inspect_maintenance(NVIDIA_RTX_VIDEO_BACKEND, operation_id);
      else if (action == "verify") {
        operation_id = input.at("operation_id").get<std::string>();
        result = manager().verify_maintenance(NVIDIA_RTX_VIDEO_BACKEND, operation_id);
      }
      else if (action == "recover")
        result = manager().recover_maintenance(NVIDIA_RTX_VIDEO_BACKEND);
      else if (action == "commit" || action == "cancel") {
        operation_id = input.at("operation_id").get<std::string>();
        result = manager().finish_maintenance(NVIDIA_RTX_VIDEO_BACKEND, operation_id);
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
}  // namespace hdr_enhanced::api
