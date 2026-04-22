/**
 * @file src/platform/windows/clipboard.h
 * @brief Windows clipboard helpers for Sunshine clipboard sync.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platf::clipboard {
  constexpr std::size_t image_size_limit = 4 * 1024 * 1024;

  struct item_t {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> data;
    std::string mime_type;
    std::string name;
    std::uint64_t content_hash = 0;
  };

  bool
  is_backend_available();

  std::uint32_t
  supported_capabilities();

  std::uint32_t
  current_sequence_number();

  bool
  read_current_item(item_t &item, std::string *reason = nullptr);

  bool
  write_item(const item_t &item, std::string *reason = nullptr);
}  // namespace platf::clipboard
