/**
 * @file src/settings_reset.cpp
 * @brief Restoring the host settings to their defaults, and taking that back (see settings_reset.h).
 */
#include "src/settings_reset.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace settings_reset {

  namespace {
    constexpr std::string_view infix = ".reset-";
    constexpr std::size_t stamp_size = 15;  // 20261119-153012

    // days since 1970-01-01 <-> civil date (the proleptic Gregorian calendar; Howard Hinnant's algorithms)
    std::int64_t
    days_from_civil(std::int64_t y, unsigned m, unsigned d) {
      y -= m <= 2;
      const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
      const auto yoe = static_cast<unsigned>(y - era * 400);
      const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
      const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    }

    void
    civil_from_days(std::int64_t z, std::int64_t &y, unsigned &m, unsigned &d) {
      z += 719468;
      const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const auto doe = static_cast<unsigned>(z - era * 146097);
      const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      y = static_cast<std::int64_t>(yoe) + era * 400;
      const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const auto mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp + (mp < 10 ? 3 : -9);
      y += m <= 2;
    }

    std::int64_t
    floor_div(std::int64_t a, std::int64_t b) {
      auto q = a / b;
      if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
      return q;
    }

    fs::path
    copy_of(const fs::path &base, std::string_view stamp) {
      auto copy = base;
      copy += std::string { infix };
      copy += std::string { stamp };
      return copy;
    }

    fs::path
    folder_of(const fs::path &file) {
      return file.has_parent_path() ? file.parent_path() : fs::path { "." };
    }

    /// The whole file, or nothing: a path that is not a regular file, or a read that came back shorter than the file, is not an empty file.
    std::optional<std::string>
    read_all(const fs::path &path) {
      std::error_code probe;
      if (!fs::is_regular_file(path, probe) || probe) return std::nullopt;
      const auto size = fs::file_size(path, probe);
      if (probe) return std::nullopt;
      std::ifstream input(path, std::ios::binary);
      if (!input.is_open()) return std::nullopt;
      std::string contents { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
      if (input.bad() || contents.size() != size) return std::nullopt;
      return contents;
    }

    /// The file as it is replaced: written next to it, then moved over it, so a reader sees the old file or the new one.
    bool
    write_atomic(const fs::path &path, std::string_view contents) {
      auto temp = path;
      temp += ".tmp-reset";
      {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) {
          out.close();
          std::error_code ignored;
          fs::remove(temp, ignored);
          return false;
        }
      }
      std::error_code error;
      fs::rename(temp, path, error);
      if (error) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
      }
      return true;
    }

    bool
    copy_checked(const fs::path &from, const fs::path &to) {
      std::error_code error;
      if (!fs::copy_file(from, to, fs::copy_options::none, error) || error) {
        std::error_code ignored;
        fs::remove(to, ignored);
        return false;
      }
      const auto a = fs::file_size(from, error);
      const auto b = error ? 0 : fs::file_size(to, error);
      if (error || a != b) {
        std::error_code ignored;
        fs::remove(to, ignored);
        return false;
      }
      return true;
    }

    void
    remove_quietly(const fs::path &path) {
      std::error_code ignored;
      fs::remove(path, ignored);
    }

    std::string
    unique_stamp(const paths_t &paths, std::int64_t now_ms) {
      const auto base = format_stamp(now_ms);
      auto stamp = base;
      std::error_code error;
      for (int n = 2; fs::exists(copy_of(paths.config, stamp), error) || fs::exists(copy_of(paths.apps, stamp), error); ++n) {
        stamp = base + "-" + std::to_string(n);
      }
      return stamp;
    }

    // `20261119-153012` or `20261119-153012-3`: the sort key is the time, then the number
    std::optional<std::pair<std::string, int>>
    split_stamp(std::string_view stamp) {
      if (stamp.size() < stamp_size) return std::nullopt;
      for (std::size_t i = 0; i < stamp_size; ++i) {
        const auto c = stamp[i];
        if (i == 8 ? c != '-' : (c < '0' || c > '9')) return std::nullopt;
      }
      int n = 1;
      if (stamp.size() > stamp_size) {
        if (stamp[stamp_size] != '-' || stamp.size() == stamp_size + 1 || stamp.size() > stamp_size + 6) return std::nullopt;
        n = 0;
        for (auto c : stamp.substr(stamp_size + 1)) {
          if (c < '0' || c > '9') return std::nullopt;
          n = n * 10 + (c - '0');
        }
      }
      return std::pair { std::string { stamp.substr(0, stamp_size) }, n };
    }

    void
    rotate(const paths_t &paths) {
      const auto backups = list_backups(paths);
      for (std::size_t i = kept_copies; i < backups.size(); ++i) {
        remove_quietly(copy_of(paths.config, backups[i].stamp));
        remove_quietly(copy_of(paths.apps, backups[i].stamp));
      }
    }

    std::string
    serialize(const std::map<std::string, std::string> &settings) {
      std::string out;
      for (const auto &[key, value] : settings) {
        if (value.empty() || value == "null") continue;
        out.append(key).append(" = ").append(value).push_back('\n');
      }
      return out;
    }

    std::map<std::string, std::string>
    only_kept(const std::map<std::string, std::string> &settings) {
      std::map<std::string, std::string> kept;
      for (const auto key : kept_keys()) {
        const auto found = settings.find(std::string { key });
        if (found != settings.end() && !found->second.empty() && found->second != "null") kept.emplace(*found);
      }
      return kept;
    }

    struct current_t {
      bool existed = false;
      std::string text;
    };

    std::optional<current_t>
    read_current(const fs::path &config) {
      std::error_code error;
      const auto exists = fs::exists(config, error);
      if (error) return std::nullopt;
      if (!exists) return current_t {};
      auto text = read_all(config);
      if (!text) return std::nullopt;
      return current_t { true, std::move(*text) };
    }
  }  // namespace

  const std::vector<std::string_view> &
  kept_keys() {
    static const std::vector<std::string_view> keys {
      "session_binding",
      "vdd_keep_enabled",
      "vdd_headless_create",
      "tray_locale",
      "widget_token",
      "clients",
      "locale",
      "port",
      "telemetry_consent",
      "telemetry_errors",
      "file_apps",
      "file_state",
      "credentials_file",
      "pkey",
      "cert",
      "log_path",
    };
    return keys;
  }

  std::string_view
  error_code(error_e error) {
    switch (error) {
      case error_e::none:
        return "ok";
      case error_e::busy:
        return "busy";
      case error_e::no_backup:
        return "no_backup";
      case error_e::read_failed:
        return "read_failed";
      case error_e::backup_failed:
        return "backup_failed";
      case error_e::write_failed:
        return "write_failed";
      case error_e::defaults_missing:
        return "defaults_missing";
    }
    return "unknown";
  }

  std::string
  format_stamp(std::int64_t unix_ms) {
    const auto seconds = floor_div(unix_ms, 1000);
    const auto days = floor_div(seconds, 86400);
    const auto of_day = static_cast<int>(seconds - days * 86400);
    std::int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, year, month, day);
    char text[32];
    std::snprintf(text, sizeof(text), "%04d%02u%02u-%02d%02d%02d", static_cast<int>(year), month, day, of_day / 3600, of_day / 60 % 60, of_day % 60);
    return text;
  }

  std::int64_t
  parse_stamp(std::string_view stamp) {
    const auto split = split_stamp(stamp);
    if (!split) return 0;
    const auto &text = split->first;
    const auto num = [&](std::size_t from, std::size_t size) {
      int value = 0;
      for (std::size_t i = from; i < from + size; ++i) value = value * 10 + (text[i] - '0');
      return value;
    };
    const auto month = num(4, 2), day = num(6, 2), hour = num(9, 2), minute = num(11, 2), second = num(13, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) return 0;
    const auto days = days_from_civil(num(0, 4), static_cast<unsigned>(month), static_cast<unsigned>(day));
    return ((days * 24 + hour) * 60 + minute) * 60000 + static_cast<std::int64_t>(second) * 1000;
  }

  std::vector<backup_t>
  list_backups(const paths_t &paths) {
    std::vector<std::pair<std::pair<std::string, int>, backup_t>> found;
    const auto prefix = paths.config.filename().string() + std::string { infix };
    std::error_code error;
    fs::directory_iterator it { folder_of(paths.config), error }, end;
    for (; !error && it != end; it.increment(error)) {
      const auto name = it->path().filename().string();
      if (name.rfind(prefix, 0) != 0) continue;
      const auto stamp = name.substr(prefix.size());
      const auto key = split_stamp(stamp);
      if (!key) continue;
      std::error_code probe;
      if (!it->is_regular_file(probe) || probe) continue;
      backup_t backup;
      backup.stamp = stamp;
      backup.created_ms = parse_stamp(stamp);
      backup.has_apps = fs::exists(copy_of(paths.apps, stamp), probe) && !probe;
      found.emplace_back(*key, std::move(backup));
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<backup_t> out;
    out.reserve(found.size());
    for (auto &entry : found) out.push_back(std::move(entry.second));
    return out;
  }

  result_t
  reset_to_defaults(const paths_t &paths, bool clear_apps, const parse_t &parse, const now_t &now) {
    result_t result;
    try {
      const auto current = read_current(paths.config);
      if (!current) {
        result.error = error_e::read_failed;
        return result;
      }
      std::optional<std::string> bundled;
      if (clear_apps) {
        bundled = read_all(paths.bundled_apps);
        if (!bundled || bundled->empty()) {
          result.error = error_e::defaults_missing;
          return result;
        }
      }

      const auto stamp = unique_stamp(paths, now());
      const auto conf_copy = copy_of(paths.config, stamp);
      const auto apps_copy = copy_of(paths.apps, stamp);
      const auto drop_copies = [&] {
        remove_quietly(conf_copy);
        remove_quietly(apps_copy);
      };
      std::error_code probe;
      if (current->existed && !copy_checked(paths.config, conf_copy)) {
        result.error = error_e::backup_failed;
        return result;
      }
      if (clear_apps && fs::exists(paths.apps, probe) && !copy_checked(paths.apps, apps_copy)) {
        drop_copies();
        result.error = error_e::backup_failed;
        return result;
      }

      if (!write_atomic(paths.config, serialize(only_kept(parse(current->text))))) {
        drop_copies();
        result.error = error_e::write_failed;
        return result;
      }
      if (clear_apps) {
        if (!write_atomic(paths.apps, *bundled)) {
          // the settings go back to what they were: nothing is half done. When even that fails, the copies stay: they hold the earlier settings.
          bool back = true;
          if (current->existed)
            back = write_atomic(paths.config, current->text);
          else
            remove_quietly(paths.config);
          if (back) drop_copies();
          result.error = error_e::write_failed;
          return result;
        }
        result.apps_changed = true;
      }
      if (current->existed) result.backup = stamp;
      rotate(paths);
    }
    catch (const std::exception &) {
      result.error = error_e::write_failed;
    }
    return result;
  }

  result_t
  restore_latest(const paths_t &paths, const parse_t &parse, const now_t &now) {
    result_t result;
    try {
      const auto backups = list_backups(paths);
      if (backups.empty()) {
        result.error = error_e::no_backup;
        return result;
      }
      const auto &latest = backups.front();
      const auto saved = read_all(copy_of(paths.config, latest.stamp));
      std::optional<std::string> saved_apps;
      if (latest.has_apps) saved_apps = read_all(copy_of(paths.apps, latest.stamp));
      const auto current = read_current(paths.config);
      if (!saved || !current || (latest.has_apps && !saved_apps)) {
        result.error = error_e::read_failed;
        return result;
      }

      const auto stamp = unique_stamp(paths, now());
      const auto conf_copy = copy_of(paths.config, stamp);
      const auto apps_copy = copy_of(paths.apps, stamp);
      const auto drop_copies = [&] {
        remove_quietly(conf_copy);
        remove_quietly(apps_copy);
      };
      std::error_code probe;
      if (current->existed && !copy_checked(paths.config, conf_copy)) {
        result.error = error_e::backup_failed;
        return result;
      }
      const auto apps_now = latest.has_apps && fs::exists(paths.apps, probe);
      if (apps_now && !copy_checked(paths.apps, apps_copy)) {
        drop_copies();
        result.error = error_e::backup_failed;
        return result;
      }

      // the kept keys stay as they are now, whatever the copy says (a key that is not there now is not there after)
      auto settings = parse(*saved);
      const auto now_settings = parse(current->text);
      for (const auto key : kept_keys()) {
        const std::string name { key };
        settings.erase(name);
        const auto found = now_settings.find(name);
        if (found != now_settings.end()) settings.emplace(*found);
      }
      if (!write_atomic(paths.config, serialize(settings))) {
        drop_copies();
        result.error = error_e::write_failed;
        return result;
      }
      if (latest.has_apps) {
        if (!write_atomic(paths.apps, *saved_apps)) {
          bool back = true;
          if (current->existed)
            back = write_atomic(paths.config, current->text);
          else
            remove_quietly(paths.config);
          if (back) drop_copies();
          result.error = error_e::write_failed;
          return result;
        }
        result.apps_changed = true;
      }
      result.restored = latest.stamp;
      if (current->existed) result.backup = stamp;
      rotate(paths);
    }
    catch (const std::exception &) {
      result.error = error_e::write_failed;
    }
    return result;
  }

}  // namespace settings_reset
