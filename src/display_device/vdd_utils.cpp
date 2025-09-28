#define WIN32_LEAN_AND_MEAN

#include "vdd_utils.h"

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <filesystem>
#include <sstream>
#include <thread>

#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/system_tray.h"
#include "to_string.h"

namespace display_device {
  namespace vdd_utils {

    const wchar_t *kVddPipeName = L"\\\\.\\pipe\\ZakoVDDPipe";
    const DWORD kPipeTimeoutMs = 5000;
    const DWORD kPipeBufferSize = 4096;
    const std::chrono::milliseconds kDefaultDebounceInterval { 2000 };

    // 上次切换显示器的时间点
    static std::chrono::steady_clock::time_point last_toggle_time { std::chrono::steady_clock::now() };
    // 防抖间隔
    static std::chrono::milliseconds debounce_interval { kDefaultDebounceInterval };

    std::chrono::milliseconds
    calculate_exponential_backoff(int attempt) {
      auto delay = kInitialRetryDelay * (1 << attempt);
      return std::min(delay, kMaxRetryDelay);
    }

    bool
    execute_vdd_command(const std::string &action) {
      static const std::string kDevManPath = (std::filesystem::path(SUNSHINE_ASSETS_DIR).parent_path() / "tools" / "DevManView.exe").string();
      static const std::string kDriverName = "Zako Display Adapter";

      boost::process::environment _env = boost::this_process::environment();
      auto working_dir = boost::filesystem::path();
      std::error_code ec;

      std::string cmd = kDevManPath + " /" + action + " \"" + kDriverName + "\"";

      for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
        auto child = platf::run_command(true, true, cmd, working_dir, _env, nullptr, ec, nullptr);
        if (!ec) {
          BOOST_LOG(info) << "成功执行VDD " << action << " 命令";
          child.detach();
          return true;
        }

        auto delay = calculate_exponential_backoff(attempt);
        BOOST_LOG(warning) << "执行VDD " << action << " 命令失败 (尝试 "
                           << (attempt + 1) << "/" << kMaxRetryCount
                           << "): " << ec.message() << ". 将在 "
                           << delay.count() << "ms 后重试";
        std::this_thread::sleep_for(delay);
      }

      BOOST_LOG(error) << "执行VDD " << action << " 命令失败，已达到最大重试次数";
      return false;
    }

    HANDLE
    connect_to_pipe_with_retry(const wchar_t *pipe_name, int max_retries) {
      HANDLE hPipe = INVALID_HANDLE_VALUE;
      int attempt = 0;
      auto retry_delay = kInitialRetryDelay;

      while (attempt < max_retries) {
        hPipe = CreateFileW(
          pipe_name,
          GENERIC_READ | GENERIC_WRITE,
          0,
          NULL,
          OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED,  // 使用异步IO
          NULL);

        if (hPipe != INVALID_HANDLE_VALUE) {
          DWORD mode = PIPE_READMODE_MESSAGE;
          if (SetNamedPipeHandleState(hPipe, &mode, NULL, NULL)) {
            return hPipe;
          }
          CloseHandle(hPipe);
        }

        ++attempt;
        retry_delay = calculate_exponential_backoff(attempt);
        std::this_thread::sleep_for(retry_delay);
      }
      return INVALID_HANDLE_VALUE;
    }

