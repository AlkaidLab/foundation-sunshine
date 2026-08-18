/**
 * @file src/ds5_config.h
 * @brief Independently persisted, hot-applied DualSense settings.
 */
#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace ds5_config {
  inline constexpr double MIN_STRENGTH = 0.1;
  inline constexpr double MAX_STRENGTH = 4.0;
  inline constexpr double MIN_CURVE = 0.3;
  inline constexpr double MAX_CURVE = 2.0;
  inline constexpr double MIN_NOISE_GATE = 0.002;
  inline constexpr double MAX_NOISE_GATE = 0.060;
  inline constexpr double MIN_MAX_OUTPUT = 0.25;
  inline constexpr double MAX_MAX_OUTPUT = 1.0;
  inline constexpr double MIN_HIGH_SCALE = 0.25;
  inline constexpr double MAX_HIGH_SCALE = 1.0;
  inline constexpr double MAX_BODY_MIX = 0.35;

  enum class legacy_profile_t {
    custom,
    quiet,
    balanced,
    strong,
  };

  enum class legacy_response_t {
    fast,
    balanced,
    smooth,
  };

  struct settings_t {
    bool enabled = false;
    bool audio_haptics = true;
    double legacy_strength = 1.0;
    double legacy_curve = 1.0;
    double legacy_noise_gate = 0.020;
    legacy_profile_t legacy_profile = legacy_profile_t::custom;
    double legacy_max_output = 1.0;
    double legacy_high_scale = 1.0;
    legacy_response_t legacy_response = legacy_response_t::balanced;
    double legacy_body_mix = 0.0;
    std::uint64_t revision = 1;
  };

  enum class load_status_t {
    LOADED,
    MISSING,
    INVALID
  };

  struct load_result_t {
    load_status_t status = load_status_t::MISSING;
    settings_t settings;
  };

  class prepared_settings_t {
  public:
    prepared_settings_t(const prepared_settings_t &) = delete;
    prepared_settings_t &operator=(const prepared_settings_t &) = delete;
    prepared_settings_t(prepared_settings_t &&) noexcept = default;
    prepared_settings_t &operator=(prepared_settings_t &&) noexcept = default;
    ~prepared_settings_t() = default;

    explicit operator bool() const noexcept {
      return settings_ != nullptr;
    }

    const settings_t &value() const noexcept {
      return *settings_;
    }

  private:
    friend prepared_settings_t prepare(settings_t settings) noexcept;
    friend bool commit(prepared_settings_t &&settings) noexcept;

    prepared_settings_t() noexcept = default;
    explicit prepared_settings_t(std::shared_ptr<const settings_t> settings) noexcept:
        settings_(std::move(settings)) {
    }

    std::shared_ptr<const settings_t> settings_;
  };

  std::filesystem::path path_for(const std::filesystem::path &sunshine_config_file);
  std::filesystem::path backup_path_for(const std::filesystem::path &settings_file);

  bool validate(const settings_t &settings) noexcept;
  settings_t resolve_legacy_profile(settings_t settings) noexcept;
  std::string_view legacy_profile_name(legacy_profile_t profile) noexcept;
  bool parse_legacy_profile(std::string_view value, legacy_profile_t &profile) noexcept;
  std::string_view legacy_response_name(legacy_response_t response) noexcept;
  bool parse_legacy_response(std::string_view value, legacy_response_t &response) noexcept;
  prepared_settings_t prepare(settings_t settings) noexcept;
  bool commit(prepared_settings_t &&settings) noexcept;
  bool configure(settings_t settings) noexcept;
  settings_t current();

  load_result_t load(const std::filesystem::path &path) noexcept;
  bool save(const std::filesystem::path &path, const settings_t &settings) noexcept;
}  // namespace ds5_config
