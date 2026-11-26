#include "strict_policy.h"

namespace remote_connect::strict_policy {
  namespace {
    // An invalid result never carries keys: a caller that ignores `valid`
    // must not be handed a truncated policy that still looks usable.
    void
    fail(key_list_t &result, const std::string &error) {
      result.valid = false;
      result.error = error;
      result.keys.clear();
    }

    constexpr std::size_t KEY_HEX_LENGTH = 64;

    bool
    is_hex(char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    char
    to_lower_hex(char ch) {
      if (ch >= 'A' && ch <= 'F') return static_cast<char>(ch - 'A' + 'a');
      return ch;
    }

    std::string
    trim(const std::string &value) {
      const auto begin = value.find_first_not_of(" \t\r\n");
      if (begin == std::string::npos) return {};
      const auto end = value.find_last_not_of(" \t\r\n");
      return value.substr(begin, end - begin + 1);
    }
  }  // namespace

  key_list_t
  parse_client_keys(const std::string &raw) {
    key_list_t result { true, {}, {} };
    std::size_t segment_index = 0;
    std::size_t start = 0;
    while (start <= raw.size()) {
      const auto separator = raw.find(';', start);
      const auto segment = trim(raw.substr(
        start,
        separator == std::string::npos ? raw.size() - start : separator - start
      ));
      ++segment_index;
      if (!segment.empty()) {
        if (segment.size() != KEY_HEX_LENGTH) {
          fail(
            result,
            "strict-data client key " + std::to_string(segment_index) + " must be 64 hex characters"
          );
          return result;
        }
        std::string normalized;
        normalized.reserve(segment.size());
        for (const char ch : segment) {
          if (!is_hex(ch)) {
            fail(
              result,
              "strict-data client key " + std::to_string(segment_index) +
                " contains a non-hex character"
            );
            return result;
          }
          normalized.push_back(to_lower_hex(ch));
        }
        // The core refuses an all-zero key and fails instance creation; keep
        // such a value out of the persisted list so a start can never be
        // broken by it.
        if (normalized.find_first_not_of('0') == std::string::npos) {
          fail(
            result,
            "strict-data client key " + std::to_string(segment_index) + " must not be all zero"
          );
          return result;
        }
        for (const auto &existing : result.keys) {
          if (existing == normalized) {
            fail(
              result,
              "strict-data client key " + std::to_string(segment_index) +
                " duplicates an earlier entry"
            );
            return result;
          }
        }
        result.keys.push_back(std::move(normalized));
      }
      if (separator == std::string::npos) break;
      start = separator + 1;
    }
    return result;
  }

  std::string
  render_table(const std::vector<std::string> &keys) {
    if (keys.empty()) return {};
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
