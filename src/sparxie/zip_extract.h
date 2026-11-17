#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace sparxie::zip {
  namespace fs = std::filesystem;

  struct Outcome {
    bool ok = false;
    std::string error_code;
    std::string error;
    std::uint64_t uncompressed = 0;
    unsigned entries = 0;
  };

  // Central directory only. APPNOTE 6.3.10 4.3.12, 4.3.16, 4.4.17
  // (pkware-appnote-6.3.10-20261110).
  Outcome
  inspect(const fs::path &archive);

  // Extract into dest. Backslashes in names become '/'. Absolute names, drive
  // letters, and '.' or '..' segments are rejected. Methods: store (0) and
  // deflate (8). Deflate is raw inflate (RFC 1951) via puff 2.3
  // (zlib-puff-2.3-20261110). CRC-32 is checked (APPNOTE 4.4.7).
  Outcome
  extract(const fs::path &archive, const fs::path &dest, const std::function<void(int)> &on_percent = {});
}  // namespace sparxie::zip
