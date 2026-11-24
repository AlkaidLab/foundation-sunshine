/**
 * @file src/file_mapping/file_mapping_base64.h
 * @brief Base64 encoding shared by file-mapping transports and persistence.
 */
#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace file_mapping::base64 {
  inline std::string
  encode(const unsigned char *data, std::size_t size) {
    if (size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }

    const auto encoded_size = 4 * ((size + 2) / 3);
    if (encoded_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }

    std::string result(encoded_size, '\0');
    const auto length = EVP_EncodeBlock(
      reinterpret_cast<unsigned char *>(result.data()), data, static_cast<int>(size));
    if (length < 0) {
      return {};
    }
    result.resize(static_cast<std::size_t>(length));
    return result;
  }

  inline std::string
  encode(std::string_view value) {
    return encode(reinterpret_cast<const unsigned char *>(value.data()), value.size());
  }
}  // namespace file_mapping::base64
