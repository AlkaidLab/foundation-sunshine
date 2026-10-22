/**
 * @file src/platform/linux/clipboard_mime.h
 * @brief Which flavour of an offered desktop selection the clipboard watcher asks for.
 *
 * A data-control offer lists every representation of one clipboard item; the watcher
 * has to pick exactly one and ask the compositor for it. The wire contract only
 * carries text and PNG, so the choice is: PNG when offered, otherwise the most
 * specific UTF-8 text flavour. Anything else (image/svg, application/*, X11's
 * COMPOUND_TEXT) has no wire representation and is skipped rather than requested
 * and dropped.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace platf::clipboard_mime {
  enum class kind_e {
    none,
    text,
    image,
  };

  inline constexpr std::string_view png_mime = "image/png";
  inline constexpr std::string_view utf8_string_mime = "UTF8_STRING";

  namespace detail {
    inline char
    lower(char ch) {
      return (char) std::tolower((unsigned char) ch);
    }

    /// "<type>/<subtype>", lowercased, spaces dropped, parameters discarded.
    inline std::string
    type_of(std::string_view mime) {
      std::string out;
      out.reserve(mime.size());
      for (const auto ch: mime) {
        if (ch == ';') {
          break;
        }
        if (ch == ' ' || ch == '\t') {
          continue;
        }
        out.push_back(lower(ch));
      }
      return out;
    }

    /// Value of the `charset` parameter, lowercased and unquoted; empty when absent.
    inline std::string
    charset_of(std::string_view mime) {
      const auto first = mime.find(';');
      if (first == std::string_view::npos) {
        return {};
      }

      auto pos = first;
      while (pos != std::string_view::npos) {
        const auto next = mime.find(';', pos + 1);
        auto param = mime.substr(pos + 1, next == std::string_view::npos ? std::string_view::npos : next - pos - 1);

        std::string normalized;
        normalized.reserve(param.size());
        for (const auto ch: param) {
          if (ch != ' ' && ch != '\t') {
            normalized.push_back(lower(ch));
          }
        }
        if (normalized.rfind("charset=", 0) == 0) {
          auto value = normalized.substr(8);
          if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
          }
          return value;
        }
        pos = next;
      }
      return {};
    }

    inline bool
    is_utf8(std::string_view charset) {
      return charset == "utf-8" || charset == "utf8";
    }

    /// Lower is better; -1 means "not a text flavour we can carry faithfully".
    inline int
    text_rank(std::string_view mime) {
      if (mime == utf8_string_mime) {
        // X11's own UTF-8 atom: no parameters, and exactly what GTK/Qt publish.
        return 1;
      }

      const auto type = type_of(mime);
      const auto charset = charset_of(mime);
      // Anything announced with a non-UTF-8 charset would reach clients as
      // mojibake, so it is refused rather than mangled.
      if (!charset.empty() && !is_utf8(charset)) {
        return -1;
      }

      if (type == "text/plain") {
        return charset.empty() ? 2 : 0;
      }
      if (type.rfind("text/", 0) == 0) {
        return 3;
      }
      return -1;
    }
  }  // namespace detail

  inline bool
  is_text_mime(std::string_view mime) {
    return detail::text_rank(mime) >= 0;
  }

  struct choice_t {
    kind_e kind = kind_e::none;
    /// The mime to pass to `receive()`; empty when `kind == none`.
    std::string mime;
  };

  /**
   * @brief Pick the representation to fetch from an offer's mime list.
   * @details PNG wins outright: it is the lossless flavour the wire carries, and
   *          image offers usually list a text fallback (a file name) beside it.
   */
  inline choice_t
  choose(const std::vector<std::string> &offered) {
    for (const auto &mime: offered) {
      if (detail::type_of(mime) == png_mime) {
        return { kind_e::image, std::string { png_mime } };
      }
    }

    int best_rank = -1;
    std::string best_mime;
    for (const auto &mime: offered) {
      const auto rank = detail::text_rank(mime);
      if (rank < 0 || (best_rank >= 0 && rank >= best_rank)) {
        continue;
      }
      best_rank = rank;
      best_mime = mime;
    }

    if (best_rank < 0) {
      return {};
    }
    return { kind_e::text, std::move(best_mime) };
  }

  /// Mimes to advertise when publishing text, most specific first.
  inline std::vector<std::string>
  text_offers() {
    return { "text/plain;charset=utf-8", std::string { utf8_string_mime }, "text/plain" };
  }
}  // namespace platf::clipboard_mime
