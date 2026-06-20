/**
 * @file src/file_transfer_http.cpp
 * @brief See file_transfer_http.h.
 */
#include "file_transfer_http.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "config.h"
#include "file_transfer_store.h"

namespace file_transfer_http {
  namespace {
    response_t
    json_response(SimpleWeb::StatusCode status, std::string body) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("Cache-Control", "no-store");
      return { status, std::move(body), std::move(headers) };
    }

    std::string
    json_error(const std::string &err) {
      nlohmann::json out;
      out["error"] = err;
      return out.dump();
    }

    bool
    is_hex_id(const std::string &id) {
      if (id.size() != 64) {
        return false;
      }
      return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
    }

    std::filesystem::path
    path_from_utf8(const std::string &raw) {
#if defined(_WIN32)
      std::u8string u8(raw.begin(), raw.end());
      return std::filesystem::path(u8);
#else
      return std::filesystem::path(raw);
#endif
    }

    std::string
    percent_encode(const std::string &raw) {
      static constexpr char kHex[] = "0123456789ABCDEF";
      std::string out;
      out.reserve(raw.size());
      for (unsigned char c : raw) {
        const bool unreserved =
          (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
          out.push_back(static_cast<char>(c));
        } else {
          out.push_back('%');
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0x0F]);
        }
      }
      return out;
    }

    std::string
    ascii_fallback_filename(std::string name) {
      for (char &c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc >= 0x7F || c == '"' || c == '\\') {
          c = '_';
        }
      }
      if (name.empty()) {
        return "download";
      }
      return name;
    }

    SimpleWeb::CaseInsensitiveMultimap
    download_headers(const std::string &name, std::uint64_t size) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/octet-stream");
      headers.emplace("X-Content-Type-Options", "nosniff");
      headers.emplace("Cache-Control", "no-store");
      headers.emplace("Content-Length", std::to_string(size));
      headers.emplace("X-Sunshine-File-Name", ascii_fallback_filename(name));
      headers.emplace("Content-Disposition",
        "attachment; filename=\"" + ascii_fallback_filename(name) + "\"; filename*=UTF-8''" + percent_encode(name));
      return headers;
    }

    void
    handle_offer(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      auto out = process_offer(req);
      resp->write(out.status, out.body, out.headers);
    }

    void
    handle_download(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      write_download_response(resp, process_download(req));
    }
  }  // namespace

  response_t
  make_offer_response(const std::string &body) {
    if (!config::input.file_transfer) {
      return json_response(SimpleWeb::StatusCode::client_error_forbidden,
        json_error("file_transfer_disabled"));
    }

    if (body.empty()) {
      return json_response(SimpleWeb::StatusCode::client_error_bad_request,
        json_error("empty_body"));
    }

    nlohmann::json in;
    try {
      in = nlohmann::json::parse(body);
    } catch (...) {
      return json_response(SimpleWeb::StatusCode::client_error_bad_request,
        json_error("bad_json"));
    }

    const auto path_value = in.find("path");
    if (path_value == in.end() || !path_value->is_string() || path_value->get<std::string>().empty()) {
      return json_response(SimpleWeb::StatusCode::client_error_bad_request,
        json_error("missing_path"));
    }

    const auto created = file_transfer_store::create_single_file_offer(path_from_utf8(path_value->get<std::string>()));
    if (!created.ok) {
      const auto status = created.err == "too_large" ?
                            SimpleWeb::StatusCode::client_error_payload_too_large :
                            SimpleWeb::StatusCode::client_error_bad_request;
      return json_response(status, json_error(created.err.empty() ? "offer_failed" : created.err));
    }

    nlohmann::json out;
    out["id"] = created.offer.id;
    out["name"] = created.offer.display_name;
    out["size"] = created.offer.size;
    out["mime"] = "application/octet-stream";
    out["download_url"] = "/api/v1/file-transfer/" + created.offer.id;
    out["expires_in"] = file_transfer_store::kOfferTtlSeconds;
    out["type"] = "file";

    return json_response(SimpleWeb::StatusCode::success_ok, out.dump());
  }

  download_response_t
  make_download_response(const std::string &id) {
    if (!config::input.file_transfer) {
      auto out = json_response(SimpleWeb::StatusCode::client_error_forbidden,
        json_error("file_transfer_disabled"));
      return { out.status, std::move(out.body), std::move(out.headers), {}, false };
    }

    if (!is_hex_id(id)) {
      auto out = json_response(SimpleWeb::StatusCode::client_error_not_found,
        json_error("not_found"));
      return { out.status, std::move(out.body), std::move(out.headers), {}, false };
    }

    const auto got = file_transfer_store::get(id);
    if (!got.found) {
      auto out = json_response(SimpleWeb::StatusCode::client_error_not_found,
        json_error("not_found"));
      return { out.status, std::move(out.body), std::move(out.headers), {}, false };
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(got.offer.path, ec) || ec) {
      auto out = json_response(SimpleWeb::StatusCode::client_error_not_found,
        json_error("not_found"));
      return { out.status, std::move(out.body), std::move(out.headers), {}, false };
    }

    const auto size = std::filesystem::file_size(got.offer.path, ec);
    if (ec) {
      auto out = json_response(SimpleWeb::StatusCode::server_error_internal_server_error,
        json_error("stat_failed"));
      return { out.status, std::move(out.body), std::move(out.headers), {}, false };
    }

    return {
      SimpleWeb::StatusCode::success_ok,
      {},
      download_headers(got.offer.display_name, static_cast<std::uint64_t>(size)),
      got.offer.path,
      true
    };
  }

  void
  register_routes(https_server_t &server, auth_fn auth) {
    auto auth_file_transfer = std::make_shared<auth_fn>(std::move(auth));

    server.resource["^/api/v1/file-transfer/offers$"]["POST"] =
      [auth_file_transfer](resp_https_t resp, req_https_t req) {
        handle_offer(*auth_file_transfer, std::move(resp), std::move(req));
      };

    server.resource["^/api/v1/file-transfer/([a-f0-9]{64})$"]["GET"] =
      [auth_file_transfer](resp_https_t resp, req_https_t req) {
        handle_download(*auth_file_transfer, std::move(resp), std::move(req));
      };
  }
}  // namespace file_transfer_http
