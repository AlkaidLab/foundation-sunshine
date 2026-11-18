#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>

namespace sparxie {
  // Return no body on a read error, including after a partial response.
  template<class Query, class Read>
  std::optional<std::string> read_http_body(std::size_t limit, Query &&query, Read &&read) {
    std::string body;
    try {
      while (true) {
        std::uint32_t available = 0;
        if (!query(available)) {
          return std::nullopt;
        }
        if (available == 0) {
          return body;
        }
        if (available > limit - body.size()) {
          return std::nullopt;
        }
        const std::size_t offset = body.size();
        body.resize(offset + available);
        std::uint32_t received = 0;
        if (!read(body.data() + offset, available, received) || received == 0 || received > available) {
          return std::nullopt;
        }
        body.resize(offset + received);
      }
    }
    catch (const std::bad_alloc &) {
      return std::nullopt;
    }
  }
}  // namespace sparxie
