#define WIN32_LEAN_AND_MEAN

#include "vdd_utils.h"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <future>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/confighttp.h"
#include "src/globals.h"
#include "src/platform/common.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "src/rtsp.h"
#include "src/system_tray.h"
#include "to_string.h"

namespace pt = boost::property_tree;

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
    // 上一次使用的客户端UUID，用于在没有提供UUID时使用
    static std::string last_used_client_uuid;

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
      bool success = execute_pipe_command(kVddPipeName, command.c_str(), &response);

      // 如果带GUID的命令失败，降级为不带GUID的命令（兼容旧版驱动）
      if (!success && !guid_str.empty()) {
        BOOST_LOG(warning) << "带GUID的命令失败，尝试降级为不带GUID的命令";
        success = execute_pipe_command(kVddPipeName, L"CREATEMONITOR", &response);
      }

      if (!success) {
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
      return !find_device_by_friendlyname(ZAKO_NAME).empty();
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

      if (is_display_on()) {
        destroy_vdd_monitor();
        return;
      }

      // since we enable the extended mode by default, we don't need to show the notice message
      // constexpr auto notice_message =
      //   L"？只不过是创建了个虚拟显示器，看到屏幕变黑就吓得手忙脚乱了吗？\n"
      //   L"杂鱼❤真是没见过世面的杂鱼大叔呢❤\n"
      //   L"这可是Windows记住了你那些乱七八糟的多显示器设置才变成这样的，这完全是正常现象哦？\n"
      //   L"真是的，别在那边丢人现眼地慌张了，看着好丢人。\n"
      //   L"听好了，本小姐大发慈悲教你这一遍——按两次 Win+P 就能变回来了。\n"
      //   L"这么简单的操作都要人教，你的脑子是装饰品吗？哼~。\n\n"
      //   L"After creating a virtual display, based on Windows' remembered multi-display combination strategy, "
      //   L"you may see a black screen which is normal. Don't panic. You can press Win+P twice to return to a visible display.";

      // if (MessageBoxW(NULL, notice_message, L"❗Zako Display Notice", MB_OKCANCEL | MB_ICONINFORMATION) == IDCANCEL) {
      //   BOOST_LOG(info) << "用户取消创建虚拟显示器";
      //   return;
      // }

      if (!create_vdd_monitor("", vdd_utils::hdr_brightness_t {}, vdd_utils::physical_size_t {})) {
        return;
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

      std::thread([vdd_device_id = find_device_by_friendlyname(ZAKO_NAME), physical_devices_before]() mutable {
        if (vdd_device_id.empty()) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          vdd_device_id = find_device_by_friendlyname(ZAKO_NAME);
        }

        if (vdd_device_id.empty()) {
          BOOST_LOG(warning) << "无法找到虚拟显示器设备，跳过配置";
        }
        else {
          BOOST_LOG(info) << "找到虚拟显示器设备: " << vdd_device_id;

          if (ensure_vdd_extended_mode(vdd_device_id, physical_devices_before)) {
            BOOST_LOG(info) << "已确保虚拟显示器处于扩展模式";
          }
        }

        constexpr auto timeout = std::chrono::seconds(20);
        constexpr auto dialog_title = L"显示器确认";
        constexpr auto confirm_message =
          L"已创建虚拟显示器，是否继续使用？\n\n"
          L"如不确认，20秒后将自动关闭显示器";

        auto future = std::async(std::launch::async, [&]() {
          return MessageBoxW(nullptr, confirm_message, dialog_title, MB_YESNO | MB_ICONQUESTION) == IDYES;
        });

        if (future.wait_for(timeout) == std::future_status::ready && future.get()) {
          BOOST_LOG(info) << "用户确认保留虚拟显示器";
          return;
        }

        BOOST_LOG(info) << "用户未确认或超时，自动销毁虚拟显示器";

        if (HWND hwnd = FindWindowW(L"#32770", dialog_title); hwnd && IsWindow(hwnd)) {
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
        if (config.refresh_rate && fps == to_string(*config.refresh_rate)) {
          is_fps_cached = true;
        }
      }

      // 如果需要更新设置
      bool needs_update = (!is_res_cached || !is_fps_cached) && config.resolution;
      if (needs_update) {
        if (!is_res_cached) {
          res_stream << to_string(*config.resolution);
        }
        if (!is_fps_cached && config.refresh_rate) {
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
        BOOST_LOG(debug) << "未找到虚拟显示器设备，跳过HDR状态设置";
        return true;
      }

      std::unordered_set<std::string> vdd_device_ids = { vdd_device_id };
      auto current_hdr_states = get_current_hdr_states(vdd_device_ids);

      auto hdr_state_it = current_hdr_states.find(vdd_device_id);
      if (hdr_state_it == current_hdr_states.end()) {
        BOOST_LOG(debug) << "虚拟显示器不支持HDR或状态未知";
        return true;
      }

      hdr_state_e target_state = enable_hdr ? hdr_state_e::enabled : hdr_state_e::disabled;
      if (hdr_state_it->second == target_state) {
        BOOST_LOG(debug) << "虚拟显示器HDR状态已是目标状态";
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
  }  // namespace vdd_utils
}  // namespace display_device