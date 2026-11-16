/**
 * @file src/telemetry/error_code.h
 * @brief The code an error line of the log is reported under: a hash of the fixed words the message starts with.
 *
 * The line itself is never sent. Its code is a hash of the words it starts with, up to the first character that starts a value (a digit, '=', a quote, a bracket, a path separator, a colon after the
 * first few words): the fixed text of the message, so that the same error is one code and what a person chose is not in it. Pure code (std only), unit tested on its own.
 *
 * Three rules keep different errors from sharing a code (11-16: one code covered every NvEnc line, one every "Couldn't ..." line, one every line that starts with a bracketed tag):
 *   - an apostrophe inside a word ("Couldn't", "doesn't") is part of the word; only a quote that opens a value (after a space, '=', '(' ...) stops the head;
 *   - a leading "[Tag]" of letters, spaces, '_' '-' '.' is part of the head (the tag is the component of the message);
 *   - a first prefix of at most two words before a colon ("NvEnc: ", "Audio capture: ") is part of the head, the text after it continues up to the next value start.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace telemetry {

  /// 32-bit FNV-1a, the same function the code has used since the first release.
  inline std::uint32_t
  fnv1a_32(std::string_view text) {
    std::uint32_t h = 2166136261u;
    for (const unsigned char c : text) {
      h ^= c;
      h *= 16777619u;
    }
    return h;
  }

  /// The fixed words a message starts with, lower case, single spaces, at most `max_chars` long (cut at a word).
  inline std::string
  error_head(std::string_view text, std::size_t max_chars = 96) {
    std::string head;
    std::size_t i = 0;
    const auto alpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
    // a leading "[Tag] " belongs to the head
    if (!text.empty() && text[0] == '[') {
      const auto close = text.find(']');
      if (close != std::string_view::npos && close <= 41) {
        bool tag = close > 1;
        for (std::size_t k = 1; k < close && tag; ++k) tag = alpha(text[k]) || text[k] == ' ' || text[k] == '_' || text[k] == '-' || text[k] == '.';
        if (tag) {
          for (std::size_t k = 1; k < close; ++k) head.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[k]))));
          head.push_back(' ');
          i = close + 1;
        }
      }
    }
    bool prefix_colon_used = false;
    for (; i < text.size(); ++i) {
      const char c = text[i];
      const unsigned char u = static_cast<unsigned char>(c);
      if (std::isdigit(u) || c == '=' || c == '"' || c == '[' || c == '(' || c == '{' || c == '<' || c == '/' || c == '\\') break;
      if (c == '\'') {
        // inside a word ("couldn't") it is part of the word; anywhere else it opens a value
        const bool inside = !head.empty() && alpha(head.back()) && i + 1 < text.size() && alpha(text[i + 1]);
        if (!inside) break;
        head.push_back(c);
        continue;
      }
      if (c == ':') {
        // "NvEnc: ..." / "Audio capture: ...": a short first prefix is a component name, the message goes on after it
        const auto words = std::count(head.begin(), head.end(), ' ') + (head.empty() ? 0 : 1);
        const bool short_prefix = !prefix_colon_used && words <= 2 && head.size() <= 24 && i + 1 < text.size() && text[i + 1] == ' ';
        if (!short_prefix) break;
        prefix_colon_used = true;
        if (!head.empty() && head.back() != ' ') head.push_back(' ');
        continue;
      }
      if (c == ' ' && (head.empty() || head.back() == ' ')) continue;
      head.push_back(static_cast<char>(std::tolower(u)));
    }
    while (!head.empty() && head.back() == ' ') head.pop_back();
    if (head.size() > max_chars) {
      head.resize(max_chars);
      const auto space = head.rfind(' ');
      if (space != std::string::npos && space > 0) head.resize(space);
    }
    return head;
  }

  /// "e" and eight hex digits.
  inline std::string
  error_code_of(std::string_view scrubbed_message) {
    char code[16];
    std::snprintf(code, sizeof(code), "e%08x", fnv1a_32(error_head(scrubbed_message)));
    return code;
  }

}  // namespace telemetry
