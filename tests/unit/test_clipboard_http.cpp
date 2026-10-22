/**
 * @file tests/unit/test_clipboard_http.cpp
 * @brief Clipboard SSE session ID validation regressions.
 */
#include "src/clipboard_http.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
  // Exercise the registered handlers and real response serialization without
  // opening a listener or depending on TLS certificates or a desktop clipboard.
  class ClipboardHttp: public testing::Test {
  protected:
    struct connection_t {
      std::string output;
      bool closed = false;
    };

    clipboard_http::https_server_t server;
    std::vector<std::shared_ptr<connection_t>> connections;

    void
    SetUp() override {
      clipboard_http::register_routes(server, [](auto, auto) { return true; });
    }

    auto
    request(const std::string &path, const std::string &method,
      const web::CaseInsensitiveMultimap &headers = {}) {
      auto connection = std::make_shared<connection_t>();
      connections.push_back(connection);
      auto req = std::make_shared<web::Request>();
      req->path = path;
      req->method = method;
      req->header = headers;
      web::detail::ResponseChannel channel;
      channel.send = [connection](std::string bytes, auto completion, bool, bool) {
        if (!connection->closed) connection->output += bytes;
        if (completion) {
          completion(connection->closed ? make_error_code(boost::system::errc::broken_pipe) : web::error_code {});
        }
      };
      channel.begin_stream = [] {};
      channel.closed = [connection] { return connection->closed; };
      channel.abort = [connection] { connection->closed = true; };
      auto response = std::make_shared<clipboard_http::https_server_t::Response>(*req, std::move(channel));
      server.resource.at("^" + path + "$").at(method)(response, req);
      response->send();
      return connection;
    }

    auto
    subscribe(const web::CaseInsensitiveMultimap &headers = {}) {
      return request("/api/v1/clipboard/events", "GET", headers);
    }

    void
    TearDown() override {
      // A heartbeat exercises the normal failed-send/prune path for every
      // subscription, including filters that do not match any test session.
      for (auto &connection : connections) connection->closed = true;
      request("/api/v1/clipboard/capability", "POST");
    }
  };
}  // namespace

TEST_F(ClipboardHttp, MalformedSessionHeadersReturnBadRequest) {
  for (const std::string value : { "123abc", "-1", "+123", " 123", "123 ", "0x7b", "1.5",
         "18446744073709551616", "999999999999999999999999999999999999" }) {
    SCOPED_TRACE(value);
    const auto connection = subscribe({ { "X-Clipboard-Session-Sid", value } });
    EXPECT_NE(connection->output.find("HTTP/1.1 400"), std::string::npos);
    EXPECT_NE(connection->output.find(R"({"error":"bad_session_sid"})"), std::string::npos);
    EXPECT_EQ(connection->output.find("text/event-stream"), std::string::npos);
  }
}

TEST_F(ClipboardHttp, DecimalSessionHeadersAcceptTheFullUint64Range) {
  for (const std::string value : { "0", "123", "000123", "18446744073709551615" }) {
    SCOPED_TRACE(value);
    const auto connection = subscribe({ { "X-Clipboard-Session-Sid", value } });
    EXPECT_NE(connection->output.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_NE(connection->output.find("text/event-stream"), std::string::npos);
  }
}
