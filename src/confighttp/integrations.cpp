/**
 * @file src/confighttp/integrations.cpp
 * @brief Web UI integrations handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "integrations.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/image_enhancement/api.h"
#include "src/webhook/webhook_api.h"
#ifdef _WIN32
  #include "src/ds5/config_api.h"
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::integrations {

  using namespace confighttp;

  namespace {
    void
    getImageEnhancementConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request) || !require_localhost(response, request, "Image enhancement configuration")) return;
      image_enhancement::api::get_config(response);
    }

    void
    saveImageEnhancementConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request) || !require_localhost(response, request, "Image enhancement configuration")) return;
      image_enhancement::api::save_config(response, request);
    }

    void
    getImageEnhancementStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request) || !require_localhost(response, request, "Image enhancement status")) return;
      image_enhancement::api::get_status(response);
    }

    void
    getEnhancementSessions(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request) || !require_localhost(response, request, "Session image enhancement")) return;
      image_enhancement::api::get_sessions(response);
    }

    void
    setSessionNr(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request) || !require_localhost(response, request, "Session image enhancement")) return;
      image_enhancement::api::set_session_nr(response, request);
    }

    void
    rememberSessionNr(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request) || !require_localhost(response, request, "Session image enhancement")) return;
      image_enhancement::api::remember_session_nr(response, request);
    }

    void
    maintainImageEnhancementComponent(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request) || !require_localhost(response, request, "Image enhancement maintenance")) return;
      image_enhancement::api::maintenance(response, request);
    }

    void
    getWebhookConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      webhook::api::get_config(std::move(response), config::sunshine.config_file);
    }

    void
    saveWebhookConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      webhook::api::save_config(
        std::move(response),
        std::move(request),
        config::sunshine.config_file);
    }

    void
    testWebhook(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      webhook::api::test_delivery(std::move(response), std::move(request));
    }

#ifdef _WIN32
    void
    getDualSenseConfig(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      ds5_config::api::get_config(std::move(response), config::sunshine.config_file);
    }
#endif

#ifdef _WIN32
    void
    saveDualSenseConfig(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      ds5_config::api::save_config(
        std::move(response),
        std::move(request),
        config::sunshine.config_file);
    }
#endif

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/hdr-enhanced/config$"]["GET"] = getImageEnhancementConfig;
    server.resource["^/api/hdr-enhanced/config$"]["POST"] = saveImageEnhancementConfig;
    server.resource["^/api/hdr-enhanced/status$"]["GET"] = getImageEnhancementStatus;
    server.resource["^/api/hdr-enhanced/sessions$"]["GET"] = getEnhancementSessions;
    server.resource["^/api/hdr-enhanced/session-nr$"]["POST"] = setSessionNr;
    server.resource["^/api/hdr-enhanced/session-nr/remember$"]["POST"] = rememberSessionNr;
    server.resource["^/api/hdr-enhanced/components/([a-z0-9_.-]+)/maintenance$"]["POST"] = maintainImageEnhancementComponent;
    server.resource["^/api/webhook/config$"]["GET"] = getWebhookConfig;
    server.resource["^/api/webhook/config$"]["POST"] = saveWebhookConfig;
    server.resource["^/api/webhook/test$"]["POST"] = testWebhook;
#ifdef _WIN32
    server.resource["^/api/dualsense/config$"]["GET"] = getDualSenseConfig;
#endif
#ifdef _WIN32
    server.resource["^/api/dualsense/config$"]["POST"] = saveDualSenseConfig;
#endif
  }

}  // namespace confighttp::integrations
