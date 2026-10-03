#include "abr_api.h"
#include "legacy_control.h"

#include <algorithm>
#include <sstream>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>

#include "src/abr.h"
#include "src/config.h"
#include "src/confighttp.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/video.h"

using json = nlohmann::json;

namespace nvhttp::abr_api {

  namespace {

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    SimpleWeb::CaseInsensitiveMultimap
    json_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return headers;
    }

    int
    host_max_bitrate_kbps() {
      return std::max(config::video.max_bitrate, 0);
    }

    bool
    apply_host_bitrate_cap(abr::config_t &cfg) {
      const auto host_cap = host_max_bitrate_kbps();
      if (host_cap <= 0) {
        return false;
      }

      const auto requested_max = cfg.max_bitrate_kbps;
      cfg.max_bitrate_kbps = requested_max > 0 ? std::min(requested_max, host_cap) : host_cap;
      if (cfg.min_bitrate_kbps > cfg.max_bitrate_kbps) {
        cfg.min_bitrate_kbps = cfg.max_bitrate_kbps;
      }

      return requested_max <= 0 || cfg.max_bitrate_kbps != requested_max;
    }

    int
    clamp_bitrate_to_range(int bitrate, const abr::config_t &cfg) {
      if (cfg.min_bitrate_kbps > 0 && cfg.max_bitrate_kbps > 0) {
        return std::clamp(bitrate, cfg.min_bitrate_kbps, cfg.max_bitrate_kbps);
      }
      if (cfg.max_bitrate_kbps > 0) {
        return std::min(bitrate, cfg.max_bitrate_kbps);
      }
      if (cfg.min_bitrate_kbps > 0) {
        return std::max(bitrate, cfg.min_bitrate_kbps);
      }
      return bitrate;
    }

  }  // namespace

  void
  capabilities(resp_https_t response, req_https_t request) {
    log_request(request);

    auto caps = abr::get_capabilities();

    json resp_json;
    resp_json["supported"] = caps.supported;
    resp_json["version"] = caps.version;
    resp_json["features"] = json::array({ "llm_ai", "game_aware", "fallback_threshold", "bitrate_cap", "connection_scope" });
    resp_json["llmEnabled"] = confighttp::isAiEnabled();
    resp_json["hostMaxBitrate"] = host_max_bitrate_kbps();

    response->write(SimpleWeb::StatusCode::success_ok, resp_json.dump(), json_headers());
  }

  void
  configure(resp_https_t response, req_https_t request) {
    log_request(request);

    auto headers = json_headers();

    try {
      auto body = transport::parse_legacy_control_body(request->content.string());
      auto client = legacy_control::resolve(request, transport::parse_legacy_scope(body));

      bool enabled = body.value("enabled", false);

      if (!enabled) {
        abr::disable(client.abr_key);
        json resp_json;
        resp_json["success"] = true;
        resp_json["enabled"] = false;
        response->write(SimpleWeb::StatusCode::success_ok, resp_json.dump(), headers);
        return;
      }

      std::string mode_str = body.value("mode", "balanced");
      abr::mode_e mode;
      if (mode_str == "balanced") {
        mode = abr::mode_e::BALANCED;
      }
      else if (mode_str == "quality") {
        mode = abr::mode_e::QUALITY;
      }
      else if (mode_str == "lowLatency") {
        mode = abr::mode_e::LOW_LATENCY;
      }
      else {
        json err;
        err["success"] = false;
        err["error"] = "Invalid mode: must be 'balanced', 'quality', or 'lowLatency'";
        response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
        return;
      }

      abr::config_t cfg;
      cfg.enabled = true;
      cfg.min_bitrate_kbps = body.value("minBitrate", 0);
      cfg.max_bitrate_kbps = body.value("maxBitrate", 0);
      cfg.mode = mode;
      const auto requested_max_bitrate = cfg.max_bitrate_kbps;
      const auto host_max_bitrate = host_max_bitrate_kbps();

      if (cfg.min_bitrate_kbps < 0 || cfg.max_bitrate_kbps < 0) {
        json err;
        err["success"] = false;
        err["error"] = "minBitrate and maxBitrate must be non-negative";
        response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
        return;
      }
      if (cfg.min_bitrate_kbps > 0 && cfg.max_bitrate_kbps > 0 && cfg.min_bitrate_kbps > cfg.max_bitrate_kbps) {
        json err;
        err["success"] = false;
        err["error"] = "minBitrate must not exceed maxBitrate";
        response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
        return;
      }

      apply_host_bitrate_cap(cfg);
      const auto capped_by_host = host_max_bitrate > 0 && requested_max_bitrate > 0 && cfg.max_bitrate_kbps < requested_max_bitrate;
      const auto inherited_host_cap = host_max_bitrate > 0 && requested_max_bitrate <= 0;

      int initial_bitrate = client.session.bitrate > 0 ? client.session.bitrate
                            : cfg.max_bitrate_kbps > 0 ? cfg.max_bitrate_kbps
                            : 20000;
      initial_bitrate = clamp_bitrate_to_range(initial_bitrate, cfg);

      abr::enable(client.abr_key, cfg, initial_bitrate, client.session.app_name);
      // Stop may have raced enable outside the registry lock. Clean only this
      // connection's key, never a replacement session sharing its display name.
      if (!legacy_control::still_legacy(client)) {
        abr::cleanup(client.abr_key);
        throw legacy_control::error_t(404, "session_stopped");
      }

      bool bitrate_applied = true;
      if (client.session.bitrate > 0 && cfg.max_bitrate_kbps > 0 && client.session.bitrate > cfg.max_bitrate_kbps) {
        video::dynamic_param_t param;
        param.type = video::dynamic_param_type_e::BITRATE;
        param.value.int_value = cfg.max_bitrate_kbps;
        param.valid = true;
        bitrate_applied = legacy_control::submit(client, param);
      }

      json resp_json;
      resp_json["success"] = true;
      resp_json["enabled"] = true;
      resp_json["mode"] = mode_str;
      resp_json["minBitrate"] = cfg.min_bitrate_kbps;
      resp_json["maxBitrate"] = cfg.max_bitrate_kbps;
      resp_json["initialBitrate"] = initial_bitrate;
      resp_json["requestedMaxBitrate"] = requested_max_bitrate;
      resp_json["hostMaxBitrate"] = host_max_bitrate;
      resp_json["maxBitrateCapped"] = capped_by_host;
      resp_json["maxBitrateInheritedFromHost"] = inherited_host_cap;
      resp_json["bitrateApplied"] = bitrate_applied;
      if (!bitrate_applied) {
        resp_json["bitrateApplyError"] = "ABR configured, but failed to apply bitrate to the active session";
      }
      response->write(SimpleWeb::StatusCode::success_ok, resp_json.dump(), headers);
    }
    catch (const legacy_control::error_t &error) {
      response->write(error.http_status(), json({ { "success", false }, { "error", error.what() } }).dump(), headers);
    }
    catch (const std::invalid_argument &error) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request,
        json({ { "success", false }, { "error", error.what() } }).dump(), headers);
    }
    catch (const json::exception &e) {
      BOOST_LOG(warning) << "ABR configure: JSON parse error: " << e.what();
      json err;
      err["success"] = false;
      err["error"] = "Invalid JSON body";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "ABR configure: " << e.what();
      json err;
      err["success"] = false;
      err["error"] = e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
    }
  }

  void
  feedback(resp_https_t response, req_https_t request) {
    auto headers = json_headers();

    try {
      auto body = transport::parse_legacy_control_body(request->content.string());
      auto client = legacy_control::resolve(request, transport::parse_legacy_scope(body));
      if (!abr::is_enabled(client.abr_key)) {
        json err;
        err["error"] = "ABR not enabled for this client";
        response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
        return;
      }

      abr::network_feedback_t feedback;
      feedback.packet_loss = body.value("packetLoss", 0.0);
      feedback.rtt_ms = body.value("rttMs", 0.0);
      feedback.decode_fps = body.value("decodeFps", 0.0);
      feedback.dropped_frames = body.value("droppedFrames", 0);
      feedback.current_bitrate_kbps = body.value("currentBitrate", 0);

      auto action = abr::process_feedback(client.abr_key, feedback);
      bool bitrate_applied = true;
      if (action.new_bitrate_kbps > 0) {
        video::dynamic_param_t param;
        param.type = video::dynamic_param_type_e::BITRATE;
        param.value.int_value = action.new_bitrate_kbps;
        param.valid = true;

        bitrate_applied = legacy_control::submit(client, param);
      }

      json resp_json;
      if (action.new_bitrate_kbps > 0 && bitrate_applied) {
        resp_json["newBitrate"] = action.new_bitrate_kbps;
      }
      resp_json["bitrateApplied"] = bitrate_applied;
      if (!bitrate_applied) {
        resp_json["bitrateApplyError"] = "Failed to apply ABR bitrate update to the active session";
      }
      resp_json["reason"] = action.reason;
      response->write(SimpleWeb::StatusCode::success_ok, resp_json.dump(), headers);
    }
    catch (const legacy_control::error_t &error) {
      response->write(error.http_status(), json({ { "error", error.what() } }).dump(), headers);
    }
    catch (const std::invalid_argument &error) {
      response->write(SimpleWeb::StatusCode::client_error_bad_request, json({ { "error", error.what() } }).dump(), headers);
    }
    catch (const json::exception &e) {
      json err;
      err["error"] = "Invalid JSON body";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "ABR feedback: " << e.what();
      json err;
      err["error"] = e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
    }
  }

}  // namespace nvhttp::abr_api
