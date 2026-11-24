#pragma once
// The one JSON reading the installer web host needs: the string value of a TOP-LEVEL key of a one-line object.
// Keys of nested objects are skipped: the setup UI (serde_json, keys in alphabetical order) writes
// {"action":{"type":"install"},"type":"action"}, where a plain search for "type" finds the nested one first. That was
// the blocker of 2026-11-09 (every action of the real UI was dropped).
#include <cstdint>
#include <string>

namespace installer_json {

  /** The text of a JSON string whose opening quote is at `at`, as UTF-8: every escape of RFC 8259 decoded (\\, \", \/, \b \f \n \r \t, \uXXXX with surrogate pairs). Stops at the closing quote. */
  inline std::string
  decode_string(const std::string &line, size_t at) {
    std::string out;
    const auto push = [&out](uint32_t cp) {
      if (cp < 0x80) out.push_back(static_cast<char>(cp));
      else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    };
    const auto hex4 = [&line](size_t from, uint32_t *value) {
      if (from + 4 > line.size()) return false;
      uint32_t v = 0;
      for (size_t i = from; i < from + 4; ++i) {
        const char c = line[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return false;
      }
      *value = v;
      return true;
    };
    for (size_t k = at + 1; k < line.size() && line[k] != '"'; ++k) {
      if (line[k] != '\\' || k + 1 >= line.size()) {
        out.push_back(line[k]);
        continue;
      }
      switch (line[++k]) {
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(k + 1, &cp)) {
            out.push_back('u');
            break;
          }
          k += 4;
          uint32_t low = 0;
          if (cp >= 0xD800 && cp < 0xDC00 && k + 6 < line.size() && line[k + 1] == '\\' && line[k + 2] == 'u' && hex4(k + 3, &low) && low >= 0xDC00 && low < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            k += 6;
          }
          // a surrogate that has no partner is not text: a '?' stands in for it, which no folder path accepts, so the rules refuse it (it is never turned into a replacement character)
          push(cp >= 0xD800 && cp < 0xE000 ? static_cast<uint32_t>('?') : cp);
          break;
        }
        default: out.push_back(line[k]);  // \\ \" \/ and anything else: the character itself
      }
    }
    return out;
  }

  /** The decoded string value of `"key":"..."` anywhere in the line (a key of a nested object, like the action's `path`); empty when absent. */
  inline std::string
  string_value(const std::string &line, const std::string &key) {
    const std::string needle = "\"" + key + "\":\"";
    const size_t at = line.find(needle);
    return at == std::string::npos ? std::string() : decode_string(line, at + needle.size() - 1);
  }

  inline std::string
  top_str(const std::string &line, const std::string &key) {
    const std::string needle = "\"" + key + "\"";
    int depth = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '{' || c == '[') {
        ++depth;
        continue;
      }
      if (c == '}' || c == ']') {
        --depth;
        continue;
      }
      if (c != '"') continue;
      const bool candidate = depth == 1 && line.compare(i, needle.size(), needle) == 0;
      // skip this string (a key or a value), escapes included
      size_t end = i + 1;
      while (end < line.size() && line[end] != '"') end += line[end] == '\\' ? 2 : 1;
      if (candidate && end == i + needle.size() - 1) {
        size_t j = end + 1;
        while (j < line.size() && (line[j] == ' ' || line[j] == ':')) ++j;
        if (j >= line.size() || line[j] != '"') return {};
        std::string out;
        for (size_t k = j + 1; k < line.size() && line[k] != '"'; ++k) {
          if (line[k] == '\\' && k + 1 < line.size()) ++k;
          out.push_back(line[k]);
        }
        return out;
      }
      i = end;
    }
    return {};
  }

}  // namespace installer_json
