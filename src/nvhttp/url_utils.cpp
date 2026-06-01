#include "url_utils.h"

#include <cctype>

namespace nvhttp::url_utils {

  std::string
  decode(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '%' && i + 2 < value.size()) {
        auto hi = static_cast<unsigned char>(value[i + 1]);
        auto lo = static_cast<unsigned char>(value[i + 2]);
        if (std::isxdigit(hi) && std::isxdigit(lo)) {
          auto hex_value = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
          };
          decoded += static_cast<char>((hex_value(value[i + 1]) << 4) | hex_value(value[i + 2]));
          i += 2;
          continue;
        }
      }

      decoded += value[i] == '+' ? ' ' : value[i];
    }

    return decoded;
  }

}  // namespace nvhttp::url_utils
