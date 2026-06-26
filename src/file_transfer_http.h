/**
 * @file src/file_transfer_http.h
 * @brief HTTP helpers for host-to-client file-transfer offers.
 */
#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include <Simple-Web-Server/server_https.hpp>

namespace file_transfer_http {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  struct response_t {
    SimpleWeb::StatusCode status;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;
  };

  struct download_response_t {
    SimpleWeb::StatusCode status;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::filesystem::path path;
    bool stream_file;
  };

  response_t make_offer_response(const std::string &body);
  download_response_t make_download_response(const std::string &id);

  template <typename Request>
  inline response_t process_offer(const Request &req) {
    std::stringstream ss;
    ss << req->content.rdbuf();
    return make_offer_response(ss.str());
  }

  template <typename Request>
  inline download_response_t process_download(const Request &req) {
    const std::string id = req->path_match.size() >= 2 ? req->path_match[1].str() : std::string {};
    return make_download_response(id);
  }

  template <typename Response>
  inline void write_download_response(const Response &resp, download_response_t out) {
    if (!out.stream_file) {
      resp->write(out.status, out.body, out.headers);
      return;
    }

    std::ifstream in(out.path, std::ios::binary);
    if (!in) {
      SimpleWeb::CaseInsensitiveMultimap err_headers;
      err_headers.emplace("Content-Type", "application/json");
      err_headers.emplace("Cache-Control", "no-store");
      resp->write(SimpleWeb::StatusCode::client_error_not_found,
        R"({"error":"not_found"})", err_headers);
      return;
    }
    resp->write(out.status, in, out.headers);
  }

  void register_routes(https_server_t &server, auth_fn auth);
}  // namespace file_transfer_http
