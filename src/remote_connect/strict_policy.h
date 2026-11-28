#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace remote_connect::strict_policy {

  // Lowercase hex for a 32-byte X25519 public key, the form the EasyTier
  // strict-data table uses.
  std::string
  key_hex(const std::array<std::uint8_t, 32> &key);

  // Renders the [experimental_strict_data] TOML table with one wildcard
  // entry per key. An empty key list renders an empty string (no table),
  // which keeps hosts without enrolled devices, and unpatched cores,
  // byte-identical to the configuration they had before.
  //
  // Returns nullopt if any entry is not 64 lowercase hex characters: a
  // malformed value would change what the table says, so the caller must
  // refuse to start rather than write a configuration built from it.
  std::optional<std::string>
  render_table(const std::vector<std::string> &keys);

}  // namespace remote_connect::strict_policy
