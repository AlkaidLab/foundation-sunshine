/**
 * @file src/logging_site.h
 * @brief Source site (file:line:function) of error and fatal log lines. Boost.Log and std only.
 *
 * `BOOST_LOG(error) << "..."` and `BOOST_LOG(fatal) << "..."` add the site of the statement to the record; the log formatter prints it at the end of the line as
 * `[src/stream.cpp:3912:videoBroadcastThread]`. Cost: the file is cut at compile time (consteval) and the function is `__func__`, so a site is two pointers and an int that
 * live in the program image. Lines below `error` take the unchanged path: one integer compare, no attribute, no allocation.
 *
 * Boost.Log: attribute values added with `boost::log::add_value` belong to the one record being built and only exist while a record is open, so nothing is stored for a
 * line the filter drops (https://www.boost.org/doc/libs/release/libs/log/doc/html/boost/log/add_value.html).
 */
#pragma once

#include <string>
#include <string_view>

#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/attributes/value_visitation.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>

namespace logging {

  /** The name of the record attribute that carries a source site. */
  inline constexpr const char *SITE_ATTRIBUTE = "Site";
  /** Severities from this one up carry a site (the numbers of logging_severity.h: 4 = error, 5 = fatal). */
  inline constexpr int SITE_MIN_SEVERITY = 4;

  struct source_site_t {
    const char *file;  ///< relative to the source tree: src/..., src_assets/..., tools/..., tests/...; the bare file name when the path has none of them
    int line;
    const char *function;  ///< the unqualified function name (__func__)
  };

  namespace detail {
    /** `text` begins with the directory name `name` followed by a path separator. */
    constexpr bool
    starts_with_directory(const char *text, std::string_view name) {
      for (std::size_t i = 0; i < name.size(); ++i)
        if (text[i] != name[i]) return false;
      return text[name.size()] == '/' || text[name.size()] == '\\';
    }
  }  // namespace detail

  /**
   * The part of a path from its first `src/`, `src_assets/`, `tools/` or `tests/` directory on (same rule as telemetry::location_of), so no build or home directory leaks.
   * Where there is none, the file name alone. Evaluated by the compiler: the result points into the string literal that was passed in.
   */
  consteval const char *
  trim_source_path(const char *path) {
    constexpr std::string_view roots[] = { "src", "src_assets", "tools", "tests" };
    const char *last_name = path;
    for (const char *p = path; *p; ++p) {
      if (*p == '/' || *p == '\\') last_name = p + 1;
      if (p != path && p[-1] != '/' && p[-1] != '\\') continue;  // a root is a whole directory name
      for (const auto root : roots)
        if (detail::starts_with_directory(p, root)) return p;
    }
    return last_name;
  }

  /** Streamed after the message head by BOOST_LOG: adds the site to records of severity error and fatal, and nothing to the others. */
  struct site_tag_t {
    source_site_t site;
    int severity;

    template <typename CharT>
    friend boost::log::basic_record_ostream<CharT> &
    operator<<(boost::log::basic_record_ostream<CharT> &stream, const site_tag_t &tag) {
      if (tag.severity >= SITE_MIN_SEVERITY) stream << boost::log::add_value(SITE_ATTRIBUTE, tag.site);
      return stream;
    }
  };

  template <typename Logger>
  inline site_tag_t
  site_of(const Logger &logger, const char *file, int line, const char *function) {
    if constexpr (requires { logger.default_severity(); })
      return { { file, line, function }, static_cast<int>(logger.default_severity()) };
    else
      return { { file, line, function }, 0 };
  }

  /** `src/stream.cpp:3912:videoBroadcastThread`, or empty when the record has no site. */
  inline std::string
  site_text(const boost::log::record_view &view) {
    const auto value = view.attribute_values()[SITE_ATTRIBUTE].extract<source_site_t>();
    if (!value) return {};
    const auto &s = value.get();
    return std::string { s.file ? s.file : "" } + ':' + std::to_string(s.line) + ':' + (s.function ? s.function : "");
  }

  /** The end of a log line: ` [src/stream.cpp:3912:videoBroadcastThread]` for a record that has a site, nothing for the others. */
  inline void
  append_site(boost::log::formatting_ostream &os, const boost::log::record_view &view) {
    if (const auto site = site_text(view); !site.empty()) os << " [" << site << ']';
  }

}  // namespace logging

#undef BOOST_LOG
/**
 * Replaces Boost's BOOST_LOG so that error and fatal records carry their source site (see above). `__FILE__` is cut by the compiler, `__func__` is a pointer.
 * The expression is still a stream, so `BOOST_LOG(error) << a << b;` and its use as the body of an unbraced if/else behave as before.
 */
#define BOOST_LOG(logger) \
  BOOST_LOG_STREAM(logger) << ::logging::site_of((logger), ::logging::trim_source_path(__FILE__), __LINE__, __func__)
