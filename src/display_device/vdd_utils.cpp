#define WIN32_LEAN_AND_MEAN

#include "vdd_utils.h"

#include "vdd_ioctl.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/platform/run_command.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "src/rtsp.h"
#include "src/tray/system_tray.h"
#include "src/tray/system_tray_i18n.h"
#include "src/tray/tray_state.h"
#include "to_string.h"

namespace pt = boost::property_tree;

namespace display_device {
  namespace vdd_utils {

    // ===========================================================
    // [LEGACY-PIPE] Transitional named-pipe transport
    // -----------------------------------------------------------
    // The named pipe was the only way Sunshine talked to ZakoVDD
    // before the IOCTL device interface (`vdd_ioctl.cpp`) landed.
    // It is kept as a fallback so this Sunshine build still works
    // against older driver releases that don't expose the IOCTL
    // interface yet.
    //
    // Once every driver release in the wild speaks IOCTL, the
    // entire pipe code path can be removed mechanically:
    //   1. grep -nE '\[LEGACY-PIPE\]' vdd_utils.cpp and delete
    //      every tagged block (constants, helpers, fallback calls).
    //   2. Drop kVddPipeName, kPipeTimeoutMs, kPipeBufferSize,
    //      connect_to_pipe_with_retry, execute_pipe_command, the
    //      pipe-write half of destroy_vdd_monitor_nolog.
    // The IOCTL primitives in vdd_ioctl.{h,cpp} stay untouched.
    // ===========================================================

    // [LEGACY-PIPE]
    const wchar_t *kVddPipeName = L"\\\\.\\pipe\\ZakoVDDPipe";
    // [LEGACY-PIPE]
    const DWORD kPipeTimeoutMs = 3000;
    // [LEGACY-PIPE]
    const DWORD kPipeBufferSize = 4096;
    const std::chrono::milliseconds kDefaultDebounceInterval { 2000 };

    // 上次切换显示器的时间点
    static std::chrono::steady_clock::time_point last_toggle_time { std::chrono::steady_clock::now() };
    // 防抖间隔
    static std::chrono::milliseconds debounce_interval { kDefaultDebounceInterval };
    // 上一次使用的客户端UUID，用于在没有提供UUID时使用
    static std::string last_used_client_uuid;

    std::chrono::milliseconds
    calculate_exponential_backoff(int attempt) {
      auto delay = kInitialRetryDelay * (1 << attempt);
      return std::min(delay, kMaxRetryDelay);
    }

    /**
     * @brief Allowed DevManView actions for VDD driver management.
     */
    enum class vdd_action_e {
      enable,
      disable,
      disable_enable
    };

    /**
     * @brief Get the command-line argument string for a VDD action.
     */
    const char *
    vdd_action_to_string(vdd_action_e action) {
      switch (action) {
        case vdd_action_e::enable: return "enable";
        case vdd_action_e::disable: return "disable";
        case vdd_action_e::disable_enable: return "disable_enable";
        default: return nullptr;
      }
    }

    bool
    execute_vdd_command(vdd_action_e action) {
      static const std::string kDevManPath = (std::filesystem::path(SUNSHINE_ASSETS_DIR).parent_path() / "tools" / "DevManView.exe").string();
      static const std::string kDriverName = "Zako Display Adapter";

      const char *action_str = vdd_action_to_string(action);
      if (!action_str) {
        BOOST_LOG(error) << "未知的VDD命令操作";
        return false;
      }

      boost::process::v1::environment _env = boost::this_process::environment();
      auto working_dir = boost::filesystem::path();
      std::error_code ec;

      std::string cmd = kDevManPath + " /" + action_str + " \"" + kDriverName + "\"";

      for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
        auto child = platf::run_command(true, true, cmd, working_dir, _env, nullptr, ec, nullptr);
        if (!ec) {
          BOOST_LOG(info) << "成功执行VDD " << action_str << " 命令";
          child.detach();
          return true;
        }

        auto delay = calculate_exponential_backoff(attempt);
        BOOST_LOG(warning) << "执行VDD " << action_str << " 命令失败 (尝试 "
                           << (attempt + 1) << "/" << kMaxRetryCount
                           << "): " << ec.message() << ". 将在 "
                           << delay.count() << "ms 后重试";
        std::this_thread::sleep_for(delay);
      }

      BOOST_LOG(error) << "执行VDD " << action_str << " 命令失败，已达到最大重试次数";
      return false;
    }

    // [LEGACY-PIPE] entire function
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

    // [LEGACY-PIPE] entire function
    bool
    execute_pipe_command(const wchar_t *pipe_name, const wchar_t *command, std::string *response, bool *timed_out) {
      auto hPipe = connect_to_pipe_with_retry(pipe_name);
      if (hPipe == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "连接MTT虚拟显示管道失败，已重试多次";
        return false;
      }

      // RAII guard for pipe handle to prevent handle leak
      struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
          if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
      } pipe_guard { hPipe };

      // 异步IO结构体
      OVERLAPPED overlapped = { 0 };
      overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

