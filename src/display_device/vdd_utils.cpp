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

    const std::chrono::milliseconds kDefaultDebounceInterval { 2000 };

    // 上次切换显示器的时间点
    static std::chrono::steady_clock::time_point last_toggle_time { std::chrono::steady_clock::now() };
    // 防抖间隔
    static std::chrono::milliseconds debounce_interval { kDefaultDebounceInterval };
    // 上一次使用的客户端UUID，用于在没有提供UUID时使用
    static std::string last_used_client_uuid;

    namespace {
      constexpr auto kModePublicationTimeout = 3s;
      constexpr auto kModePublicationInitialPoll = 50ms;
      constexpr auto kModePublicationMaxPoll = 500ms;

      std::vector<std::string>
      collect_physical_devices_for_preservation() {
        const auto topology = get_current_topology();
        const auto devices = enum_available_devices();
        std::vector<std::string> ordered_devices;
        std::unordered_set<std::string> included;

        const auto append = [&](const std::string &device_id) {
          const auto device_it = devices.find(device_id);
          const auto friendly_name = device_it != devices.end() ?
                                       device_it->second.friendly_name :
                                       get_display_friendly_name(device_id);
          if (friendly_name != ZAKO_NAME && included.insert(device_id).second) {
            ordered_devices.push_back(device_id);
          }
        };

        // Active topology order preserves the current primary display first.
        for (const auto &group : topology) {
          for (const auto &device_id : group) {
            append(device_id);
          }
        }

        if (ordered_devices.empty()) {
          // If no topology is active, preserve the enumerated primary first,
          // followed by the remaining physical displays.
          for (const auto &[device_id, device_info] : devices) {
            if (device_info.device_state == device_state_e::primary) {
              append(device_id);
            }
          }
          for (const auto &entry : devices) {
            append(entry.first);
          }
        }

        return ordered_devices;
      }
    }  // namespace

    vdd_status_t
    get_vdd_status() {
      vdd_status_t status;
      const auto adapter = vdd_ioctl::query_adapter_status();
      status.installed = adapter.present;
      status.problem_code_valid = adapter.problem_code_valid;
      status.problem_code = adapter.problem_code;
      status.running = status.installed && status.problem_code_valid && status.problem_code == 0;
      status.control_available = status.installed && vdd_ioctl::ping();
      status.monitor_active = !display_device::find_device_by_friendlyname(ZAKO_NAME).empty();

      status.state = classify_vdd_state(
        status.installed,
        status.running,
        status.control_available,
        status.problem_code_valid,
        status.problem_code);

      return status;
    }

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

    bool
    set_hardware_cursor_enabled(bool enabled) {
      const wchar_t *command = enabled ? L"HARDWARECURSOR true" : L"HARDWARECURSOR false";

      switch (vdd_ioctl::send_command(command)) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "VDD hardware cursor " << (enabled ? "enabled" : "disabled") << " (IOCTL)";
          return true;
        case vdd_ioctl::result::failed:
          BOOST_LOG(error) << "VDD hardware cursor command was rejected by driver";
          return false;
        case vdd_ioctl::result::interface_missing:
          BOOST_LOG(error) << "VDD hardware cursor command unavailable: IOCTL interface missing";
          return false;
      }
      return false;
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
                           << " wchar_t); refusing a partial mode-list update";
        return set_vdd_result::failed;
      }

      switch (vdd_ioctl::send_command(command_string)) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "VDD live mode list updated via SETMODES: " << mode_count
                          << " modes; requested " << to_string(*config.resolution)
                          << "@" << *session_refresh_hz << "Hz";
          return set_vdd_result::ok;
        case vdd_ioctl::result::failed:
          BOOST_LOG(warning) << "VDD SETMODES IOCTL failed";
          return set_vdd_result::failed;
        case vdd_ioctl::result::interface_missing:
          BOOST_LOG(debug) << "VDD SETMODES unavailable: IOCTL interface missing";
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
      const auto status = get_vdd_status();
      if (!status.is_usable()) {
        const auto error_code = status.installed ? "VDD_DRIVER_UNAVAILABLE" : "VDD_DRIVER_NOT_INSTALLED";
        BOOST_LOG(error) << error_code << ": ZakoVDD must be installed and healthy before creating a virtual display. Open the Sunshine desktop VDD settings to install or repair it.";
        return false;
      }

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

      // 尝试通过 IOCTL 发送命令（带 GUID 或不带 GUID）
      switch (vdd_ioctl::send_command(command)) {
        case vdd_ioctl::result::success:
          tray_state::set_vdd_state(true, config::video.vdd_keep_enabled, config::video.vdd_headless_create_enabled, false);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_vdd_menu();
#endif
          BOOST_LOG(info) << "创建虚拟显示器完成 (IOCTL)";
          return true;
        case vdd_ioctl::result::failed:
          BOOST_LOG(error) << "创建虚拟显示器失败 (IOCTL)";
          return false;
        case vdd_ioctl::result::interface_missing:
          BOOST_LOG(error) << "创建虚拟显示器失败: VDD IOCTL interface missing";
          return false;
      }
      return false;
    }

    bool
    destroy_vdd_monitor() {
      // 如果VDD已不存在，直接返回成功
      if (find_device_by_friendlyname(ZAKO_NAME).empty()) {
        BOOST_LOG(debug) << "VDD设备已不存在，跳过销毁";
        return true;
      }

      switch (vdd_ioctl::send_command(L"DESTROYMONITOR")) {
        case vdd_ioctl::result::success:
          BOOST_LOG(info) << "销毁虚拟显示器完成 (IOCTL)";
          break;
        case vdd_ioctl::result::failed:
          BOOST_LOG(error) << "销毁虚拟显示器失败 (IOCTL)";
          return false;
        case vdd_ioctl::result::interface_missing:
          BOOST_LOG(error) << "销毁虚拟显示器失败: VDD IOCTL interface missing";
          return false;
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
      (void) vdd_ioctl::send_command(L"DESTROYMONITOR");
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
      const auto physical_devices_before = collect_physical_devices_for_preservation();

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
      const auto physical_devices_before = collect_physical_devices_for_preservation();

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
      std::vector<resolution_t> resolution_modes;
      std::vector<unsigned int> refresh_rates_hz;

      for (const auto &res : config::nvhttp.resolutions) {
        if (const auto parsed_resolution = parse_vdd_resolution(res)) {
          append_unique_resolution(resolution_modes, *parsed_resolution);
        }
      }

      for (const auto &fps : config::nvhttp.fps) {
        if (const auto parsed_refresh_hz = parse_vdd_refresh_hz(fps)) {
          append_unique_refresh_rate(refresh_rates_hz, *parsed_refresh_hz);
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

      return { std::move(resolution_modes), std::move(refresh_rates_hz) };
    }

    bool
    is_mode_advertised(const std::string &device_id, const display_mode_t &requested_mode) {
      if (device_id.empty() || requested_mode.refresh_rate.denominator == 0) {
        return false;
      }

      const auto devices = enum_available_devices();
      const auto device_it = devices.find(device_id);
      if (device_it == std::end(devices) || device_it->second.display_name.empty()) {
        return false;
      }

      const auto &display_name = device_it->second.display_name;
      const std::wstring wide_display_name(display_name.begin(), display_name.end());
      for (DWORD index = 0; index < 4096; ++index) {
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(wide_display_name.c_str(), index, &mode)) {
          break;
        }

        if (advertised_mode_matches(
              mode.dmPelsWidth,
              mode.dmPelsHeight,
              mode.dmDisplayFrequency,
              requested_mode)) {
          return true;
        }
      }

      return false;
    }

    bool
    wait_for_mode_publication(const std::string &device_id, const display_mode_t &requested_mode) {
      const auto deadline = std::chrono::steady_clock::now() + kModePublicationTimeout;
      auto poll_delay = kModePublicationInitialPoll;

      while (true) {
        if (is_mode_advertised(device_id, requested_mode)) {
          return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return false;
        }

        std::this_thread::sleep_for(std::min(
          poll_delay,
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        poll_delay = std::min(kModePublicationMaxPoll, poll_delay * 2);
      }
    }

    bool
    ensure_vdd_extended_mode(const std::string &device_id, const std::vector<std::string> &physical_devices_to_preserve) {
      if (device_id.empty()) {
        return false;
      }

      auto current_topology = get_current_topology();

      // 查找VDD所在的拓扑组
      std::size_t vdd_group_index = SIZE_MAX;
      for (std::size_t i = 0; i < current_topology.size(); ++i) {
        if (std::find(current_topology[i].begin(), current_topology[i].end(), device_id) != current_topology[i].end()) {
          vdd_group_index = i;
          break;
        }
      }

      if (vdd_group_index == SIZE_MAX) {
        // A cold-created IDDCX monitor can be available to DisplayConfig while
        // remaining absent from the active topology (and therefore have no
        // \\.\DISPLAYn name). Add it as an independent extended display.
        active_topology_t new_topology = current_topology;
        std::unordered_set<std::string> included;
        for (const auto &group : new_topology) {
          included.insert(group.begin(), group.end());
        }

        for (const auto &physical_id : physical_devices_to_preserve) {
          if (physical_id != device_id && included.insert(physical_id).second) {
            new_topology.push_back({ physical_id });
          }
        }
        new_topology.push_back({ device_id });

        if (!is_topology_valid(new_topology) || !set_topology(new_topology)) {
          BOOST_LOG(error) << "Failed to add inactive VDD to the extended topology";
          return false;
        }

        BOOST_LOG(info) << "Added inactive VDD to the extended topology";
        return true;
      }

      // 检查是否需要切换
      bool is_duplicated = current_topology[vdd_group_index].size() > 1;
      bool is_vdd_only = (current_topology.size() == 1 && current_topology[0].size() == 1 && current_topology[0][0] == device_id);

      if (!is_duplicated && !is_vdd_only) {
        BOOST_LOG(debug) << "VDD已经是扩展模式";
        return true;
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
        // Continue building a VDD-only topology. This is required on headless
        // systems where a cold-created monitor is not activated automatically.
        BOOST_LOG(debug) << "No physical displays to preserve; activating a VDD-only topology";
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
