#include "url_utils.h"

#include <sstream>

namespace nvhttp::url_utils {

  std::string
  decode(std::string value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '%' && i + 2 < value.size()) {
        int hex_val;
        std::istringstream hex_stream(value.substr(i + 1, 2));
        if (hex_stream >> std::hex >> hex_val) {
          decoded += static_cast<char>(hex_val);
          i += 2;
          continue;
        }
      }

      decoded += value[i] == '+' ? ' ' : value[i];
    }

    return decoded;
  }

}  // namespace nvhttp::url_utils