      HandleGuard event_guard { overlapped.hEvent };

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
      bool read_timed_out = false;
      if (response) {
        char buffer[kPipeBufferSize];
        DWORD bytesRead = 0;
        if (!ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, &overlapped)) {
          if (GetLastError() != ERROR_IO_PENDING) {
            BOOST_LOG(warning) << "读取响应失败，错误代码: " << GetLastError();
            return false;
          }

          DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
          if (waitResult == WAIT_OBJECT_0 && GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE)) {
            buffer[bytesRead] = '\0';
            *response = std::string(buffer, bytesRead);
          }
          else {
            read_timed_out = true;
            CancelIo(hPipe);
          }
        }
        else {
          // ReadFile completed synchronously
          buffer[bytesRead] = '\0';
          *response = std::string(buffer, bytesRead);
        }
      }

      if (timed_out) {
        *timed_out = read_timed_out;
      }
      return true;
    }

    bool
    reload_driver() {
      // Preferred path: IOCTL device interface (PnP-wakes the driver,
      // immune to WUDFHost recycle races).
      switch (vdd_ioctl::send_command(L"RELOAD_DRIVER")) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "Reload VDD driver requested (IOCTL)";
          return true;
        case vdd_ioctl::result::failed:
          // Driver is present but rejected the IOCTL -- propagate so the
          // caller doesn't waste a ~6s pipe timeout retrying the same
          // command on a transport the driver already answered.
          BOOST_LOG(error) << "Reload VDD driver via IOCTL failed; not falling back to pipe";
          return false;
        case vdd_ioctl::result::interface_missing:
          // Driver too old to expose the IOCTL interface -- fall through
          // to the legacy pipe transport.
          break;
      }

      // [LEGACY-PIPE] Fallback for older driver builds that do not
      // expose the IOCTL device interface.
      std::string response;
      const bool ok = execute_pipe_command(kVddPipeName, L"RELOAD_DRIVER", &response);
      if (ok) {
        BOOST_LOG(info) << "Reload VDD driver requested (PIPE)";
      }
      return ok;
    }

    bool
    set_hardware_cursor_enabled(bool enabled) {
      const wchar_t *command = enabled ? L"HARDWARECURSOR true" : L"HARDWARECURSOR false";

      switch (vdd_ioctl::send_command(command)) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "VDD hardware cursor " << (enabled ? "enabled" : "disabled") << " (IOCTL)";
          return true;
        case vdd_ioctl::result::failed:
          BOOST_LOG(error) << "VDD hardware cursor command was rejected by driver; not falling back to pipe";
          return false;
        case vdd_ioctl::result::interface_missing:
          break;
      }

      std::string response;
      const bool ok = execute_pipe_command(kVddPipeName, command, &response);
      if (ok) {
        BOOST_LOG(info) << "VDD hardware cursor " << (enabled ? "enabled" : "disabled") << " (PIPE)";
      }
      return ok;
    }

    bool
    persist_hardware_cursor_setting(bool enabled) {
      const auto settings_path = std::filesystem::path(platf::appdata()) / "vdd_settings.xml";

      try {
        pt::ptree root;
        if (std::filesystem::exists(settings_path)) {
          pt::read_xml(settings_path.string(), root);
        }

        root.put("vdd_settings.cursor.HardwareCursor", enabled ? "true" : "false");

        auto setting = boost::property_tree::xml_writer_make_settings<std::string>(' ', 2);
        pt::write_xml(settings_path.string(), root, std::locale(), setting);
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Unable to persist VDD HardwareCursor setting: " << e.what();
        return false;
      }
    }

    bool
    ensure_hardware_cursor_disabled_for_capture(bool *changed) {
      if (changed) {
        *changed = false;
      }

      const auto settings_path = std::filesystem::path(platf::appdata()) / "vdd_settings.xml";
      bool needs_disable = true;

      try {
        if (std::filesystem::exists(settings_path)) {
          pt::ptree tree;
          pt::read_xml(settings_path.string(), tree);

          if (const auto value = tree.get_optional<std::string>("vdd_settings.cursor.HardwareCursor")) {
            auto hardware_cursor = *value;
            boost::algorithm::trim(hardware_cursor);
            needs_disable = !(boost::algorithm::iequals(hardware_cursor, "false") || hardware_cursor == "0");
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Unable to inspect VDD HardwareCursor setting; will request software cursor for direct capture: " << e.what();
      }

      if (!needs_disable) {
        BOOST_LOG(debug) << "VDD HardwareCursor is already disabled for direct capture";
        return true;
      }

      BOOST_LOG(info) << "Disabling VDD HardwareCursor because direct VDD capture only receives the shared framebuffer texture";
      if (!set_hardware_cursor_enabled(false)) {
        return false;
      }

      bool persisted = false;
      for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
        if (persist_hardware_cursor_setting(false)) {
          persisted = true;
          break;
        }

        std::this_thread::sleep_for(calculate_exponential_backoff(attempt));
      }

      if (!persisted) {
        BOOST_LOG(error) << "VDD HardwareCursor disabled in driver, but failed to persist vdd_settings.xml";
        return false;
      }

      if (changed) {
        *changed = true;
      }
      return true;
    }

    bool
    same_resolution(const resolution_t &a, const resolution_t &b) {
      return a.width == b.width && a.height == b.height;
    }

    void
    append_unique_resolution(std::vector<resolution_t> &resolutions, const resolution_t &resolution) {
      if (std::find_if(resolutions.begin(), resolutions.end(), [&](const auto &cached) {
            return same_resolution(cached, resolution);
          }) == resolutions.end()) {
        resolutions.push_back(resolution);
      }
    }

    void
    append_unique_refresh_rate(std::vector<unsigned int> &refresh_rates_hz, unsigned int refresh_hz) {
      if (refresh_hz > 0 && std::find(refresh_rates_hz.begin(), refresh_rates_hz.end(), refresh_hz) == refresh_rates_hz.end()) {
        refresh_rates_hz.push_back(refresh_hz);
      }
    }

    boost::optional<resolution_t>
    parse_vdd_resolution(const std::string &value) {
      std::string trimmed = value;
      boost::algorithm::trim(trimmed);
      if (trimmed.empty()) {
        return {};
      }

      std::stringstream input(trimmed);
      unsigned int width = 0;
      unsigned int height = 0;
      char separator = '\0';
      input >> width >> separator >> height;

      if (!input || !input.eof() || (separator != 'x' && separator != 'X') || width == 0 || height == 0) {
        BOOST_LOG(warning) << "Skipping invalid VDD resolution entry: " << value;
        return {};
      }

      return resolution_t { width, height };
    }

    boost::optional<unsigned int>
    rounded_vdd_refresh_hz(double refresh_hz) {
      constexpr auto max_unsigned_refresh_hz = static_cast<double>(std::numeric_limits<unsigned int>::max());
      constexpr auto max_lround_input = static_cast<double>(std::numeric_limits<long>::max());

      if (!std::isfinite(refresh_hz) || refresh_hz <= 0.0 || refresh_hz > std::min(max_unsigned_refresh_hz, max_lround_input)) {
        return {};
      }

      const auto rounded = std::lround(refresh_hz);
      if (rounded <= 0 || static_cast<unsigned long>(rounded) > std::numeric_limits<unsigned int>::max()) {
        return {};
      }

      return static_cast<unsigned int>(rounded);
    }

    boost::optional<unsigned int>
    rounded_refresh_hz(const refresh_rate_t &refresh_rate) {
      if (refresh_rate.denominator == 0) {
        return {};
      }

      const double refresh_hz = static_cast<double>(refresh_rate.numerator) / refresh_rate.denominator;
      return rounded_vdd_refresh_hz(refresh_hz);
    }

    boost::optional<unsigned int>
    parse_vdd_refresh_hz(const std::string &value) {
      std::string trimmed = value;
      boost::algorithm::trim(trimmed);
      if (trimmed.empty()) {
        return {};
      }

      try {
        std::size_t parsed_len = 0;
        const double refresh_hz = std::stod(trimmed, &parsed_len);
        if (parsed_len != trimmed.size()) {
          BOOST_LOG(warning) << "Skipping invalid VDD refresh-rate entry: " << value;
          return {};
        }

        const auto rounded = rounded_vdd_refresh_hz(refresh_hz);
        if (!rounded) {
          BOOST_LOG(warning) << "Skipping invalid VDD refresh-rate entry: " << value;
          return {};
        }

        return *rounded;
      }
      catch (const std::exception &) {
        BOOST_LOG(warning) << "Skipping invalid VDD refresh-rate entry: " << value;
        return {};
      }
    }

    set_vdd_result
    set_vdd_session_mode(const parsed_config_t &config, const VddSettings &settings) {
      if (!config.resolution || !config.refresh_rate) {
        BOOST_LOG(debug) << "SETMODES skipped: session resolution or refresh rate is not set";
        return set_vdd_result::invalid_config;
      }

      auto session_refresh_hz = rounded_refresh_hz(*config.refresh_rate);
      if (!session_refresh_hz) {
        BOOST_LOG(warning) << "SETMODES skipped: invalid refresh rate " << to_string(*config.refresh_rate);
        return set_vdd_result::invalid_config;
      }

      auto resolutions = settings.resolution_modes;
      auto refresh_rates_hz = settings.refresh_rates_hz;
      append_unique_resolution(resolutions, *config.resolution);
      append_unique_refresh_rate(refresh_rates_hz, *session_refresh_hz);

      if (resolutions.empty() || refresh_rates_hz.empty()) {
        BOOST_LOG(warning) << "SETMODES skipped: full VDD mode list is empty";
        return set_vdd_result::invalid_config;
      }

      std::wstringstream command;
      command << L"SETMODES ";
      std::size_t mode_count = 0;
      for (const auto &resolution : resolutions) {
        for (const auto refresh_hz : refresh_rates_hz) {
          if (mode_count > 0) {
            command << L",";
          }
          command << resolution.width << L"x" << resolution.height << L"x" << refresh_hz;
          ++mode_count;
        }
      }

      const auto command_string = command.str();
      if (command_string.size() >= 2048) {
        BOOST_LOG(warning) << "SETMODES command too large (" << command_string.size()
                           << " wchar_t); XML fallback will be used";
        return set_vdd_result::failed;
      }

      switch (vdd_ioctl::send_command(command_string)) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "VDD live mode list updated via SETMODES: " << mode_count
                          << " modes; requested " << to_string(*config.resolution)
                          << "@" << *session_refresh_hz << "Hz";
          return set_vdd_result::ok;
        case vdd_ioctl::result::failed:
          BOOST_LOG(warning) << "VDD SETMODES IOCTL failed; XML fallback will be used";
          return set_vdd_result::failed;
        case vdd_ioctl::result::interface_missing:
          BOOST_LOG(debug) << "VDD SETMODES unavailable: IOCTL interface missing; XML fallback will be used";
          return set_vdd_result::interface_missing;
      }

      return set_vdd_result::failed;
    }

    std::string
    generate_client_guid(const std::string &identifier) {
      if (identifier.empty()) {
        return "";
      }

      // 使用SHA1 name generator确保相同标识符生成相同GUID
      static constexpr boost::uuids::uuid ns_id {};
      const auto boost_uuid = boost::uuids::name_generator_sha1 { ns_id }(
        reinterpret_cast<const unsigned char *>(identifier.c_str()),
        identifier.size());

      return "{" + boost::uuids::to_string(boost_uuid) + "}";
    }

    /**
     * @brief 从客户端配置中获取物理尺寸
     * @param client_name 客户端名称
     * @return 物理尺寸结构，如果未找到则返回默认值（0,0）
     */
    physical_size_t
    get_client_physical_size(const std::string &client_name) {
      if (client_name.empty()) {
        return {};
      }

      // 预定义尺寸映射表
      static const std::unordered_map<std::string, physical_size_t> size_map = {
        { "small", { 13.3f, 7.5f } },  // 小型设备：约6英寸，16:9比例
        { "medium", { 34.5f, 19.4f } },  // 中型设备：约15.6英寸，16:9比例
        { "large", { 70.8f, 39.8f } }  // 大型设备：约32英寸，16:9比例
      };

      try {
        pt::ptree clientArray;
        std::stringstream ss(config::nvhttp.clients);
        pt::read_json(ss, clientArray);

        for (const auto &client : clientArray) {
          if (client.second.get<std::string>("name", "") == client_name) {
            const std::string device_size = client.second.get<std::string>("deviceSize", "medium");
            auto it = size_map.find(device_size);
            return (it != size_map.end()) ? it->second : size_map.at("medium");
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(debug) << "获取客户端物理尺寸失败: " << e.what();
      }

      return {};
    }

    bool
    create_vdd_monitor(const std::string &client_identifier, const hdr_brightness_t &hdr_brightness, const physical_size_t &physical_size) {
      std::string response;
      std::wstring command = L"CREATEMONITOR";

      // 如果没有提供UUID，使用上一次的UUID
      std::string identifier_to_use = client_identifier.empty() && !last_used_client_uuid.empty() ? last_used_client_uuid : client_identifier;

      if (identifier_to_use != client_identifier && !identifier_to_use.empty()) {
        BOOST_LOG(info) << "未提供客户端标识符，使用上一次的UUID: " << identifier_to_use;
      }

      // 生成GUID并构建命令
      std::string guid_str = generate_client_guid(identifier_to_use);
      if (!guid_str.empty()) {
        // 构建完整参数: {GUID}:[max_nits,min_nits,maxFALL][widthCm,heightCm]
        std::ostringstream param_stream;
        param_stream << guid_str << ":[" << hdr_brightness.max_nits << "," << hdr_brightness.min_nits << "," << hdr_brightness.max_full_nits << "]";

        // 如果提供了物理尺寸，添加到参数中
        if (physical_size.width_cm > 0.0f && physical_size.height_cm > 0.0f) {
          param_stream << "[" << physical_size.width_cm << "," << physical_size.height_cm << "]";
        }

        std::string param_str = param_stream.str();

        // 转换为宽字符并添加到命令
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, param_str.c_str(), -1, NULL, 0);
        if (size_needed > 0) {
          std::vector<wchar_t> param_wide(size_needed);
          MultiByteToWideChar(CP_UTF8, 0, param_str.c_str(), -1, param_wide.data(), size_needed);
          command += L" " + std::wstring(param_wide.data());
        }

        std::ostringstream log_stream;
        log_stream << "创建虚拟显示器，客户端标识符: " << identifier_to_use
                   << ", GUID: " << guid_str
                   << ", HDR亮度范围: [" << hdr_brightness.max_nits << ", " << hdr_brightness.min_nits << ", " << hdr_brightness.max_full_nits << "]";
        if (physical_size.width_cm > 0.0f && physical_size.height_cm > 0.0f) {
          log_stream << ", 物理尺寸: [" << physical_size.width_cm << "cm, " << physical_size.height_cm << "cm]";
        }
        BOOST_LOG(info) << log_stream.str();
      }

      // 如果使用了有效的UUID，更新上一次使用的UUID
      if (!identifier_to_use.empty()) {
        last_used_client_uuid = identifier_to_use;
      }

      // 尝试发送命令（带GUID或不带GUID）
      // Preferred path: IOCTL device interface. On success skip pipe entirely.
      switch (vdd_ioctl::send_command(command)) {
        case vdd_ioctl::result::success:
          tray_state::set_vdd_state(true, config::video.vdd_keep_enabled, config::video.vdd_headless_create_enabled, false);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_vdd_menu();
#endif
          BOOST_LOG(info) << "创建虚拟显示器完成 (IOCTL)";
          return true;
        case vdd_ioctl::result::failed:
          // Driver answered but rejected CREATEMONITOR -- avoid the pipe
          // retry (could double-allocate monitor state on a partially
          // applied request).
          BOOST_LOG(error) << "创建虚拟显示器失败 (IOCTL); not falling back to pipe";
          return false;
        case vdd_ioctl::result::interface_missing:
          break;
      }

      // [LEGACY-PIPE] Fallback for older driver builds.
      bool read_timed_out = false;
      bool success = execute_pipe_command(kVddPipeName, command.c_str(), &response, &read_timed_out);

      // 如果带GUID的命令失败，降级为不带GUID的命令（兼容旧版驱动）
      if (!success && !guid_str.empty()) {
        BOOST_LOG(warning) << "带GUID的命令失败，尝试降级为不带GUID的命令";
        read_timed_out = false;
        success = execute_pipe_command(kVddPipeName, L"CREATEMONITOR", &response, &read_timed_out);
      }

      if (!success) {
        BOOST_LOG(error) << "创建虚拟显示器失败";
        return false;
      }

      tray_state::set_vdd_state(true, config::video.vdd_keep_enabled, config::video.vdd_headless_create_enabled, false);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_vdd_menu();
#endif
      BOOST_LOG(info) << "创建虚拟显示器完成，响应: " << response << " [return=" << (read_timed_out ? 1 : 0) << "]";
      return true;
    }

    bool
    destroy_vdd_monitor() {
      // 如果VDD已不存在，直接返回成功
      if (find_device_by_friendlyname(ZAKO_NAME).empty()) {
        BOOST_LOG(debug) << "VDD设备已不存在，跳过销毁";
        return true;
      }

      // Preferred path: IOCTL.
      switch (vdd_ioctl::send_command(L"DESTROYMONITOR")) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "销毁虚拟显示器完成 (IOCTL)";
          break;
        case vdd_ioctl::result::failed:
          // Driver answered but DESTROYMONITOR errored -- don't redo it
          // on the pipe (state may already be torn down).
          BOOST_LOG(error) << "销毁虚拟显示器失败 (IOCTL); not falling back to pipe";
          return false;
        case vdd_ioctl::result::interface_missing: {
          // [LEGACY-PIPE] Fallback for older driver builds.
          std::string response;
          if (!execute_pipe_command(kVddPipeName, L"DESTROYMONITOR", &response)) {
            BOOST_LOG(error) << "销毁虚拟显示器失败";
            return false;
          }
          BOOST_LOG(info) << "销毁虚拟显示器完成 (PIPE)，响应: " << response;
          break;
        }
      }

      // 等待驱动程序完全卸载，避免WUDFHost.exe崩溃
      // 这是必要的，因为驱动程序卸载是异步的
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      tray_state::set_vdd_state(false, config::video.vdd_keep_enabled, config::video.vdd_headless_create_enabled, false);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_vdd_menu();
#endif
      return true;
    }

    void
    destroy_vdd_monitor_nolog() {
      // Preferred path: IOCTL. Both success and "driver said no" stop
      // here -- the latter would just churn through a stale pipe handle
      // during shutdown, which we explicitly do not want to log about.
      switch (vdd_ioctl::send_command(L"DESTROYMONITOR")) {
        case vdd_ioctl::result::success:
        case vdd_ioctl::result::failed:
          return;
        case vdd_ioctl::result::interface_missing:
          break;
      }

      // [LEGACY-PIPE] Best-effort pipe write for shutdown / process-exit
      // paths where we don't want to log a failure if the pipe is gone.
      HANDLE hPipe = CreateFileW(
        kVddPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
      if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);
        const wchar_t cmd[] = L"DESTROYMONITOR";
        DWORD bytesWritten;
        WriteFile(hPipe, cmd, sizeof(cmd), &bytesWritten, NULL);
        CloseHandle(hPipe);
      }
    }

    void
    enable_vdd() {
      execute_vdd_command(vdd_action_e::enable);
    }

    void
    disable_vdd() {
      execute_vdd_command(vdd_action_e::disable);
    }

    void
    disable_enable_vdd() {
      execute_vdd_command(vdd_action_e::disable_enable);
    }

    bool
    is_display_on() {
      return !find_device_by_friendlyname(ZAKO_NAME).empty();
    }

    bool
    create_vdd_monitor_noninteractive() {
      std::unordered_set<std::string> physical_devices_before;
      const auto topology_before = get_current_topology();
      const auto all_devices_before = enum_available_devices();

      for (const auto &group : topology_before) {
        for (const auto &device_id : group) {
          if (get_display_friendly_name(device_id) != ZAKO_NAME) {
            physical_devices_before.insert(device_id);
          }
        }
      }
      if (physical_devices_before.empty()) {
        for (const auto &[device_id, device_info] : all_devices_before) {
          if (get_display_friendly_name(device_id) != ZAKO_NAME) {
            physical_devices_before.insert(device_id);
          }
        }
      }

      if (!create_vdd_monitor("", hdr_brightness_t {}, physical_size_t {})) {
        return false;
      }

      auto vdd_device_id = find_device_by_friendlyname(ZAKO_NAME);
      if (vdd_device_id.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        vdd_device_id = find_device_by_friendlyname(ZAKO_NAME);
      }
      if (vdd_device_id.empty()) {
        BOOST_LOG(warning) << "VDD was created but its display device was not found for topology setup";
        return true;
      }

      if (!ensure_vdd_extended_mode(vdd_device_id, physical_devices_before)) {
        BOOST_LOG(warning) << "VDD was created but extended topology setup did not complete";
      }
      return true;
    }

    bool
    toggle_display_power() {
      auto now = std::chrono::steady_clock::now();

      if (now - last_toggle_time < debounce_interval) {
        BOOST_LOG(debug) << "忽略快速重复的显示器开关请求，请等待"
                         << std::chrono::duration_cast<std::chrono::seconds>(
                              debounce_interval - (now - last_toggle_time))
                              .count()
                         << "秒";
        return false;
      }

      last_toggle_time = now;

      if (is_display_on()) {
        destroy_vdd_monitor();
        return true;
      }

      // 创建前先确认
      std::wstring confirm_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_CREATE_TITLE));
      std::wstring confirm_message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_CREATE_MSG));

      if (MessageBoxW(NULL, confirm_message.c_str(), confirm_title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) == IDCANCEL) {
        BOOST_LOG(info) << system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CANCEL_CREATE_LOG);
        return false;
      }

      if (!create_vdd_monitor("", vdd_utils::hdr_brightness_t {}, vdd_utils::physical_size_t {})) {
        return false;
      }

      // 保存创建虚拟显示器前的物理设备列表
      // 同时从所有可用设备中查找物理显示器（包括可能被禁用的）
      std::unordered_set<std::string> physical_devices_before;
      auto topology_before = get_current_topology();
      auto all_devices_before = enum_available_devices();

      // 从当前拓扑中获取活动的物理设备
      for (const auto &group : topology_before) {
        for (const auto &device_id : group) {
          if (get_display_friendly_name(device_id) != ZAKO_NAME) {
            physical_devices_before.insert(device_id);
          }
        }
      }

      // 如果拓扑中没有物理设备，尝试从所有设备中查找（可能被禁用了）
      if (physical_devices_before.empty()) {
        for (const auto &[device_id, device_info] : all_devices_before) {
          if (get_display_friendly_name(device_id) != ZAKO_NAME) {
            physical_devices_before.insert(device_id);
            BOOST_LOG(debug) << "从所有设备中找到物理显示器: " << device_id;
          }
        }
      }

      // 后台线程确保VDD处于扩展模式，并进行二次确认
      std::thread([vdd_device_id = find_device_by_friendlyname(ZAKO_NAME), physical_devices_before]() mutable {
        if (vdd_device_id.empty()) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          vdd_device_id = find_device_by_friendlyname(ZAKO_NAME);
        }

        if (vdd_device_id.empty()) {
          BOOST_LOG(warning) << "无法找到基地显示器设备，跳过配置";
        }
        else {
          BOOST_LOG(info) << "找到基地显示器设备: " << vdd_device_id;

          if (ensure_vdd_extended_mode(vdd_device_id, physical_devices_before)) {
            BOOST_LOG(info) << "已确保基地显示器处于扩展模式";
          }
        }

        // 创建后二次确认，20秒超时
        constexpr auto timeout = std::chrono::seconds(20);
        std::wstring dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_KEEP_TITLE));
        std::wstring confirm_message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_KEEP_MSG));

        auto future = std::async(std::launch::async, [&]() {
          return MessageBoxW(nullptr, confirm_message.c_str(), dialog_title.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
        });

        if (future.wait_for(timeout) == std::future_status::ready && future.get()) {
          BOOST_LOG(info) << "用户确认保留基地显示器";
          return;
        }

        BOOST_LOG(info) << "用户未确认或超时，自动销毁基地显示器";

        std::wstring w_dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_KEEP_TITLE));
        if (HWND hwnd = FindWindowW(L"#32770", w_dialog_title.c_str()); hwnd && IsWindow(hwnd)) {
          PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
          PostMessage(hwnd, WM_CLOSE, 0, 0);

          for (int i = 0; i < 5 && IsWindow(hwnd); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }

          if (IsWindow(hwnd)) {
            BOOST_LOG(warning) << "无法正常关闭确认窗口，尝试终止窗口进程";
            EndDialog(hwnd, IDNO);
          }
        }

        destroy_vdd_monitor();
      }).detach();

      return true;
    }

    VddSettings
    prepare_vdd_settings(const parsed_config_t &config) {
      auto is_res_cached = false;
      auto is_fps_cached = false;
      std::ostringstream res_stream, fps_stream;
      std::vector<resolution_t> resolution_modes;
      std::vector<unsigned int> refresh_rates_hz;

      res_stream << '[';
      fps_stream << '[';

      // 检查分辨率是否已缓存
      for (const auto &res : config::nvhttp.resolutions) {
        res_stream << res << ',';
        if (const auto parsed_resolution = parse_vdd_resolution(res)) {
          append_unique_resolution(resolution_modes, *parsed_resolution);
        }
        if (config.resolution && res == to_string(*config.resolution)) {
          is_res_cached = true;
        }
      }

      // 检查帧率是否已缓存
      for (const auto &fps : config::nvhttp.fps) {
        fps_stream << fps << ',';
        if (const auto parsed_refresh_hz = parse_vdd_refresh_hz(fps)) {
          append_unique_refresh_rate(refresh_rates_hz, *parsed_refresh_hz);
        }
        if (config.refresh_rate && fps == to_string(*config.refresh_rate)) {
          is_fps_cached = true;
        }
      }

      // 如果需要更新设置
      bool needs_update = config.resolution && (!is_res_cached || (config.refresh_rate && !is_fps_cached));
      if (needs_update) {
        if (!is_res_cached) {
          res_stream << to_string(*config.resolution);
        }
        if (!is_fps_cached && config.refresh_rate) {
          fps_stream << to_string(*config.refresh_rate);
        }
      }

      if (config.resolution) {
        append_unique_resolution(resolution_modes, *config.resolution);
      }
      if (config.refresh_rate) {
        if (const auto session_refresh_hz = rounded_refresh_hz(*config.refresh_rate)) {
          append_unique_refresh_rate(refresh_rates_hz, *session_refresh_hz);
        }
      }

      // 移除最后的逗号并添加结束括号
      auto res_str = res_stream.str();
      auto fps_str = fps_stream.str();
      if (res_str.back() == ',') res_str.pop_back();
      if (fps_str.back() == ',') fps_str.pop_back();
      res_str += ']';
      fps_str += ']';

      return { res_str, fps_str, resolution_modes, refresh_rates_hz, needs_update };
    }

    bool
    ensure_vdd_extended_mode(const std::string &device_id, const std::unordered_set<std::string> &physical_devices_to_preserve) {
      if (device_id.empty()) {
        return false;
      }

      auto current_topology = get_current_topology();
      if (current_topology.empty()) {
        BOOST_LOG(warning) << "无法获取当前显示器拓扑";
        return false;
      }

      // 查找VDD所在的拓扑组
      std::size_t vdd_group_index = SIZE_MAX;
      for (std::size_t i = 0; i < current_topology.size(); ++i) {
        if (std::find(current_topology[i].begin(), current_topology[i].end(), device_id) != current_topology[i].end()) {
          vdd_group_index = i;
          break;
        }
      }

      // 检查是否需要切换
      bool is_duplicated = (vdd_group_index != SIZE_MAX && current_topology[vdd_group_index].size() > 1);
      bool is_vdd_only = (current_topology.size() == 1 && current_topology[0].size() == 1 && current_topology[0][0] == device_id);

      if (!is_duplicated && !is_vdd_only) {
        BOOST_LOG(debug) << "VDD已经是扩展模式";
        return false;
      }

      BOOST_LOG(info) << "检测到VDD处于" << (is_vdd_only ? "仅启用" : "复制") << "模式，切换到扩展模式";

      // 构建新拓扑：分离VDD，保留其他设备
      active_topology_t new_topology;
      std::unordered_set<std::string> included;

      for (std::size_t i = 0; i < current_topology.size(); ++i) {
        const auto &group = current_topology[i];

        if (i == vdd_group_index) {
          // 分离VDD到独立组
          for (const auto &id : group) {
            new_topology.push_back({ id });
            included.insert(id);
          }
        }
        else {
          for (const auto &id : group) {
            included.insert(id);
          }
          new_topology.push_back(group);
        }
      }

      // 添加缺失的物理显示器
      auto all_devices = enum_available_devices();
      for (const auto &physical_id : physical_devices_to_preserve) {
        if (included.count(physical_id) == 0 && all_devices.find(physical_id) != all_devices.end()) {
          new_topology.push_back({ physical_id });
          BOOST_LOG(info) << "添加物理显示器到拓扑: " << physical_id;
        }
      }

      if (!is_topology_valid(new_topology) || !set_topology(new_topology)) {
        BOOST_LOG(error) << "设置拓扑失败";
        return false;
      }

      BOOST_LOG(info) << "成功切换到扩展模式";
      return true;
    }

    bool
    set_hdr_state(bool enable_hdr) {
      auto vdd_device_id = find_device_by_friendlyname(ZAKO_NAME);
      if (vdd_device_id.empty()) {
        BOOST_LOG(info) << "未找到虚拟显示器设备，跳过HDR状态设置";
        return true;
      }

      std::unordered_set<std::string> vdd_device_ids = { vdd_device_id };
      auto current_hdr_states = get_current_hdr_states(vdd_device_ids);

      auto hdr_state_it = current_hdr_states.find(vdd_device_id);
      if (hdr_state_it == current_hdr_states.end()) {
        BOOST_LOG(info) << "虚拟显示器不支持HDR或状态未知";
        return true;
      }

      hdr_state_e target_state = enable_hdr ? hdr_state_e::enabled : hdr_state_e::disabled;
      if (hdr_state_it->second == target_state) {
        BOOST_LOG(info) << "虚拟显示器HDR状态已是目标状态";
        return true;
      }

      hdr_state_map_t new_hdr_states;
      new_hdr_states[vdd_device_id] = target_state;

      const std::string action = enable_hdr ? "启用" : "关闭";
      BOOST_LOG(info) << "正在" << action << "虚拟显示器HDR...";

      if (set_hdr_states(new_hdr_states)) {
        BOOST_LOG(info) << "成功" << action << "虚拟显示器HDR";
        return true;
      }

      BOOST_LOG(warning) << action << "虚拟显示器HDR失败";
      return false;
    }

    bool
    apply_vdd_prep(const std::string &vdd_device_id, parsed_config_t::vdd_prep_e vdd_prep,
      const device_info_map_t &pre_vdd_devices) {
      if (vdd_device_id.empty()) {
        BOOST_LOG(info) << "VDD设备ID为空，跳过vdd_prep处理";
        return true;
      }

      if (vdd_prep == parsed_config_t::vdd_prep_e::no_operation) {
        BOOST_LOG(info) << "vdd_prep设置为无操作，跳过物理显示器处理";
        return true;
      }

      // 从 pre_vdd_devices（VDD创建前保存的设备列表）中获取物理显示器，
      // 确保即使 VDD 创建后物理屏变 inactive 也能正确识别
      std::vector<std::string> physical_devices;
      std::string original_primary_id;
      const auto is_active_physical_display = [](const device_info_t &info) {
        return info.friendly_name != ZAKO_NAME &&
               (info.device_state == device_state_e::active ||
                info.device_state == device_state_e::primary);
      };

      if (!pre_vdd_devices.empty()) {
        // 使用 VDD 创建前保存的设备信息（可靠）
        for (const auto &[device_id, info] : pre_vdd_devices) {
          if (is_active_physical_display(info)) {
            physical_devices.push_back(device_id);
            if (info.device_state == device_state_e::primary) {
              original_primary_id = device_id;
            }
          }
        }
        BOOST_LOG(info) << "使用pre-VDD设备列表: " << physical_devices.size() << "个物理显示器"
                        << (original_primary_id.empty() ? "" : ", 原主屏: " + original_primary_id);
      }
      else {
        // 回退：从当前设备枚举中获取（VDD创建前未保存时的兜底逻辑）
        BOOST_LOG(warning) << "未提供pre-VDD设备列表，从当前设备枚举中查找物理显示器";
        const auto all_devices = enum_available_devices();
        for (const auto &[device_id, info] : all_devices) {
          if (device_id != vdd_device_id && is_active_physical_display(info)) {
            physical_devices.push_back(device_id);
            if (info.device_state == device_state_e::primary) {
              original_primary_id = device_id;
            }
          }
        }
      }

      // 确保原主屏在列表最前面（set_topology 中第一组拥有主屏优先权）
      if (!original_primary_id.empty()) {
        auto it = std::find(physical_devices.begin(), physical_devices.end(), original_primary_id);
        if (it != physical_devices.begin() && it != physical_devices.end()) {
          std::rotate(physical_devices.begin(), it, it + 1);
        }
      }

      if (physical_devices.empty()) {
        BOOST_LOG(debug) << "没有物理显示器需要处理";
        return true;
      }

      active_topology_t new_topology;

      switch (vdd_prep) {
        case parsed_config_t::vdd_prep_e::vdd_as_primary: {
          // VDD为主屏模式：VDD放在第一位（主屏），物理显示器作为扩展显示器
          BOOST_LOG(info) << "应用vdd_prep: VDD为主屏，物理显示器为副屏";
          // VDD单独一组（放在第一位作为主显示器）
          new_topology.push_back({ vdd_device_id });
          // 每个物理显示器单独一组（扩展模式）
          for (const auto &physical_id : physical_devices) {
            new_topology.push_back({ physical_id });
          }
          break;
        }

        case parsed_config_t::vdd_prep_e::vdd_as_secondary: {
          // VDD为副屏模式：物理显示器为主屏，VDD作为扩展显示器
          BOOST_LOG(info) << "应用vdd_prep: 物理显示器为主屏，VDD为副屏";
          // 物理显示器放在前面（第一个为主显示器）
          for (const auto &physical_id : physical_devices) {
            new_topology.push_back({ physical_id });
          }
          // VDD单独一组（作为副显示器）
          new_topology.push_back({ vdd_device_id });
          break;
        }

        case parsed_config_t::vdd_prep_e::display_off: {
          // 熄屏模式：只保留VDD，关闭所有物理显示器
          BOOST_LOG(info) << "应用vdd_prep: 关闭物理显示器";
          new_topology.push_back({ vdd_device_id });
          // 不添加物理显示器，它们将被禁用
          break;
        }

        default:
          return true;
      }

      if (!is_topology_valid(new_topology)) {
        BOOST_LOG(error) << "新拓扑无效";
        return false;
      }

      if (!set_topology(new_topology)) {
        BOOST_LOG(error) << "设置拓扑失败";
        return false;
      }

      BOOST_LOG(info) << "成功应用vdd_prep设置";
      return true;
    }
  }  // namespace vdd_utils
}  // namespace display_device
