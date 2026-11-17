#pragma once
// The one JSON reading the installer web host needs: the string value of a TOP-LEVEL key of a one-line object.
// Keys of nested objects are skipped: the setup UI (serde_json, keys in alphabetical order) writes
// {"action":{"type":"install"},"type":"action"}, where a plain search for "type" finds the nested one first. That was
// the blocker of 2026-11-09 (every action of the real UI was dropped).
#include <string>

namespace installer_json {

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
