/**
 * @file src/confighttp.cpp
 * @brief Web UI HTTPS listener lifecycle and module shutdown.
 */
#include "confighttp.h"
#include "ai/http_stream.h"
#include "config.h"
#include "confighttp/routes.h"
#include "confighttp/types.h"
#include "globals.h"
#include "image_enhancement/api.h"
#include "logging.h"
#include "network.h"
#include <atomic>
#include <latch>
#include <optional>
#include <thread>

using namespace std::literals;

namespace confighttp {
  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    register_routes(server);
    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;
    // Use a small thread pool so that a slow request handler (proxy upstream,
    // file upload, etc.) doesn't block other web UI requests on the same
    // single-threaded io_service.
    server.config.thread_pool_size = 2;
    // Enforce the limit while Beast is parsing and buffering the request, before
    // individual handlers parse it. This still leaves headroom for the 10 MiB
    // base64 cover-upload endpoint.
    server.config.max_request_streambuf_size = 16 * 1024 * 1024;

    // A concrete bind address intentionally limits remote Sunshine traffic to
    // one interface. Keep an IPv4 loopback-only Web UI listener so the local
    // GUI/tray and Control Panel entry points remain available. The Panel
    // intentionally uses 127.0.0.1, including when the configured address is
    // IPv6.
    std::optional<https_server_t> loopback_server;
    if (!config::sunshine.bind_address.empty()) {
      boost::system::error_code bind_error;
      const auto configured_address = boost::asio::ip::make_address(config::sunshine.bind_address, bind_error);
      if (!bind_error && (configured_address.is_v6() || !configured_address.is_unspecified())) {
        const bool already_uses_panel_loopback = configured_address.is_v4() &&
                                                 configured_address.to_v4() == boost::asio::ip::address_v4::loopback();
        if (!already_uses_panel_loopback) {
          loopback_server.emplace(config::nvhttp.cert, config::nvhttp.pkey);
          loopback_server->default_resource = server.default_resource;
          loopback_server->resource = server.resource;
          loopback_server->config.reuse_address = true;
          loopback_server->config.address = "127.0.0.1";
          loopback_server->config.port = port_https;
          loopback_server->config.thread_pool_size = server.config.thread_pool_size;
          loopback_server->config.max_request_streambuf_size = server.config.max_request_streambuf_size;
        }
      }
    }

    auto accept_and_run = [&](https_server_t *server,
                            const char *listener_name,
                            std::string listener_address,
                            bool required,
                            const std::shared_ptr<std::latch> &startup_latch,
                            const std::shared_ptr<std::atomic_bool> &startup_signaled) {
      const auto signal_startup = [&]() {
        if (startup_latch && startup_signaled && !startup_signaled->exchange(true)) {
          startup_latch->count_down();
        }
      };
      try {
        server->start([listener_name,
                        listener_address = std::move(listener_address),
                        startup_latch,
                        startup_signaled](unsigned short port) {
          if (startup_latch && startup_signaled && !startup_signaled->exchange(true)) {
            startup_latch->count_down();
          }
          BOOST_LOG(debug) << "Configuration UI listener ["sv << listener_name << "] ready at [https://"
                           << listener_address << ':' << port << "]"sv;
        });
      }
      catch (boost::system::system_error &err) {
        signal_startup();
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }
        if (!required) {
          BOOST_LOG(warning) << "Optional Configuration UI listener ["sv << listener_name
                             << "] could not start on ["sv << listener_address << ':' << port_https
                             << "]: "sv << err.what();
          return;
        }
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
      catch (std::exception &err) {
        signal_startup();
        if (shutdown_event->peek()) {
          return;
        }
        if (!required) {
          BOOST_LOG(warning) << "Optional Configuration UI listener ["sv << listener_name
                             << "] failed on ["sv << listener_address << ':' << port_https
                             << "]: "sv << err.what();
          return;
        }
        BOOST_LOG(fatal) << "Configuration HTTPS server failed to start: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    const auto listener_host = [](const std::string &address) {
      boost::system::error_code error;
      const auto parsed = boost::asio::ip::make_address(address, error);
      return error ? address : net::addr_to_url_escaped_string(parsed);
    };

    std::thread tcp {
      accept_and_run,
      &server,
      "configured",
      listener_host(server.config.address),
      true,
      nullptr,
      nullptr
    };
    std::thread loopback_tcp;
    std::shared_ptr<std::latch> loopback_startup_latch;
    std::shared_ptr<std::atomic_bool> loopback_startup_signaled;
    if (loopback_server) {
      loopback_startup_latch = std::make_shared<std::latch>(1);
      loopback_startup_signaled = std::make_shared<std::atomic_bool>(false);
      loopback_tcp = std::thread {
        accept_and_run,
        &*loopback_server,
        "loopback",
        listener_host(loopback_server->config.address),
        false,
        loopback_startup_latch,
        loopback_startup_signaled
      };
      loopback_startup_latch->wait();
    }

    // Wait for any event
    shutdown_event->view();

    ai::http::shutdown_streams();
    image_enhancement::api::shutdown();
    server.stop();
    if (loopback_server) {
      loopback_server->stop();
    }

    tcp.join();
    if (loopback_tcp.joinable()) {
      loopback_tcp.join();
    }
  }
}  // namespace confighttp
