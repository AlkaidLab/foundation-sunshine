/**
 * @file src/version_compare.h
 * @brief Compare Foundation Sunshine versions by their date part only.
 *
 * A version is `YYYY.MDD.HMMSS`, written either unpadded (CI tag `v2026.1103.21122`) or zero-padded (local build `2026.1112.025657`). After the
 * third part anything may follow: `.杂鱼` (every CI release), `.<short commit>` (a build off another branch), emoji, any other text. None of it
 * takes part in a comparison. A manual CI build is tagged `manual-YYYYMMDD-HHMMSS[-dev]`; it compares by that embedded date, and a string with no
 * date at all is "unknown", which is never newer, older or equal.
 *
 * Whether a build is dirty is not decided here: only the `dirty` field of GET /api/version says that.
 *
 * The same rules, in the same order, are in the panel (version_key in update/metadata.rs) and the Web UI (sunshine_version.js); the three share the
 * test vectors in tests/fixtures/version_vectors.json.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace version_compare {
  using key_t = std::array<std::uint64_t, 3>;

  namespace detail {
    template <class Ch>
    constexpr bool
    is_digit(Ch c) {
      return c >= static_cast<Ch>(L'0') && c <= static_cast<Ch>(L'9');
    }

    /** Read a digit run of 1..9 digits at `pos`; false when there is none or it is longer. */
    template <class Ch>
    constexpr bool
    read_number(std::basic_string_view<Ch> text, std::size_t &pos, std::size_t want_digits, std::uint64_t &value) {
      std::size_t n = 0;
      std::uint64_t v = 0;
      while (pos + n < text.size() && is_digit(text[pos + n])) {
        if (n >= 9) return false;
        v = v * 10 + static_cast<std::uint64_t>(text[pos + n] - static_cast<Ch>(L'0'));
        ++n;
      }
      if (n == 0 || (want_digits && n != want_digits)) return false;
      pos += n;
      value = v;
      return true;
    }

    template <class Ch>
    constexpr bool
    starts_with_ascii_nocase(std::basic_string_view<Ch> text, std::string_view prefix) {
      if (text.size() < prefix.size()) return false;
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        auto c = static_cast<char32_t>(text[i]);
        if (c >= U'A' && c <= U'Z') c += U'a' - U'A';
        if (c != static_cast<char32_t>(static_cast<unsigned char>(prefix[i]))) return false;
      }
      return true;
    }
  }  // namespace detail

  /** The first three parts of `text`, or nothing when it carries no date-style version. */
  template <class Ch>
  constexpr std::optional<key_t>
  parse(std::basic_string_view<Ch> text) {
    using namespace detail;
    while (!text.empty() && (text.front() == static_cast<Ch>(L' ') || text.front() == static_cast<Ch>(L'\t') || text.front() == static_cast<Ch>(L'\n') || text.front() == static_cast<Ch>(L'\r'))) text.remove_prefix(1);
    if (!text.empty() && (text.front() == static_cast<Ch>(L'v') || text.front() == static_cast<Ch>(L'V'))) text.remove_prefix(1);

    std::size_t pos = 0;
    if (starts_with_ascii_nocase(text, "manual-")) {
      // manual-YYYYMMDD-HHMMSS[-dev]: the date is the version
      pos = 7;
      std::uint64_t date = 0, time = 0;
      if (!read_number(text, pos, 8, date)) return std::nullopt;
      if (pos >= text.size() || text[pos] != static_cast<Ch>(L'-')) return std::nullopt;
      ++pos;
      if (!read_number(text, pos, 6, time)) return std::nullopt;
      const auto month = date / 100 % 100, day = date % 100;
      if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
      return key_t { date / 10000, date % 10000, time };
    }

    key_t key {};
    for (std::size_t i = 0; i < 3; ++i) {
      if (!read_number(text, pos, 0, key[i])) return std::nullopt;
      if (i < 2) {
        if (pos >= text.size() || text[pos] != static_cast<Ch>(L'.')) return std::nullopt;
        ++pos;
      }
    }
    return key;
  }

  /** -1, 0, 1 as `left` is older than, the same as, newer than `right`; nothing when either side has no date version. */
  template <class Ch>
  constexpr std::optional<int>
  compare(std::basic_string_view<Ch> left, std::basic_string_view<Ch> right) {
    const auto a = parse(left), b = parse(right);
    if (!a || !b) return std::nullopt;
    if (*a == *b) return 0;
    return *a < *b ? -1 : 1;
  }

  // string literal / std::string convenience (char and wchar_t)
  inline std::optional<key_t>
  parse(std::string_view text) {
    return parse<char>(text);
  }
  inline std::optional<key_t>
  parse(std::wstring_view text) {
    return parse<wchar_t>(text);
  }
  inline std::optional<int>
  compare(std::string_view left, std::string_view right) {
    return compare<char>(left, right);
  }
  inline std::optional<int>
  compare(std::wstring_view left, std::wstring_view right) {
    return compare<wchar_t>(left, right);
  }
}  // namespace version_compare
