#include "strict_policy.h"

#include <algorithm>

namespace remote_connect::strict_policy {
  namespace {
    constexpr std::size_t KEY_HEX_LENGTH = 64;

    bool
    is_lowercase_hex(const std::string &key) {
      return key.size() == KEY_HEX_LENGTH &&
             std::all_of(key.begin(), key.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
             });
    }
  }  // namespace

  std::string
  key_hex(const std::array<std::uint8_t, 32> &key) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (const auto byte : key) {
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0f]);
    }
    return out;
  }

  std::optional<std::string>
  render_table(const std::vector<std::string> &keys) {
    for (const auto &key : keys) {
      if (!is_lowercase_hex(key)) return std::nullopt;
    }
    if (keys.empty()) return std::string {};
    std::string table = "[experimental_strict_data]\n"
                        "advertise_v1 = true\n"
                        "require_v1 = true\n"
                        "expected_peer_keys = [";
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (index != 0) table += ", ";
      table += "\"0:" + keys[index] + "\"";
    }
    table += "]\n";
    return table;
  }

}  // namespace remote_connect::strict_policy
