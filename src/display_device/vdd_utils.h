#pragma once

#define WIN32_LEAN_AND_MEAN

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

#include <boost/optional.hpp>

#include "parsed_config.h"

namespace display_device::vdd_utils {

  using namespace std::chrono_literals;

  // 常量定义
  inline constexpr int kMaxRetryCount = 3;
  inline constexpr auto kInitialRetryDelay = 500ms;
  inline constexpr auto kMaxRetryDelay = 3000ms;

  extern const std::chrono::milliseconds kDefaultDebounceInterval;

  // HDR亮度范围结构
  struct hdr_brightness_t {
    float max_nits = 1000.0f;
    float min_nits = 0.001f;
    float max_full_nits = 1000.0f;

    bool operator==(const hdr_brightness_t &) const = default;

    // 实测上报的亮度逐会话存在小抖动;容差内视为能力未变,避免无谓重建 VDD
    bool
    nearly_equal(const hdr_brightness_t &other) const {
      constexpr auto close = [](float a, float b) {
        const float scale = std::min(std::abs(a), std::abs(b));
        return std::abs(a - b) <= std::max(25.0f, 0.05f * scale);
      };
      return close(max_nits, other.max_nits) &&
             close(min_nits, other.min_nits) &&
             close(max_full_nits, other.max_full_nits);
    }
  };

  // 物理尺寸结构（厘米）
  struct physical_size_t {
    float width_cm = 0.0f;   // 宽度（厘米），0表示未指定
    float height_cm = 0.0f;  // 高度（厘米），0表示未指定
  };

  // 重试配置结构
  struct RetryConfig {
    int max_attempts = kMaxRetryCount;
    std::chrono::milliseconds initial_delay = kInitialRetryDelay;
    std::chrono::milliseconds max_delay = kMaxRetryDelay;
    std::string_view context;
  };

  // VDD设置结构
  struct VddSettings {
    std::vector<resolution_t> resolution_modes;
    std::vector<unsigned int> refresh_rates_hz;
  };

  enum class advertised_mode_match_e {
    none,
    exact,
    rotation_equivalent,
  };

  /**
   * @brief Classify a Windows-advertised mode against a client-requested mode.
   * @details Windows exposes logical width and height after display rotation.
   *          A swapped match is valid only when the current orientation swaps
   *          the display axes (90 or 270 degrees).
   */
  constexpr advertised_mode_match_e
  classify_advertised_mode(unsigned int width,
    unsigned int height,
    unsigned int refresh_hz,
    const display_mode_t &requested_mode,
    bool orientation_swaps_axes) {
    if (requested_mode.refresh_rate.denominator == 0) {
      return advertised_mode_match_e::none;
    }

    const auto advertised_scaled =
      static_cast<std::uint64_t>(refresh_hz) * requested_mode.refresh_rate.denominator;
    const auto requested_scaled =
      static_cast<std::uint64_t>(requested_mode.refresh_rate.numerator);
    const auto difference = advertised_scaled > requested_scaled ?
                              advertised_scaled - requested_scaled :
                              requested_scaled - advertised_scaled;
    if (difference > requested_mode.refresh_rate.denominator) {
      return advertised_mode_match_e::none;
    }

    if (width == requested_mode.resolution.width &&
        height == requested_mode.resolution.height) {
      return advertised_mode_match_e::exact;
    }

    if (orientation_swaps_axes &&
        width == requested_mode.resolution.height &&
        height == requested_mode.resolution.width) {
      return advertised_mode_match_e::rotation_equivalent;
    }

    return advertised_mode_match_e::none;
  }

  constexpr bool
  advertised_mode_matches(unsigned int width,
    unsigned int height,
    unsigned int refresh_hz,
    const display_mode_t &requested_mode) {
    return classify_advertised_mode(width, height, refresh_hz, requested_mode, false) !=
           advertised_mode_match_e::none;
  }

  /**
   * @brief Check whether Windows exposes a requested mode for a display device.
   * @details SETMODES updates the driver mode list, but an existing IddCx
   *          monitor can retain a stale monitor-description list. This checks
   *          the effective modes published to Windows, not just IOCTL success.
   */
  bool
  is_mode_advertised(const std::string &device_id, const display_mode_t &requested_mode);

  /**
   * @brief Wait until Windows exposes a requested mode for a display device.
   * @details Uses a bounded deadline internally. Callers do not need to encode
   *          driver timing assumptions or retry counts.
   */
  bool
  wait_for_mode_publication(const std::string &device_id, const display_mode_t &requested_mode);

  struct vdd_status_t {
    std::string state;
    bool installed = false;
    bool running = false;
    bool control_available = false;
    bool monitor_active = false;
    bool problem_code_valid = false;
    std::uint32_t problem_code = 0;

    constexpr bool
    is_usable() const {
      return installed && problem_code_valid && running && control_available;
    }
  };

  enum class vdd_prerequisite_e {
    usable,
    not_installed,
    unavailable,
  };

  constexpr vdd_prerequisite_e
  classify_vdd_prerequisite(const vdd_status_t &status) {
    if (status.is_usable()) {
      return vdd_prerequisite_e::usable;
    }
    return status.installed ? vdd_prerequisite_e::unavailable : vdd_prerequisite_e::not_installed;
  }

  constexpr std::string_view
  classify_vdd_state(bool installed, bool running, bool control_available, bool problem_code_valid, std::uint32_t problem_code) {
    if (!installed) {
      return "not_installed";
    }
    if (!problem_code_valid) {
      return "unknown";
    }
    if (!running) {
      return problem_code == 14 ? "reboot_required" : "unhealthy";
    }
    return control_available ? "ready" : "degraded";
  }

