#pragma once

#include <string>

namespace nvhttp::url_utils {

  std::string
  encode(const std::string &value);

  std::string
  decode(std::string value);

}  // namespace nvhttp::url_utils
