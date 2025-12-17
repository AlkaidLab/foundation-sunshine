#pragma once

#define WIN32_LEAN_AND_MEAN

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <windows.h>

#include "parsed_config.h"

namespace display_device::vdd_utils {

  using namespace std::chrono_literals;

  // 常量定义
  inline constexpr int kMaxRetryCount = 3;
  inline constexpr auto kInitialRetryDelay = 500ms;
  inline constexpr auto kMaxRetryDelay = 5000ms;

  extern const wchar_t *kVddPipeName;
  extern const DWORD kPipeTimeoutMs;
  extern const DWORD kPipeBufferSize;
  extern const std::chrono::milliseconds kDefaultDebounceInterval;

  // HDR亮度范围结构
  struct hdr_brightness_t {
    float max_nits = 1000.0f;
    float min_nits = 0.001f;
    float max_full_nits = 1000.0f;
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
    std::string resolutions;
    std::string fps;
    bool needs_update = false;
  };

  // 指数退避计算
  std::chrono::milliseconds
  calculate_exponential_backoff(int attempt);

  // VDD命令执行
  bool
  execute_vdd_command(const std::string &action);

  // 管道相关函数
  HANDLE
  connect_to_pipe_with_retry(const wchar_t *pipe_name, int max_retries = 3);

  bool
  execute_pipe_command(const wchar_t *pipe_name, const wchar_t *command, std::string *response = nullptr);

  // 驱动重载函数
  bool
  reload_driver();

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

  bool
  destroy_vdd_monitor();

  void
  enable_vdd();

  void
  disable_vdd();

  void
  disable_enable_vdd();

  void
  toggle_display_power();

  bool
  is_display_on();

  bool
  set_hdr_state(bool enable_hdr);

  bool
  ensure_vdd_extended_mode(const std::string &device_id, const std::unordered_set<std::string> &physical_devices_to_preserve = {});

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