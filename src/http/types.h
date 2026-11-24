/**
 * @file src/http/types.h
 * @brief Application HTTP values, independent of listener and TLS policy.
 */
#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace web {
  using error_code = boost::system::error_code;
  using StatusCode = boost::beast::http::status;
  struct HTTP {};
  struct HTTPS {};
  struct PairedHTTPS {};

  struct CaseInsensitiveLess {
    bool
    operator()(std::string_view left, std::string_view right) const {
      const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
      return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), [&](unsigned char a, unsigned char b) { return lower(a) < lower(b); });
    }
  };
  using CaseInsensitiveMultimap = std::multimap<std::string, std::string, CaseInsensitiveLess>;

  struct Percent {
    static std::string
    encode(std::string_view value);
    static std::string
    decode(std::string_view value);
  };
  CaseInsensitiveMultimap
  parse_query(std::string_view query);
  std::string
  status_code(StatusCode status);
  std::string
  decode_base64(std::string_view value);

  class Content: public std::istringstream {
  public:
    using std::istringstream::istringstream;
    size_t
    size() const { return str().size(); }
    std::string
    string() const { return str(); }
  };

  struct Request {
    std::string method, path, query_string, http_version;
    Content content;
    CaseInsensitiveMultimap header;
    std::smatch path_match;
    std::chrono::system_clock::time_point header_read_time;
    bool keep_alive = false;
    unsigned version = 11;
    // Set only by the successful TLS peer policy, carried by this connection
    // across keep-alive requests. Never read identity from HTTP headers.
    std::string client_uuid;
    std::string certificate_sha256;
    boost::asio::ip::tcp::endpoint remote, local;
    auto
    remote_endpoint() const noexcept { return remote; }
    auto
    local_endpoint() const noexcept { return local; }
    CaseInsensitiveMultimap
    parse_query_string() const { return parse_query(query_string); }
  };
}  // namespace web
