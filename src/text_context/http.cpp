/**
 * @file src/text_context/http.cpp
 * @brief See text_context/http.h.
 */
#include "http.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "bridge.h"
#include "../logging.h"

namespace text_context::http {
  namespace {
    constexpr std::size_t kMaxBodyBytes = 4096;

    void write_json(resp_https_t &resp, SimpleWeb::StatusCode status, const nlohmann::json &body) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      resp->write(status, body.dump(), headers);
    }

    bool read_int32(const nlohmann::json &value, std::int32_t &result) {
      if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) return false;
        result = static_cast<std::int32_t>(number);
        return true;
      }
      if (!value.is_number_integer()) return false;
      const auto number = value.get<std::int64_t>();
      if (number < std::numeric_limits<std::int32_t>::min() ||
          number > std::numeric_limits<std::int32_t>::max()) return false;
      result = static_cast<std::int32_t>(number);
      return true;
    }

    bool read_json(req_https_t &req, nlohmann::json &body) {
      auto length = req->header.find("Content-Length");
      if (length != req->header.end()) {
        try {
          if (std::stoull(length->second) > kMaxBodyBytes) return false;
        }
        catch (...) { return false; }
      }
      std::stringstream stream;
      stream << req->content.rdbuf();
      const auto text = stream.str();
      if (text.empty() || text.size() > kMaxBodyBytes) return false;
      try {
        body = nlohmann::json::parse(text);
        if (!body.is_object() || !body.contains("version")) return false;
        const auto &version = body["version"];
        return version.is_number_integer() &&
               ((version.is_number_unsigned() && version.get<std::uint64_t>() == 1) ||
                (!version.is_number_unsigned() && version.get<std::int64_t>() == 1));
      }
      catch (...) { return false; }
    }

    bool read_rect(const nlohmann::json &body, const char *name,
                   std::optional<text_context::screen_rect_t> &result) {
      result.reset();
      if (!body.contains(name)) return true;
      if (!body[name].is_object()) return false;
      const auto &value = body[name];
      static constexpr const char *fields[] = {"left", "top", "right", "bottom"};
      for (const auto *field : fields) {
        if (!value.contains(field)) return false;
      }
      text_context::screen_rect_t rect;
      if (!read_int32(value["left"], rect.left) ||
          !read_int32(value["top"], rect.top) ||
          !read_int32(value["right"], rect.right) ||
          !read_int32(value["bottom"], rect.bottom)) return false;
      if (rect.valid()) result = rect;
      return true;
    }
  }  // namespace

  void register_routes(https_server_t &server, auth_fn auth) {
    server.resource["^/api/v1/text-context/capability$"]["POST"] = [auth](resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) return;
      nlohmann::json body;
      if (!read_json(req, body)) {
        write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_body"}});
        return;
      }
      auto &bridge = text_context::bridge_t::instance();
      try {
        bridge.notify_gui_alive(body.value("input_pane", false), body.value("uia", false));
      }
      catch (const nlohmann::json::exception &) {
        write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_body"}});
        return;
      }
      BOOST_LOG(debug) << "Remote text context GUI capability: input_pane="
                      << bridge.input_pane_available() << ", uia=" << bridge.uia_available();
      write_json(resp, SimpleWeb::StatusCode::success_ok, {
        {"ok", true}, {"input_pane", bridge.input_pane_available()}, {"uia", bridge.uia_available()},
      });
    };

    server.resource["^/api/v1/text-context/observation$"]["POST"] = [auth](resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) return;
      nlohmann::json body;
      if (!read_json(req, body)) {
        write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_body"}});
        return;
      }
      text_context::observation_t observation;
      std::string source;
      try {
        source = body.value("source", std::string {});
        if (source == "input_pane") observation.source = text_context::source_e::input_pane;
        else if (source == "uia") observation.source = text_context::source_e::uia;
        else {
          write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_source"}});
          return;
        }
        observation.active = body.value("active", false);
        observation.editable = body.value("editable", false);
        observation.password = body.value("password", false);
        observation.multiline = body.value("multiline", false);
        observation.pane_visible = body.value("pane_visible", false);
        observation.auto_show = body.value("auto_show", false);
        if (!read_rect(body, "element_rect", observation.element_rect) ||
            !read_rect(body, "caret_rect", observation.caret_rect)) {
          write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_body"}});
          return;
        }
      }
      catch (const nlohmann::json::exception &) {
        write_json(resp, SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_body"}});
        return;
      }

      auto &bridge = text_context::bridge_t::instance();
      const bool matched = bridge.observe(observation);
      BOOST_LOG(debug) << "Remote text context observation: source=" << source
                      << ", active=" << observation.active
                      << ", editable=" << observation.editable
                      << ", has_element_rect=" << observation.element_rect.has_value()
                      << ", matched=" << matched;
      write_json(resp, SimpleWeb::StatusCode::success_accepted, {{"ok", true}, {"matched", matched}});
    };
  }
}  // namespace text_context::http
