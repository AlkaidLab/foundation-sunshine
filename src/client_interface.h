/**
 * @file src/client_interface.h
 * @brief Host facts and durable application facts for the specialised client interface.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/property_tree/ptree_fwd.hpp>

namespace client_interface {
  inline constexpr int capability_version = 1;

  enum class fs_kind_e {
    bigscreen,
    desktop,
    game,
    tool,
  };

  struct app_facts_t {
    fs_kind_e kind { fs_kind_e::game };
    bool kind_overridden = false;
    bool is_bigscreen_entry = false;
    bool is_default = false;
    std::string source { "custom" };
    std::int64_t last_played = 0;
    std::int64_t play_seconds = 0;
    std::string art_version;
    std::string hero_path;
    std::string logo_path;
    std::string icon_path;
  };

  struct play_record_t {
    std::int64_t last_played = 0;
    std::int64_t play_seconds = 0;
    std::int64_t started = 0;
  };

  struct host_profile_t {
    std::string platform;
    std::string os_version;
    std::string gpu;
    std::string display_modes;
    std::string host_icon;
  };

  inline std::string
  lower(std::string_view value) {
    std::string result { value };
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return result;
  }

  inline std::string
  trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
  }

  inline std::string_view
  to_string(fs_kind_e kind) {
    switch (kind) {
      case fs_kind_e::bigscreen:
        return "bigscreen";
      case fs_kind_e::desktop:
        return "desktop";
      case fs_kind_e::game:
        return "game";
      case fs_kind_e::tool:
        return "tool";
    }
    return "game";
  }

  inline std::optional<fs_kind_e>
  parse_kind(std::string_view value) {
    const auto normalized = lower(trim(std::string { value }));
    if (normalized == "bigscreen") return fs_kind_e::bigscreen;
    if (normalized == "desktop") return fs_kind_e::desktop;
    if (normalized == "game") return fs_kind_e::game;
    if (normalized == "tool") return fs_kind_e::tool;
    return std::nullopt;
  }

  inline fs_kind_e
  infer_kind(bool opens_big_screen, bool has_command, std::string_view command) {
    if (opens_big_screen) return fs_kind_e::bigscreen;
    if (!has_command) return fs_kind_e::desktop;
    const auto normalized = lower(command);
    // Only what is a tool by its name: the Steam Big Picture and DaVinci Resolve. A game's own "Launcher.exe" or a folder called "Tools" is the game (a person can still say otherwise
    // with fs-kind in apps.json).
    if (normalized.find("bigpicture") != std::string::npos || normalized.find("davinci") != std::string::npos) {
      return fs_kind_e::tool;
    }
    return fs_kind_e::game;
  }

  inline std::string
  infer_source(std::string_view command) {
    const auto normalized = lower(command);
    if (normalized.find("steam") != std::string::npos) return "steam";
    if (normalized.find("xbox") != std::string::npos) return "xbox";
    return "custom";
  }

  /// CRC-32 (the zlib polynomial) of a text, as decimal text: the version tag of a set of art.
  inline std::string
  crc32_text(std::string_view value) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : value) {
      crc ^= static_cast<std::uint8_t>(byte);
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return std::to_string(~crc);
  }

  inline std::string
  make_art_version(std::string_view app_id,
    std::string_view poster_path,
    std::string_view hero_path,
    std::string_view logo_path,
    std::string_view icon_path) {
    std::string value;
    value.reserve(app_id.size() + poster_path.size() + hero_path.size() + logo_path.size() + icon_path.size() + 4);
    value.append(app_id);
    value.push_back('\n');
    value.append(poster_path);
    value.push_back('\n');
    value.append(hero_path);
    value.push_back('\n');
    value.append(logo_path);
    value.push_back('\n');
    value.append(icon_path);
    return crc32_text(value);
  }

  /// The version tag of an application's art as it is now: the configured paths' own tag plus a fact for each present art file
  /// (its size and its modification time), so a file replaced under the same name is a new version. The facts are one
  /// "type:size:mtime" text per present file, joined with ';', in the caller's fixed type order; with no file present the
  /// configured tag stands unchanged.
  inline std::string
  live_art_version(std::string_view configured_version, std::string_view file_facts) {
    if (file_facts.empty()) return std::string { configured_version };
    return crc32_text(std::string { configured_version } + '\n' + std::string { file_facts });
  }

  /// One present art file as "type:size:mtime;" (empty when it cannot be read): the fact `live_art_version` takes. The same facts feed the application list tag, so a picture
  /// replaced under the same name changes both.
  inline std::string
  art_file_fact(std::string_view type, const std::filesystem::path &file) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) return {};
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) return {};
    return std::string { type } + ':' + std::to_string(size) + ':' + std::to_string(written.time_since_epoch().count()) + ';';
  }

  /// The application list tag with the art in it: `apps_art` is "<app id>:<live art version>;" for every application. A client re-reads the list only when the tag changes, so a
  /// picture that changed (a new hero, a replaced cover) must change it; with the art unchanged the tag is unchanged. The tag stays decimal digits; with no application it is the list's own tag.
  inline std::string
  combine_art_etag(std::string_view list_etag, std::string_view apps_art) {
    if (apps_art.empty()) return std::string { list_etag };
    return crc32_text(std::string { list_etag } + '~' + std::string { apps_art });
  }

  namespace detail {
    inline bool
    is_ows(char character) {
      return character == ' ' || character == '\t';
    }

    inline std::string_view
    trim_ows(std::string_view value) {
      while (!value.empty() && is_ows(value.front())) value.remove_prefix(1);
      while (!value.empty() && is_ows(value.back())) value.remove_suffix(1);
      return value;
    }

    inline bool
    read_digits(std::string_view &value, int count, int &out) {
      if (static_cast<int>(value.size()) < count) return false;
      out = 0;
      for (int index = 0; index < count; ++index) {
        const auto character = value[index];
        if (character < '0' || character > '9') return false;
        out = out * 10 + (character - '0');
      }
      value.remove_prefix(count);
      return true;
    }

    inline int
    month_from_name(std::string_view name) {
      static constexpr std::string_view months[] { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
      for (int index = 0; index < 12; ++index) {
        if (name == months[index]) return index + 1;
      }
      return 0;
    }

    /// The length of each month, February made right by the leap-year rule of the calendar HTTP-dates count in.
    inline int
    days_in_month(int year, int month) {
      static constexpr int lengths[] { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
      if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
      return lengths[month - 1];
    }

    /// Days since 1970-01-01 of a date in the proleptic Gregorian calendar: a four-century era of 146097 days, a leap day
    /// in every fourth year except the whole centuries, counted from March so the leap day lands at a era's end. Written
    /// from the calendar rules themselves so a date far before the epoch is the same number on every platform.
    inline std::int64_t
    days_from_civil(int year, int month, int day) {
      const std::int64_t y = month <= 2 ? year - 1 : year;
      const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
      const std::int64_t year_of_era = y - era * 400;
      const std::int64_t month_index = month + (month > 2 ? -3 : 9);
      const std::int64_t day_of_year = (153 * month_index + 2) / 5 + day - 1;
      const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
      return era * 146097 + day_of_era - 719468;
    }

    /// Seconds since the epoch of a UTC moment in the proleptic Gregorian calendar; nothing when the moment is not a date.
    inline std::optional<std::int64_t>
    seconds_from_utc(int year, int month, int day, int hour, int minute, int second) {
      if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return std::nullopt;
      // a leap second is not a second of the clock: the second before stands for it
      return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + (second == 60 ? 59 : second);
    }

    /// "08:49:37" of an HTTP-date: three two-digit numbers separated by colons.
    inline bool
    read_time_of_day(std::string_view &value, int &hour, int &minute, int &second) {
      if (!read_digits(value, 2, hour) || value.substr(0, 1) != ":") return false;
      value.remove_prefix(1);
      if (!read_digits(value, 2, minute) || value.substr(0, 1) != ":") return false;
      value.remove_prefix(1);
      return read_digits(value, 2, second);
    }
  }  // namespace detail

  /// An HTTP-date as UTC seconds since the epoch, or nothing when the value is not one. A recipient that parses a timestamp
  /// in an HTTP field must accept all three HTTP-date forms (fs-rfc9110-cond-20261111, section 5.6.7): the IMF-fixdate and,
  /// from older senders, the obsolete RFC 850 and asctime forms. A value that does not parse makes the condition that
  /// carries it ignored (section 13.1.3).
  inline std::optional<std::int64_t>
  parse_http_date(std::string_view value) {
    value = detail::trim_ows(value);
    const auto comma = value.find(',');
    if (comma != std::string_view::npos) {
      // "Sun, 06 Nov 1994 08:49:37 GMT" and "Sunday, 06-Nov-94 08:49:37 GMT": a day-name, a comma, the date, the time and GMT.
      if (comma == 0 || comma > 10) return std::nullopt;
      for (std::size_t index = 0; index < comma; ++index) {
        const auto character = value[index];
        if ((character < 'a' || character > 'z') && (character < 'A' || character > 'Z')) return std::nullopt;
      }
      value = detail::trim_ows(value.substr(comma + 1));
      int day = 0, year = 0, hour = 0, minute = 0, second = 0;
      if (!detail::read_digits(value, 2, day)) return std::nullopt;
      const bool obsolete = value.substr(0, 1) == "-";
      if (value.substr(0, 1) != (obsolete ? "-" : " ")) return std::nullopt;
      value.remove_prefix(1);
      if (value.size() < 3) return std::nullopt;
      const auto month = detail::month_from_name(value.substr(0, 3));
      if (month == 0) return std::nullopt;
      value.remove_prefix(3);
      if (value.substr(0, 1) != (obsolete ? "-" : " ")) return std::nullopt;
      value.remove_prefix(1);
      if (!detail::read_digits(value, obsolete ? 2 : 4, year)) return std::nullopt;
      if (obsolete) year += year < 50 ? 2000 : 1900;  // the RFC 850 two-digit year (section 5.6.7)
      if (value.substr(0, 1) != " ") return std::nullopt;
      value.remove_prefix(1);
      if (value.substr(8, 4) != " GMT" || value.size() != 12) return std::nullopt;
      auto clock = value.substr(0, 8);
      if (!detail::read_time_of_day(clock, hour, minute, second)) return std::nullopt;
      return detail::seconds_from_utc(year, month, day, hour, minute, second);
    }
    // "Sun Nov  6 08:49:37 1994": no comma; the month comes after the day-name and the day of the month may be one digit
    // padded with a space.
    if (value.size() < 20 || value.substr(3, 1) != " ") return std::nullopt;
    value.remove_prefix(4);
    const auto month = detail::month_from_name(value.substr(0, 3));
    if (month == 0) return std::nullopt;
    value.remove_prefix(3);
    if (value.substr(0, 1) != " ") return std::nullopt;
    value.remove_prefix(1);
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (value.substr(0, 1) == " ") {
      value.remove_prefix(1);
      if (!detail::read_digits(value, 1, day)) return std::nullopt;
    }
    else if (!detail::read_digits(value, 2, day))
      return std::nullopt;
    if (value.substr(0, 1) != " ") return std::nullopt;
    value.remove_prefix(1);
    if (value.size() != 13) return std::nullopt;  // the time, one space and the four-digit year
    auto clock = value.substr(0, 8);
    if (!detail::read_time_of_day(clock, hour, minute, second)) return std::nullopt;
    if (value.substr(8, 1) != " ") return std::nullopt;
    auto year_text = value.substr(9);
    if (!detail::read_digits(year_text, 4, year)) return std::nullopt;
    return detail::seconds_from_utc(year, month, day, hour, minute, second);
  }

  /// The IMF-fixdate form an HTTP-date sender must generate (fs-rfc9110-cond-20261111, section 5.6.7): fixed names, no
  /// locale. gmtime_r/gmtime_s are the thread-safe calendar day of an instant (fs-posix-gmtime-20261111, fs-win-crt-gmtime-s-20261111).
  inline std::string
  format_http_date(std::int64_t seconds) {
    static constexpr const char *days[] { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static constexpr const char *months[] { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const auto moment = static_cast<std::time_t>(seconds);
    std::tm broken {};
#ifdef _WIN32
    gmtime_s(&broken, &moment);
#else
    gmtime_r(&moment, &broken);
#endif
    char text[30];
    std::snprintf(text, sizeof(text), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[broken.tm_wday], broken.tm_mday, months[broken.tm_mon],
      broken.tm_year + 1900, broken.tm_hour, broken.tm_min, broken.tm_sec);
    return text;
  }

  /// Whether one If-None-Match field value selects the representation an entity tag stands for (fs-rfc9110-cond-20261111,
  /// sections 13.1.2 and 8.8.3.2): the value may be a list of entity tags; '*' is true whenever the origin server has a
  /// current representation; tags are compared weakly, so a 'W/' prefix on either side does not hide a match. A value that
  /// is not '*' and lists nothing that matches leaves the representation to be sent.
  inline bool
  etag_matches(std::string_view if_none_match, std::string_view etag) {
    if_none_match = detail::trim_ows(if_none_match);
    if (if_none_match == "*") return true;
    if (etag.substr(0, 2) == "W/") etag.remove_prefix(2);
    // A comma cannot split our own tag (decimal digits, a dash and a type name), so walking the comma-separated entries
    // is enough to find it; an entry that is not our tag, in any state of quoting or weakness, simply does not match.
    for (std::size_t entry_begin = 0; entry_begin <= if_none_match.size();) {
      auto entry = if_none_match.substr(entry_begin, if_none_match.find(',', entry_begin) - entry_begin);
      entry = detail::trim_ows(entry);
      if (entry.substr(0, 2) == "W/") entry.remove_prefix(2);
      if (entry == etag) return true;
      const auto next_comma = if_none_match.find(',', entry_begin);
      if (next_comma == std::string_view::npos) break;
      entry_begin = next_comma + 1;
    }
    return false;
  }

  /// The conditional request result for a served file (fs-rfc9110-cond-20261111, sections 13.2.2, 13.1.2 and 13.1.3):
  /// If-None-Match decides when the field is present and If-Modified-Since is ignored then; without it, a valid
  /// If-Modified-Since at or after the file's time means not modified. last_modified < 0 stands for no modification date
  /// available, and the field is ignored then.
  inline bool
  conditional_not_modified(bool has_if_none_match, std::string_view if_none_match, std::string_view if_modified_since, std::string_view etag, std::int64_t last_modified) {
    if (has_if_none_match) return etag_matches(if_none_match, etag);
    if (last_modified < 0) return false;
    const auto since = parse_http_date(if_modified_since);
    return since.has_value() && last_modified <= *since;
  }

  /// The application list tag with the play history in it: the history (last played, time played) is part of what the list says, so a session that ends changes the tag and a client that
  /// caches the list by it asks again. The tag stays decimal digits (a client may read it as a number); with no history file it is the list's own tag, as before.
  inline std::string
  combine_list_etag(std::string_view list_etag, std::string_view play_tag) {
    if (play_tag.empty()) return std::string { list_etag };
    return crc32_text(std::string { list_etag } + ':' + std::string { play_tag });
  }

  /// The extensions a hero, a logo or an icon may have. The configured path comes from apps.json, and the client asks for it by name: anything that is not an image is not served.
  inline bool
  is_art_extension(std::string_view extension) {
    const auto normalized = lower(extension);
    return normalized == ".png" || normalized == ".jpg" || normalized == ".jpeg" || normalized == ".webp" || normalized == ".bmp" || normalized == ".ico" || normalized == ".gif" ||
           normalized == ".svg";
  }

  /// How many seconds a session counts for. A start that was recorded gives the span to `now`, unless it is older than a week (the host went down during that session and never
  /// recorded the end: the span is not play time); without a start the fallback (the host's own clock of the launch) counts, at most a day, never negative.
  inline std::int64_t
  measured_session_seconds(std::int64_t started, std::int64_t now, std::int64_t fallback_elapsed) {
    constexpr std::int64_t week = 7 * 24 * 3600;
    constexpr std::int64_t day = 24 * 3600;
    if (started > 0 && now >= started && now - started <= week) return now - started;
    return std::clamp<std::int64_t>(fallback_elapsed, 0, day);
  }

  host_profile_t
  host_profile();

  /// Adds the extra elements of serverinfo. The capability version and features are public; the host profile (system, graphics card, display modes) only goes to a paired client,
  /// because serverinfo is also answered over plain HTTP to anyone on the network.
  void
  append_serverinfo(boost::property_tree::ptree &tree, bool paired);

  app_facts_t
  make_app_facts(std::string app_id,
    std::string command,
    bool opens_big_screen,
    std::string poster_path,
    const boost::property_tree::ptree &app_node);

  std::filesystem::path
  play_facts_path();

  std::optional<play_record_t>
  read_play_record(const std::filesystem::path &path, std::string_view app_id);

  /// What the play facts file is now (its size and time), as text; empty when there is no file.
  std::string
  play_facts_tag();

  /// Every record of the play facts file, read once (an application list asks for all of them: one read, not one per application).
  std::unordered_map<std::string, play_record_t>
  read_all_play_records(const std::filesystem::path &path);

  bool
  write_play_record(const std::filesystem::path &path,
    std::string_view app_id,
    std::int64_t now,
    std::int64_t elapsed_seconds);

  bool
  record_session_start(std::string_view app_id, std::int64_t now);

  bool
  record_session_start(std::string_view app_id);

  bool
  record_session_end(std::string_view app_id, std::int64_t now, std::int64_t elapsed_seconds);

  bool
  record_session_end(std::string_view app_id, std::int64_t elapsed_seconds);

  bool
  record_session_end(std::string_view app_id, std::chrono::steady_clock::time_point started);
}  // namespace client_interface
