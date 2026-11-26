#pragma once

#include <string>
#include <vector>

namespace remote_connect::strict_policy {

  // Parsed view of the persisted strict-data client key list: 64-hex X25519
  // public keys, semicolon separated. These keys authorize the strict-data
  // v1 path; they are public material but must still come from a trusted
  // enrollment decision, never from an untrusted exchange.
  struct key_list_t {
    bool valid;
    std::vector<std::string> keys;  // normalized lowercase hex
    std::string error;              // human-readable, empty when valid
  };

  // Parses the raw configuration string. Empty or whitespace-only input is
  // valid with no keys. Invalid entries make the whole list invalid so a
  // misconfigured policy fails loudly instead of silently narrowing.
  key_list_t
  parse_client_keys(const std::string &raw);

  // Renders the [experimental_strict_data] TOML table with one wildcard
  // entry per key. An empty key list renders an empty string (no table),
  // which keeps unpatched cores and unconfigured hosts byte-identical to
  // the previous behavior.
  std::string
  render_table(const std::vector<std::string> &keys);

}  // namespace remote_connect::strict_policy
