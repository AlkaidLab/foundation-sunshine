/**
 * @file src/ds5_config.cpp
 * @brief Independent DualSense settings persistence and runtime snapshot.
 */

#include "ds5_config.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace ds5_config {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::uintmax_t MAX_FILE_SIZE = 64 * 1024;
    std::mutex settings_file_mutex;
    std::atomic<std::shared_ptr<const settings_t>> active_settings {
      std::make_shared<const settings_t>()
    };

    void remove_temp_file(const fs::path &path) noexcept {
      if (path.empty()) return;
      std::error_code ignored;
      fs::remove(path, ignored);
    }

    bool replace_file(const fs::path &temporary_path, const fs::path &destination_path) noexcept {
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

    bool finish_output(std::ofstream &file) noexcept {
      file.flush();
      if (!file.good()) {
        file.close();
        return false;
      }
      file.close();
      return !file.fail();
    }

    bool backup_existing_file(const fs::path &source_path, const fs::path &backup_path) noexcept {
      fs::path temporary_backup = backup_path;
      temporary_backup += ".tmp";
      remove_temp_file(temporary_backup);

      try {
        std::ifstream source(source_path, std::ios::binary);
        std::ofstream destination(temporary_backup, std::ios::binary | std::ios::trunc);
        if (!source.is_open() || !destination.is_open()) {
          remove_temp_file(temporary_backup);
          return false;
        }

        std::array<char, 8192> buffer {};
        while (source) {
          source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
          const auto bytes_read = source.gcount();
          if (bytes_read > 0) destination.write(buffer.data(), bytes_read);
        }
        if (source.bad() || !finish_output(destination)) {
          remove_temp_file(temporary_backup);
          return false;
        }
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

  std::filesystem::path path_for(const std::filesystem::path &sunshine_config_file) {
    return sunshine_config_file.empty() ?
             std::filesystem::path {} : sunshine_config_file.parent_path() / "ds5_config.json";
  }

  std::filesystem::path backup_path_for(const std::filesystem::path &settings_file) {
    if (settings_file.empty()) return {};
    auto backup = settings_file;
    backup += ".bak";
    return backup;
  }

  bool validate(const settings_t &settings) noexcept {
    const auto valid_profile = settings.legacy_profile == legacy_profile_t::custom ||
                               settings.legacy_profile == legacy_profile_t::quiet ||
                               settings.legacy_profile == legacy_profile_t::balanced ||
                               settings.legacy_profile == legacy_profile_t::strong;
    const auto valid_response = settings.legacy_response == legacy_response_t::fast ||
                                settings.legacy_response == legacy_response_t::balanced ||
                                settings.legacy_response == legacy_response_t::smooth;
    return settings.revision > 0 &&
           valid_profile && valid_response &&
           std::isfinite(settings.legacy_strength) &&
           settings.legacy_strength >= MIN_STRENGTH && settings.legacy_strength <= MAX_STRENGTH &&
           std::isfinite(settings.legacy_curve) &&
           settings.legacy_curve >= MIN_CURVE && settings.legacy_curve <= MAX_CURVE &&
           std::isfinite(settings.legacy_noise_gate) &&
           settings.legacy_noise_gate >= MIN_NOISE_GATE && settings.legacy_noise_gate <= MAX_NOISE_GATE &&
           std::isfinite(settings.legacy_max_output) &&
           settings.legacy_max_output >= MIN_MAX_OUTPUT && settings.legacy_max_output <= MAX_MAX_OUTPUT &&
           std::isfinite(settings.legacy_high_scale) &&
           settings.legacy_high_scale >= MIN_HIGH_SCALE && settings.legacy_high_scale <= MAX_HIGH_SCALE &&
           std::isfinite(settings.legacy_body_mix) &&
           settings.legacy_body_mix >= 0.0 && settings.legacy_body_mix <= MAX_BODY_MIX;
  }

  bool uses_stock_legacy_renderer(const settings_t &settings) noexcept {
    return settings.legacy_profile == legacy_profile_t::custom &&
           settings.legacy_strength == 1.0 &&
           settings.legacy_curve == 0.5 &&
           settings.legacy_noise_gate == 0.020 &&
           settings.legacy_max_output == 1.0 &&
           settings.legacy_high_scale == 1.0 &&
           settings.legacy_response == legacy_response_t::balanced &&
           settings.legacy_body_mix == 0.0;
  }

  std::string_view legacy_profile_name(legacy_profile_t profile) noexcept {
    switch (profile) {
      case legacy_profile_t::quiet: return "quiet";
      case legacy_profile_t::balanced: return "balanced";
      case legacy_profile_t::strong: return "strong";
      case legacy_profile_t::custom: return "custom";
    }
    return "custom";
  }

  bool parse_legacy_profile(std::string_view value, legacy_profile_t &profile) noexcept {
    if (value == "quiet") profile = legacy_profile_t::quiet;
    else if (value == "balanced") profile = legacy_profile_t::balanced;
    else if (value == "strong") profile = legacy_profile_t::strong;
    else if (value == "custom") profile = legacy_profile_t::custom;
    else return false;
    return true;
  }

  std::string_view legacy_response_name(legacy_response_t response) noexcept {
    switch (response) {
      case legacy_response_t::fast: return "fast";
      case legacy_response_t::balanced: return "balanced";
      case legacy_response_t::smooth: return "smooth";
    }
    return "balanced";
  }

  bool parse_legacy_response(std::string_view value, legacy_response_t &response) noexcept {
    if (value == "fast") response = legacy_response_t::fast;
    else if (value == "balanced") response = legacy_response_t::balanced;
    else if (value == "smooth") response = legacy_response_t::smooth;
    else return false;
    return true;
  }

  settings_t resolve_legacy_profile(settings_t settings) noexcept {
    switch (settings.legacy_profile) {
      case legacy_profile_t::quiet:
        settings.legacy_strength = 0.75;
        settings.legacy_curve = 0.75;
        settings.legacy_noise_gate = 0.008;
        settings.legacy_max_output = 0.55;
        settings.legacy_high_scale = 0.65;
        settings.legacy_response = legacy_response_t::smooth;
        settings.legacy_body_mix = 0.10;
        break;
      case legacy_profile_t::balanced:
        settings.legacy_strength = 1.00;
        settings.legacy_curve = 0.50;
        settings.legacy_noise_gate = 0.006;
        settings.legacy_max_output = 0.70;
        settings.legacy_high_scale = 0.75;
        settings.legacy_response = legacy_response_t::balanced;
        settings.legacy_body_mix = 0.15;
        break;
      case legacy_profile_t::strong:
        settings.legacy_strength = 1.10;
        settings.legacy_curve = 0.40;
        settings.legacy_noise_gate = 0.004;
        settings.legacy_max_output = 0.82;
        settings.legacy_high_scale = 0.85;
        settings.legacy_response = legacy_response_t::fast;
        settings.legacy_body_mix = 0.18;
        break;
      case legacy_profile_t::custom:
        break;
    }
    return settings;
  }

  prepared_settings_t prepare(settings_t settings) noexcept {
    if (!validate(settings)) return {};
    try {
      return prepared_settings_t {std::make_shared<const settings_t>(std::move(settings))};
    }
    catch (...) {
      return {};
    }
  }

  bool commit(prepared_settings_t &&settings) noexcept {
    if (!settings) return false;
    active_settings.store(std::move(settings.settings_), std::memory_order_release);
    return true;
  }

  bool configure(settings_t settings) noexcept {
    return commit(prepare(std::move(settings)));
  }

  settings_t current() {
    return *active_settings.load(std::memory_order_acquire);
  }

  load_result_t load(const std::filesystem::path &path) noexcept {
    if (path.empty()) return {load_status_t::INVALID, {}};
    try {
      std::lock_guard<std::mutex> lock(settings_file_mutex);
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        return ec ? load_result_t {load_status_t::INVALID, {}} :
                    load_result_t {load_status_t::MISSING, {}};
      }
      const auto size = std::filesystem::file_size(path, ec);
      if (ec || size == 0 || size > MAX_FILE_SIZE) return {load_status_t::INVALID, {}};

      std::ifstream file(path, std::ios::binary);
      if (!file.is_open()) return {load_status_t::INVALID, {}};
      std::string contents(static_cast<std::size_t>(size), '\0');
      file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
      if (file.gcount() != static_cast<std::streamsize>(contents.size()) ||
          file.peek() != std::char_traits<char>::eof()) {
        return {load_status_t::INVALID, {}};
      }

      const auto input = nlohmann::json::parse(contents);
      const bool has_extended = input.is_object() && input.size() == 11 &&
                                input.contains("ds5_legacy_haptics_schema") &&
                                input["ds5_legacy_haptics_schema"].is_number_integer() &&
                                input["ds5_legacy_haptics_schema"].get<int>() == 2;
      if (!input.is_object() || (input.size() != 5 && !has_extended) ||
          !input.contains("ds5_enabled") || !input["ds5_enabled"].is_boolean() ||
          !input.contains("ds5_audio_haptics") || !input["ds5_audio_haptics"].is_boolean() ||
          !input.contains("ds5_legacy_haptics_strength") || !input["ds5_legacy_haptics_strength"].is_number() ||
          !input.contains("ds5_legacy_haptics_curve") || !input["ds5_legacy_haptics_curve"].is_number() ||
          !input.contains("ds5_legacy_haptics_noise_gate") || !input["ds5_legacy_haptics_noise_gate"].is_number() ||
          (has_extended && (!input.contains("ds5_legacy_haptics_profile") || !input["ds5_legacy_haptics_profile"].is_string() ||
                            !input.contains("ds5_legacy_haptics_max_output") || !input["ds5_legacy_haptics_max_output"].is_number() ||
                            !input.contains("ds5_legacy_haptics_high_scale") || !input["ds5_legacy_haptics_high_scale"].is_number() ||
                            !input.contains("ds5_legacy_haptics_response") || !input["ds5_legacy_haptics_response"].is_string() ||
                            !input.contains("ds5_legacy_haptics_body_mix") || !input["ds5_legacy_haptics_body_mix"].is_number()))) {
        return {load_status_t::INVALID, {}};
      }

      settings_t settings {
        input["ds5_enabled"].get<bool>(),
        input["ds5_audio_haptics"].get<bool>(),
        input["ds5_legacy_haptics_strength"].get<double>(),
        input["ds5_legacy_haptics_curve"].get<double>(),
        input["ds5_legacy_haptics_noise_gate"].get<double>(),
      };
      if (has_extended) {
        if (!parse_legacy_profile(input["ds5_legacy_haptics_profile"].get<std::string>(), settings.legacy_profile) ||
            !parse_legacy_response(input["ds5_legacy_haptics_response"].get<std::string>(), settings.legacy_response)) {
          return {load_status_t::INVALID, {}};
        }
        settings.legacy_max_output = input["ds5_legacy_haptics_max_output"].get<double>();
        settings.legacy_high_scale = input["ds5_legacy_haptics_high_scale"].get<double>();
        settings.legacy_body_mix = input["ds5_legacy_haptics_body_mix"].get<double>();
      }
      return validate(settings) ? load_result_t {load_status_t::LOADED, settings} :
                                  load_result_t {load_status_t::INVALID, {}};
    }
    catch (...) {
      return {load_status_t::INVALID, {}};
    }
  }

  bool save(const std::filesystem::path &path, const settings_t &settings) noexcept {
    if (path.empty() || !validate(settings)) return false;
    std::filesystem::path temporary_path;
    try {
      std::lock_guard<std::mutex> lock(settings_file_mutex);
      temporary_path = path;
      temporary_path += ".tmp";
      remove_temp_file(temporary_path);

      const nlohmann::json output {
        {"ds5_enabled", settings.enabled},
        {"ds5_audio_haptics", settings.audio_haptics},
        {"ds5_legacy_haptics_strength", settings.legacy_strength},
        {"ds5_legacy_haptics_curve", settings.legacy_curve},
        {"ds5_legacy_haptics_noise_gate", settings.legacy_noise_gate},
        {"ds5_legacy_haptics_schema", 2},
        {"ds5_legacy_haptics_profile", legacy_profile_name(settings.legacy_profile)},
        {"ds5_legacy_haptics_max_output", settings.legacy_max_output},
        {"ds5_legacy_haptics_high_scale", settings.legacy_high_scale},
        {"ds5_legacy_haptics_response", legacy_response_name(settings.legacy_response)},
        {"ds5_legacy_haptics_body_mix", settings.legacy_body_mix},
      };
      std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
      if (!file.is_open()) return false;
      file << output.dump(2) << '\n';
      if (!finish_output(file)) {
        remove_temp_file(temporary_path);
        return false;
      }

      std::error_code exists_error;
      const bool destination_exists = std::filesystem::exists(path, exists_error);
      if (exists_error ||
          (destination_exists && !backup_existing_file(path, backup_path_for(path))) ||
          !replace_file(temporary_path, path)) {
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
}  // namespace ds5_config
