/**
 * @file src/webhook/webhook_auth.cpp
 * @brief Complete Webhook configuration stored separately from sunshine.conf.
 */

#include "webhook_auth.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace webhook::auth {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::uintmax_t MAX_FILE_SIZE = 64 * 1024;
    std::mutex auth_file_mutex;

    bool
    valid(const settings_t &settings) noexcept {
      return validate_configuration(settings);
    }

    void
    remove_temp_file(const fs::path &path) noexcept {
      if (path.empty()) {
        return;
      }
      std::error_code ignored;
      fs::remove(path, ignored);
    }

    bool
    replace_file(const fs::path &temporary_path, const fs::path &destination_path) noexcept {
#ifdef _WIN32
      return MoveFileExW(
               temporary_path.c_str(),
               destination_path.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
             ) != FALSE;
#else
      std::error_code ec;
      fs::rename(temporary_path, destination_path, ec);
      return !ec;
#endif
    }

    bool
    tighten_permissions(const fs::path &path) noexcept {
#ifdef _WIN32
      (void) path;
      return true;
#else
      std::error_code permission_error;
      fs::permissions(
        path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        permission_error
      );
      return !permission_error;
#endif
    }

    bool
    finish_output(std::ofstream &file) noexcept {
      file.flush();
      if (!file.good()) {
        file.close();
        return false;
      }
      file.close();
      return !file.fail();
    }

    bool
    backup_existing_file(const fs::path &source_path, const fs::path &backup_path) noexcept {
      fs::path temporary_backup = backup_path;
      temporary_backup += ".tmp";
      remove_temp_file(temporary_backup);

      try {
        std::ifstream source(source_path, std::ios::binary);
        if (!source.is_open()) {
          return false;
        }

        std::ofstream destination(temporary_backup, std::ios::binary | std::ios::trunc);
        if (!destination.is_open()) {
          remove_temp_file(temporary_backup);
          return false;
        }
        if (!tighten_permissions(temporary_backup)) {
          destination.close();
          remove_temp_file(temporary_backup);
          return false;
        }

        std::array<char, 8192> buffer {};
        while (source) {
          source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
          const auto bytes_read = source.gcount();
          if (bytes_read > 0) {
            destination.write(buffer.data(), bytes_read);
          }
        }
        if (source.bad() || !finish_output(destination)) {
          remove_temp_file(temporary_backup);
          return false;
        }
        source.close();

        if (!replace_file(temporary_backup, backup_path)) {
          remove_temp_file(temporary_backup);
          return false;
        }
        return true;
      }
      catch (...) {
        remove_temp_file(temporary_backup);
        return false;
      }
    }
  }  // namespace

  fs::path
  path_for(const fs::path &sunshine_config_file) {
    if (sunshine_config_file.empty()) {
      return {};
    }
    return sunshine_config_file.parent_path() / "webhook_auth.json";
  }

  fs::path
  backup_path_for(const fs::path &webhook_auth_file) {
    if (webhook_auth_file.empty()) {
      return {};
    }
    fs::path backup = webhook_auth_file;
    backup += ".bak";
    return backup;
  }

  bool
  parse_event_ids(std::string_view value, std::vector<int> &events) noexcept {
    try {
      if (value == "-1") {
        events.clear();
        return true;
      }
      if (value.empty()) {
        return false;
      }

      std::vector<int> parsed;
      std::bitset<EVENT_TYPE_COUNT> seen;
      while (!value.empty()) {
        const auto separator = value.find(',');
        auto token = value.substr(0, separator);
        const auto first = token.find_first_not_of(" \t\r\n");
        const auto last = token.find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
          return false;
        }
        token = token.substr(first, last - first + 1);

        int event_id = -1;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), event_id);
        if (result.ec != std::errc {} ||
            result.ptr != token.data() + token.size() ||
            event_id < 0 ||
            event_id >= EVENT_TYPE_COUNT ||
            seen.test(static_cast<std::size_t>(event_id))) {
          return false;
        }
        seen.set(static_cast<std::size_t>(event_id));
        parsed.push_back(event_id);

        if (separator == std::string_view::npos) {
          break;
        }
        if (separator + 1 == value.size()) {
          return false;
        }
        value.remove_prefix(separator + 1);
      }

      std::sort(parsed.begin(), parsed.end());
      events = std::move(parsed);
      return true;
    }
    catch (...) {
      return false;
    }
  }

  std::string
  serialize_event_ids(const std::vector<int> &events) {
    if (events.empty()) {
      return "-1";
    }

    std::vector<int> normalized = events;
    std::sort(normalized.begin(), normalized.end());
    std::string result;
    for (const auto event_id : normalized) {
      if (!result.empty()) {
        result.push_back(',');
      }
      result += std::to_string(event_id);
    }
    return result;
  }

  load_result_t
  load(const fs::path &path) noexcept {
    if (path.empty()) {
      return {load_status_t::INVALID, {}};
    }

    try {
      std::lock_guard<std::mutex> lock(auth_file_mutex);

      std::error_code ec;
      const bool exists = fs::exists(path, ec);
      if (ec) {
        return {load_status_t::INVALID, {}};
      }
      if (!exists) {
        return {load_status_t::MISSING, {}};
      }

      const auto size = fs::file_size(path, ec);
      if (ec || size == 0 || size > MAX_FILE_SIZE) {
        return {load_status_t::INVALID, {}};
      }

      std::ifstream file(path, std::ios::binary);
      if (!file.is_open()) {
        return {load_status_t::INVALID, {}};
      }

      std::string contents(static_cast<std::size_t>(size), '\0');
      file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
      if (file.gcount() != static_cast<std::streamsize>(contents.size()) ||
          file.peek() != std::char_traits<char>::eof()) {
        return {load_status_t::INVALID, {}};
      }

      const auto input = nlohmann::json::parse(contents);
      if (!input.is_object() || input.size() != 5 ||
          !input.contains("webhook_enabled") || !input["webhook_enabled"].is_boolean() ||
          !input.contains("webhook_url") || !input["webhook_url"].is_string() ||
          !input.contains("webhook_skip_ssl_verify") || !input["webhook_skip_ssl_verify"].is_boolean() ||
          !input.contains("webhook_timeout") || !input["webhook_timeout"].is_number_integer() ||
          !input.contains("webhook_events") || !input["webhook_events"].is_string()) {
        return {load_status_t::INVALID, {}};
      }

      const auto timeout = input["webhook_timeout"].get<std::int64_t>();
      if (timeout < MIN_TIMEOUT.count() || timeout > MAX_TIMEOUT.count()) {
        return {load_status_t::INVALID, {}};
      }

      std::vector<int> events;
      if (!parse_event_ids(input["webhook_events"].get_ref<const std::string &>(), events)) {
        return {load_status_t::INVALID, {}};
      }

      settings_t settings {
        input["webhook_enabled"].get<bool>(),
        input["webhook_url"].get<std::string>(),
        input["webhook_skip_ssl_verify"].get<bool>(),
        std::chrono::milliseconds {timeout},
        std::move(events)
      };
      if (!valid(settings)) {
        return {load_status_t::INVALID, {}};
      }

      return {load_status_t::LOADED, std::move(settings)};
    }
    catch (...) {
      return {load_status_t::INVALID, {}};
    }
  }

  bool
  save(const fs::path &path, const settings_t &settings) noexcept {
    if (path.empty() || !valid(settings)) {
      return false;
    }

    std::unique_lock<std::mutex> lock;
    fs::path temporary_path;
    try {
      lock = std::unique_lock<std::mutex>(auth_file_mutex);
      temporary_path = path;
      temporary_path += ".tmp";
      remove_temp_file(temporary_path);

      const nlohmann::json output {
        {"webhook_enabled", settings.enabled},
        {"webhook_events", serialize_event_ids(settings.events)},
        {"webhook_skip_ssl_verify", settings.skip_ssl_verify},
        {"webhook_timeout", settings.timeout.count()},
        {"webhook_url", settings.url}
      };

      std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
      if (!file.is_open()) {
        remove_temp_file(temporary_path);
        return false;
      }

      // Tighten permissions before any URL or token-bearing content is
      // written. The temporary file can inherit a broader process umask.
      if (!tighten_permissions(temporary_path)) {
        file.close();
        remove_temp_file(temporary_path);
        return false;
      }

      file << output.dump(2) << '\n';
      if (!finish_output(file)) {
        remove_temp_file(temporary_path);
        return false;
      }

      std::error_code exists_error;
      const bool destination_exists = fs::exists(path, exists_error);
      if (exists_error) {
        remove_temp_file(temporary_path);
        return false;
      }
      if (destination_exists &&
          !backup_existing_file(path, backup_path_for(path))) {
        remove_temp_file(temporary_path);
        return false;
      }

      if (!replace_file(temporary_path, path)) {
        remove_temp_file(temporary_path);
        return false;
      }
      return true;
    }
    catch (...) {
      remove_temp_file(temporary_path);
      return false;
    }
  }
}  // namespace webhook::auth