  /**
   * @brief Return a fast, read-only VDD prerequisite snapshot.
   * @details This only performs read-only device and IOCTL probes, so callers
   *          can use it in Web status polling and stream startup.
   */
  vdd_status_t
  get_vdd_status();

  /**
   * @brief Parse the persisted VDD HardwareCursor value.
   */
  bool
  hardware_cursor_export_enabled(std::string value);

  constexpr bool
  hardware_cursor_export_needs_enable(bool persisted_enabled, bool live_enable_confirmed) {
    return !persisted_enabled || !live_enable_confirmed;
  }

  /**
   * @brief Ensure ZakoVDD continuously exports its hardware cursor channel.
   * @details Direct VDD capture consumes this channel for both server-side
   *          composition and client-side local cursor synchronization. Once
   *          enabled, switching cursor ownership is a session-only operation
   *          and does not require another driver reload.
   * @param changed Optional output set to true when this call had to update the driver setting.
   * @return True when cursor export was already enabled or was updated successfully.
   */
  bool
  ensure_hardware_cursor_enabled_for_capture(bool *changed = nullptr);

  /**
   * @brief Outcome of attempting a live SETMODES update.
   * @details Lets callers distinguish "driver accepted" / "driver rejected" /
   *          "feature not present" / "config is unusable" without conflating
   *          transport failure with invalid input.
   */
  enum class set_vdd_result {
    ok,                 ///< Driver accepted the live mode update.
    failed,             ///< Driver reachable but rejected the IOCTL.
    interface_missing,  ///< IOCTL interface not present (old driver).
    invalid_config,     ///< Resolution/refresh rate missing or unusable; nothing was sent.
  };

  /**
   * @brief Push the complete session mode list to ZakoVDD in-memory mode list.
   * @details Uses the SETMODES IOCTL command exposed by newer ZakoVDD builds.
   *          This does not persist the session resolution to vdd_settings.xml.
   * @param config Parsed display configuration containing the requested session mode.
   * @param settings Full standard mode list plus the requested session mode.
   * @return Typed outcome describing whether the complete live update was accepted.
   */
  set_vdd_result
  set_vdd_session_mode(const parsed_config_t &config, const VddSettings &settings);

  /**
   * @brief 从客户端标识符生成GUID字符串（用于驱动识别）
   * @param identifier 客户端标识符，如果为空则返回空字符串
   * @return GUID格式字符串: {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}，如果identifier为空则返回空字符串
   */
  std::string
  generate_client_guid(const std::string &identifier);

  /**
   * @brief 从客户端配置中获取物理尺寸
   * @param client_name 客户端名称
   * @return 物理尺寸结构，如果未找到则返回默认值（0,0）
   */
  physical_size_t
  get_client_physical_size(const std::string &client_name);

  /**
   * @brief 创建VDD监视器
   * @param client_identifier 客户端标识符（可选），用于驱动识别客户端并启动对应的显示器
   * @param hdr_brightness HDR亮度配置
   * @param physical_size 物理尺寸配置（厘米），可选
   * @return 创建是否成功
   */
  bool
  create_vdd_monitor(const std::string &client_identifier = "", const hdr_brightness_t &hdr_brightness = {}, const physical_size_t &physical_size = {});

  /**
   * Create a VDD for a user-session provider without displaying Core UI.
   * The provider owns confirmation; Core performs bounded topology setup.
   */
  bool
  create_vdd_monitor_noninteractive();

  bool
  destroy_vdd_monitor();

  /**
   * @brief Shutdown-safe VDD destroy. Uses raw Win32 pipe API without BOOST_LOG.
   * Safe to call from destructors where boost::log may already be destroyed.
   */
  void
  destroy_vdd_monitor_nolog();

  void
  disable_enable_vdd();

  bool
  toggle_display_power();

  bool
  is_display_on();

  bool
  set_hdr_state(bool enable_hdr);

  bool
  ensure_vdd_extended_mode(const std::string &device_id, const std::vector<std::string> &physical_devices_to_preserve = {});

  /**
   * @brief Apply VDD prep settings to handle physical displays.
   * @param vdd_device_id The VDD device ID.
   * @param vdd_prep The vdd_prep_e value specifying how to handle physical displays.
   * @param pre_vdd_devices Physical device info captured before VDD creation.
   *        An engaged empty map represents a headless host. An unengaged value
   *        falls back to current device enumeration.
   * @returns True if the operation succeeded.
   * @note This operation modifies topology without saving/restoring state,
   *       as Windows automatically handles topology memory when displays change.
   */
  bool
  apply_vdd_prep(const std::string &vdd_device_id, parsed_config_t::vdd_prep_e vdd_prep,
    const boost::optional<device_info_map_t> &pre_vdd_devices);

  VddSettings
  prepare_vdd_settings(const parsed_config_t &config);

  // 重试函数模板
  template <typename Func>
  bool
  retry_with_backoff(Func &&check_func, const RetryConfig &config) {
    auto delay = config.initial_delay;

    for (int attempt = 0; attempt < config.max_attempts; ++attempt) {
      if (check_func()) {
        return true;
      }

      if (attempt + 1 < config.max_attempts) {
        std::this_thread::sleep_for(delay);
        delay = std::min(config.max_delay, delay * 2);
      }
    }
    return false;
  }

}  // namespace display_device::vdd_utils
