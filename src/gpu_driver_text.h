/**
 * @file src/gpu_driver_text.h
 * @brief The sentences the tray says about the graphics driver and the safe mode.
 *
 * The Panel home and the Web UI home draw the same sentences from foundation-ui (src/driverGuard/texts.js); the tray notification is made by the host, so the
 * words are repeated here, literal for literal. A test of the Web UI (tests/gpuDriverText.test.js) reads this file and compares every line below with the
 * foundation-ui texts: change both or the test fails.
 */
#pragma once

#include "src/gpu_driver_guard.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace gpu_guard::text {

  constexpr const char *ZH_PHASE_STARTUP = "启动";
  constexpr const char *ZH_PHASE_PROBE = "检测编码器";
  constexpr const char *ZH_PHASE_IDLE = "运行";
  constexpr const char *ZH_PHASE_STREAMING = "串流";
  constexpr const char *ZH_OUTCOME_CRASH = "崩溃";
  constexpr const char *ZH_OUTCOME_HANG = "无响应";
  constexpr const char *ZH_DRIVER_TITLE = "上次{phase}时 {vendor} 显卡驱动{outcome}";
  constexpr const char *ZH_DRIVER_BODY = "问题出在驱动内部（版本 {version}，模块 {module}），不是 Sunshine 的问题。更新显卡驱动通常可以解决；已是最新版时，请联系 {vendor} 支持。";
  constexpr const char *ZH_DRIVER_BODY_NO_VERSION = "问题出在驱动内部（模块 {module}），不是 Sunshine 的问题。更新显卡驱动通常可以解决；已是最新版时，请联系 {vendor} 支持。";
  constexpr const char *ZH_SAFE_TITLE = "已进入安全模式";
  constexpr const char *ZH_SAFE_BODY = "连续多次启动失败，本次启动跳过了编码器检测和显示设置恢复，开始串流时才检测编码器。运行 {minutes} 分钟或串流结束后自动退出安全模式。";

  constexpr const char *EN_PHASE_STARTUP = "at startup";
  constexpr const char *EN_PHASE_PROBE = "while checking encoders";
  constexpr const char *EN_PHASE_IDLE = "while running";
  constexpr const char *EN_PHASE_STREAMING = "during a stream";
  constexpr const char *EN_OUTCOME_CRASH = "crashed";
  constexpr const char *EN_OUTCOME_HANG = "stopped responding";
  constexpr const char *EN_DRIVER_TITLE = "The {vendor} graphics driver {outcome} {phase}";
  constexpr const char *EN_DRIVER_BODY = "The failure was inside the driver (version {version}, module {module}), not in Sunshine. Updating the graphics driver usually fixes it; if it is already the latest version, contact {vendor} support.";
  constexpr const char *EN_DRIVER_BODY_NO_VERSION = "The failure was inside the driver (module {module}), not in Sunshine. Updating the graphics driver usually fixes it; if it is already the latest version, contact {vendor} support.";
  constexpr const char *EN_SAFE_TITLE = "Safe mode is on";
  constexpr const char *EN_SAFE_BODY = "After repeated failed starts, this start skipped the encoder check and the display restore. Encoders are checked when a stream starts. Safe mode ends after {minutes} minutes of running or when a stream ends.";

  struct notification_t {
    std::string title;
    std::string message;
  };

  /// "{name}" placeholders of one template.
  inline std::string
  fill(std::string_view tpl, std::initializer_list<std::pair<std::string_view, std::string>> values) {
    std::string out;
    for (std::size_t i = 0; i < tpl.size(); ++i) {
      if (tpl[i] == '{') {
        const auto end = tpl.find('}', i);
        if (end != std::string_view::npos) {
          const auto name = tpl.substr(i + 1, end - i - 1);
          bool found = false;
          for (const auto &[key, value] : values) {
            if (key == name) {
              out += value;
              found = true;
              break;
            }
          }
          if (!found) out += tpl.substr(i, end - i + 1);
          i = end;
          continue;
        }
      }
      out += tpl[i];
    }
    return out;
  }

  inline const char *
  vendor_name(crash::gpu_driver::vendor_e vendor) {
    switch (vendor) {
      case crash::gpu_driver::vendor_e::intel: return "Intel";
      case crash::gpu_driver::vendor_e::nvidia: return "NVIDIA";
      case crash::gpu_driver::vendor_e::amd: return "AMD";
      default: return "";
    }
  }

  /// Whether a locale code ("zh", "zh_CN", "zh-TW") is Chinese; every other locale reads English.
  inline bool
  is_chinese(std::string_view locale) {
    return locale.size() >= 2 && (locale[0] == 'z' || locale[0] == 'Z') && (locale[1] == 'h' || locale[1] == 'H');
  }

  /// The tray notification for the last failure in a graphics driver: the title and the sentence of the home page notice.
  inline notification_t
  driver_notification(const driver_mark_t &mark, bool zh) {
    using crash::gpu_driver::phase_e;
    const char *phase = "";
    switch (mark.phase) {
      case phase_e::startup: phase = zh ? ZH_PHASE_STARTUP : EN_PHASE_STARTUP; break;
      case phase_e::probe: phase = zh ? ZH_PHASE_PROBE : EN_PHASE_PROBE; break;
      case phase_e::idle: phase = zh ? ZH_PHASE_IDLE : EN_PHASE_IDLE; break;
      case phase_e::streaming: phase = zh ? ZH_PHASE_STREAMING : EN_PHASE_STREAMING; break;
    }
    const bool hang = mark.kind == "hang";
    const char *outcome = hang ? (zh ? ZH_OUTCOME_HANG : EN_OUTCOME_HANG) : (zh ? ZH_OUTCOME_CRASH : EN_OUTCOME_CRASH);
    const std::string vendor = vendor_name(mark.vendor);
    notification_t out;
    out.title = fill(zh ? ZH_DRIVER_TITLE : EN_DRIVER_TITLE, { { "phase", phase }, { "vendor", vendor }, { "outcome", outcome } });
    const char *body = mark.driver.empty() ? (zh ? ZH_DRIVER_BODY_NO_VERSION : EN_DRIVER_BODY_NO_VERSION) : (zh ? ZH_DRIVER_BODY : EN_DRIVER_BODY);
    out.message = fill(body, { { "version", mark.driver }, { "module", mark.module }, { "vendor", vendor } });
    return out;
  }

  /// The tray notification for the safe mode.
  inline notification_t
  safe_mode_notification(int auto_exit_s, bool zh) {
    const int minutes = std::max(1, (auto_exit_s + 30) / 60);
    notification_t out;
    out.title = zh ? ZH_SAFE_TITLE : EN_SAFE_TITLE;
    out.message = fill(zh ? ZH_SAFE_BODY : EN_SAFE_BODY, { { "minutes", std::to_string(minutes) } });
    return out;
  }

}  // namespace gpu_guard::text
