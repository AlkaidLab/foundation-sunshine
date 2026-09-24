#include "widget_http.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/http_util.h"
#include "src/logging.h"
#include "src/perf_recorder.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/stream.h"

namespace widget_http {
  using namespace std::literals;

  namespace {
    using nlohmann::json;

    constexpr auto TOKEN_HEADER = "X-Sunshine-Token";

    bool
    token_configured() {
      return !config::sunshine.widget_token.empty();
    }

    bool
    token_valid(req_https_t request) {
      const auto found = request->header.find(TOKEN_HEADER);
      return found != request->header.end() &&
             found->second == config::sunshine.widget_token;
    }

    void
    send_json(resp_https_t response, const json &body) {
      response->write(body.dump(), { { "Content-Type", "application/json" } });
      response->close_connection_after_response = true;
    }

    template <typename Callback>
    void
    send_json(resp_https_t response, const json &body, Callback &&callback) {
      response->write(body.dump(), { { "Content-Type", "application/json" } });
      response->send(std::forward<Callback>(callback));
    }

    void
    send_error(resp_https_t response, SimpleWeb::StatusCode status, std::string error) {
      response->write(status, json {
                                   { "status", false },
                                   { "error", std::move(error) },
                                 }
                         .dump(),
        { { "Content-Type", "application/json" } });
      response->close_connection_after_response = true;
    }

    // Routes stay dark until a token is configured: an unset widget_token means
    // the widget feature is off, and no unauthenticated loopback data hole exists.
    bool
    feature_disabled(resp_https_t response) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      response->close_connection_after_response = true;
      return true;
    }

    void
    get_widget_state(resp_https_t response, req_https_t request, const auth_fn &auth) {
      if (!token_configured()) {
        feature_disabled(std::move(response));
        return;
      }
      if (!auth(response, request)) return;
      if (!token_valid(std::move(request))) {
        send_error(std::move(response), SimpleWeb::StatusCode::client_error_unauthorized, "Invalid widget token");
        return;
      }

      try {
        auto snapshot = perf::current_snapshot_json();

        // 用会话信息补充 perf 快照里没有的上下文(应用名/HDR/状态),按 session_id 对齐
        const auto sessions_info = stream::session::get_all_sessions_info();
        if (snapshot.contains("sessions") && snapshot["sessions"].is_array()) {
          for (auto &item : snapshot["sessions"]) {
            if (!item.is_object() || !item.contains("session_id")) continue;

            const auto session_id = item["session_id"].get<std::uint64_t>();
            const auto info = std::find_if(
              sessions_info.begin(), sessions_info.end(),
              [session_id](const stream::session_info_t &info) {
                return info.session_id == session_id;
              });
            if (info == sessions_info.end()) continue;

            item["app"] = info->app_name;
            item["hdr"] = info->enable_hdr;
            item["state"] = info->state;
            item["client_address"] = info->client_address;
          }
        }

        // 服务器时间戳:widget 侧用它判定数据过期(ts 停止前进 >5s → 徽标)
        snapshot["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
                           .count();

        send_json(std::move(response), snapshot);
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "widget_http: state snapshot failed: " << e.what();
        send_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, e.what());
      }
    }

    void
    post_widget_action(resp_https_t response, req_https_t request, const auth_fn &auth) {
      if (!token_configured()) {
        feature_disabled(std::move(response));
        return;
      }
      if (!auth(response, request)) return;
      if (!token_valid(request)) {
        send_error(std::move(response), SimpleWeb::StatusCode::client_error_unauthorized, "Invalid widget token");
        return;
      }

      const auto content_type = request->header.find("content-type");
      if (content_type == request->header.end() ||
          !http_util::content_type_matches(content_type->second, "application/json")) {
        send_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "Content type must be application/json");
        return;
      }

      try {
        std::stringstream ss;
        ss << request->content.rdbuf();
        const auto body = json::parse(ss.str());
        if (!body.is_object()) {
          send_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "Request body must be a JSON object");
          return;
        }
        const auto type = body.value("type", std::string {});

        if (type == "stop_all_sessions") {
          // 语义:终止当前全部串流会话(所有客户端),与按钮文案一致
          rtsp_stream::terminate_sessions_async(
            stream::session::stop_reason_e::host_terminate,
            boost::function<void()>([] {}));
          BOOST_LOG(info) << "widget_http: session termination requested by Game Bar widget"sv;
          send_json(std::move(response), {
                                            { "status", true },
                                            { "result", "session termination requested" },
                                          });
        }
        else if (type == "restart_service") {
          BOOST_LOG(info) << "widget_http: service restart requested by Game Bar widget"sv;
          // 先回包再重启,与 tray 生命周期动作的回包顺序一致
          send_json(std::move(response), {
                                            { "status", true },
                                            { "result", "restart requested" },
                                          },
            [](const auto &error) {
              if (error) {
                BOOST_LOG(debug) << "widget_http: restart response did not reach the client: "sv << error.message();
              }
              platf::restart();
            });
        }
        else {
          send_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "Unknown action type");
        }
      }
      catch (const json::parse_error &e) {
        send_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, e.what());
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "widget_http: action failed: " << e.what();
        send_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, e.what());
      }
    }
  }  // namespace

  void
  register_routes(https_server_t &server, auth_fn auth) {
    server.resource["^/api/widget/state$"]["GET"] = [auth](resp_https_t response, req_https_t request) {
      get_widget_state(std::move(response), std::move(request), auth);
    };
    server.resource["^/api/widget/action$"]["POST"] = [auth](resp_https_t response, req_https_t request) {
      post_widget_action(std::move(response), std::move(request), auth);
    };
  }
}  // namespace widget_http
