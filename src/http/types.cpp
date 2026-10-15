#include "types.h"
#include <boost/beast/http/status.hpp>
#include <limits>
#include <openssl/evp.h>
#include <stdexcept>

namespace web {
  std::string
  Percent::encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        result += c;
      else {
        result += '%';
        result += hex[c >> 4];
        result += hex[c & 15];
      }
    }
    return result;
  }
  std::string
  Percent::decode(std::string_view value) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '%' && i + 2 < value.size() && digit(value[i + 1]) >= 0 && digit(value[i + 2]) >= 0) {
        result += static_cast<char>((digit(value[i + 1]) << 4) | digit(value[i + 2]));
        i += 2;
      }
      else
        result += value[i] == '+' ? ' ' : value[i];
    }
    return result;
  }
  CaseInsensitiveMultimap
  parse_query(std::string_view query) {
    CaseInsensitiveMultimap result;
    while (!query.empty()) {
      const auto end = query.find('&');
      const auto item = query.substr(0, end);
      const auto equal = item.find('=');
      if (!item.empty()) result.emplace(Percent::decode(item.substr(0, equal)), equal == std::string_view::npos ? "" : Percent::decode(item.substr(equal + 1)));
      if (end == std::string_view::npos) break;
      query.remove_prefix(end + 1);
    }
    return result;
  }
  std::string
  status_code(StatusCode status) {
    return std::to_string(static_cast<unsigned>(status)) + ' ' + std::string(boost::beast::http::obsolete_reason(status));
  }
  std::string
  decode_base64(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() % 4 || value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::invalid_argument("Invalid Base64 input size");
    std::string result(value.size() / 4 * 3, '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(result.data()), reinterpret_cast<const unsigned char *>(value.data()), static_cast<int>(value.size()));
    if (length < 0) throw std::invalid_argument("Invalid Base64 input");
    const size_t padding = (value.back() == '=') + (value.size() > 1 && value[value.size() - 2] == '=');
    result.resize(static_cast<size_t>(length) - padding);
    return result;
  }
}  // namespace web