    bool
    execute_pipe_command(const wchar_t *pipe_name, const wchar_t *command, std::string *response) {
      auto hPipe = connect_to_pipe_with_retry(pipe_name);
      if (hPipe == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "连接MTT虚拟显示管道失败，已重试多次";
        return false;
      }

      // 异步IO结构体
      OVERLAPPED overlapped = { 0 };
      overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

      struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
          if (handle) CloseHandle(handle);
        }
      } event_guard { overlapped.hEvent };

      // 发送命令（使用宽字符版本）
      DWORD bytesWritten;
      size_t cmd_len = (wcslen(command) + 1) * sizeof(wchar_t);  // 包含终止符
      if (!WriteFile(hPipe, command, (DWORD) cmd_len, &bytesWritten, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          BOOST_LOG(error) << L"发送" << command << L"命令失败，错误代码: " << GetLastError();
          return false;
        }

        // 等待写入完成
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
          BOOST_LOG(error) << L"发送" << command << L"命令超时";
          return false;
        }
      }

      // 读取响应
      if (response) {
        char buffer[kPipeBufferSize];
        DWORD bytesRead;
        if (!ReadFile(hPipe, buffer, sizeof(buffer), &bytesRead, &overlapped)) {
          if (GetLastError() != ERROR_IO_PENDING) {
            BOOST_LOG(warning) << "读取响应失败，错误代码: " << GetLastError();
            return false;
          }

          DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
          if (waitResult == WAIT_OBJECT_0 && GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE)) {
            buffer[bytesRead] = '\0';
            *response = std::string(buffer, bytesRead);
          }
        }
      }

      return true;
    }

    bool
    reload_driver() {
      std::string response;
      return execute_pipe_command(kVddPipeName, L"RELOAD_DRIVER", &response);
    }

    bool
    create_vdd_monitor() {
      std::string response;
      if (!execute_pipe_command(kVddPipeName, L"CREATEMONITOR", &response)) {
        BOOST_LOG(error) << "创建虚拟显示器失败";
        return false;
      }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_vmonitor_checked(1);
#endif
      BOOST_LOG(info) << "创建虚拟显示器完成，响应: " << response;
      return true;
    }

    bool
    destroy_vdd_monitor() {
      std::string response;
      if (!execute_pipe_command(kVddPipeName, L"DESTROYMONITOR", &response)) {
        BOOST_LOG(error) << "销毁虚拟显示器失败";
        return false;
      }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_vmonitor_checked(0);
#endif
      BOOST_LOG(info) << "销毁虚拟显示器完成，响应: " << response;
      return true;
    }

    void
    enable_vdd() {
      execute_vdd_command("enable");
    }

    void
    disable_vdd() {
      execute_vdd_command("disable");
    }

    void
    disable_enable_vdd() {
      execute_vdd_command("disable_enable");
    }

    bool
    is_display_on() {
      return !display_device::find_device_by_friendlyname(ZAKO_NAME).empty();
    }

    void
    toggle_display_power() {
      auto now = std::chrono::steady_clock::now();

      if (now - last_toggle_time < debounce_interval) {
        BOOST_LOG(debug) << "忽略快速重复的显示器开关请求，请等待"
                         << std::chrono::duration_cast<std::chrono::seconds>(
                              debounce_interval - (now - last_toggle_time))
                              .count()
                         << "秒";
        return;
      }

      last_toggle_time = now;

      if (!is_display_on()) {
        if (create_vdd_monitor()) {
          std::thread([]() {
            // Windows弹窗确认
            auto future = std::async(std::launch::async, []() {
              return MessageBoxW(nullptr,
                       L"已创建虚拟显示器，是否继续使用？\n\n"
                       L"如不确认，20秒后将自动关闭显示器",
                       L"显示器确认",
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
            });

            // 等待20秒超时
            if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready || !future.get()) {
              BOOST_LOG(info) << "用户未确认或超时，自动销毁虚拟显示器";
              HWND hwnd = FindWindowW(L"#32770", L"显示器确认");
              if (hwnd && IsWindow(hwnd)) {
                // 发送退出命令并等待窗口关闭
                PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
                PostMessage(hwnd, WM_CLOSE, 0, 0);

                for (int i = 0; i < 5 && IsWindow(hwnd); ++i) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                // 如果窗口还存在，尝试强制关闭
                if (IsWindow(hwnd)) {
                  BOOST_LOG(warning) << "无法正常关闭确认窗口，尝试终止窗口进程";
                  EndDialog(hwnd, IDNO);
                }
              }
              destroy_vdd_monitor();
            }
            else {
              BOOST_LOG(info) << "用户确认保留虚拟显示器";
            }
          }).detach();
        }
      }
      else {
        destroy_vdd_monitor();
      }
    }

    VddSettings
    prepare_vdd_settings(const parsed_config_t &config) {
      auto is_res_cached = false;
      auto is_fps_cached = false;
      std::ostringstream res_stream, fps_stream;

      res_stream << '[';
      fps_stream << '[';

      // 检查分辨率是否已缓存
      for (const auto &res : config::nvhttp.resolutions) {
        res_stream << res << ',';
        if (config.resolution && res == to_string(*config.resolution)) {
          is_res_cached = true;
        }
      }

      // 检查帧率是否已缓存
      for (const auto &fps : config::nvhttp.fps) {
        fps_stream << fps << ',';
        if (config.refresh_rate && std::to_string(fps) == to_string(*config.refresh_rate)) {
          is_fps_cached = true;
        }
      }

      // 如果需要更新设置
      bool needs_update = (!is_res_cached || !is_fps_cached) && config.resolution;
      if (needs_update) {
        if (!is_res_cached) {
          res_stream << to_string(*config.resolution);
        }
        if (!is_fps_cached) {
          fps_stream << to_string(*config.refresh_rate);
        }
      }

      // 移除最后的逗号并添加结束括号
      auto res_str = res_stream.str();
      auto fps_str = fps_stream.str();
      if (res_str.back() == ',') res_str.pop_back();
      if (fps_str.back() == ',') fps_str.pop_back();
      res_str += ']';
      fps_str += ']';

      return { res_str, fps_str, needs_update };
    }

    bool
    set_hdr_state(bool enable_hdr) {
      try {
        // 获取虚拟显示器的设备ID
        auto vdd_device_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
        if (vdd_device_id.empty()) {
          BOOST_LOG(debug) << "未找到虚拟显示器设备，跳过HDR状态设置";
          return true;
        }

        const std::string action = enable_hdr ? "启用" : "关闭";
        BOOST_LOG(info) << "正在" << action << "虚拟显示器 " << vdd_device_id << " 的HDR状态...";

        // 获取虚拟显示器的当前HDR状态
        std::unordered_set<std::string> vdd_device_ids = { vdd_device_id };
        auto current_hdr_states = display_device::get_current_hdr_states(vdd_device_ids);

        // 检查虚拟显示器是否支持HDR
        auto hdr_state_it = current_hdr_states.find(vdd_device_id);
        if (hdr_state_it == current_hdr_states.end()) {
          BOOST_LOG(debug) << "虚拟显示器 " << vdd_device_id << " 不支持HDR或状态未知";
          return true;
        }

        // 检查当前状态是否已经是目标状态
        hdr_state_e target_state = enable_hdr ? hdr_state_e::enabled : hdr_state_e::disabled;
        if (hdr_state_it->second == target_state) {
          BOOST_LOG(debug) << "虚拟显示器 " << vdd_device_id << " 的HDR状态已经是目标状态";
          return true;
        }

        // 创建HDR状态映射
        hdr_state_map_t new_hdr_states;
        new_hdr_states[vdd_device_id] = target_state;

        BOOST_LOG(info) << "正在" << action << "虚拟显示器 " << vdd_device_id << " 的HDR状态";

        // 设置HDR状态
        if (display_device::set_hdr_states(new_hdr_states)) {
          BOOST_LOG(info) << "成功" << action << "虚拟显示器HDR状态";
          return true;
        }
        else {
          BOOST_LOG(warning) << action << "虚拟显示器HDR状态失败";
          return false;
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "设置虚拟显示器HDR状态时发生异常: " << e.what();
        return false;
      }
    }
  }  // namespace vdd_utils
}  // namespace display_device