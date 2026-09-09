#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN

#include "vdd_utils.h"

#include "vdd_ioctl.h"

#include <algorithm>
#include <atomic>
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
#include <fstream>
#include <future>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/globals.h"
#include "src/file_handler.h"
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
      std::atomic_bool hardware_cursor_live_enable_confirmed { false };

      bool
      orientation_swaps_axes(DWORD orientation) {
        return orientation == DMDO_90 || orientation == DMDO_270;
      }

      unsigned int
      orientation_degrees(DWORD orientation) {
        switch (orientation) {
          case DMDO_90:
            return 90;
          case DMDO_180:
            return 180;
          case DMDO_270:
            return 270;
          case DMDO_DEFAULT:
          default:
            return 0;
        }
      }

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

    bool
    execute_vdd_disable_enable_command() {
      static const std::string kDevManPath = file_handler::path_to_utf8(
        file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR).parent_path() / "tools" / "DevManView.exe");
      static const std::string kDriverName = "Zako Display Adapter";
      static constexpr auto kAction = "disable_enable";

      boost::process::v1::environment _env = boost::this_process::environment();
      auto working_dir = boost::filesystem::path();
      std::error_code ec;

      std::string cmd = kDevManPath + " /" + kAction + " \"" + kDriverName + "\"";

      for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
        auto child = platf::run_command(true, true, cmd, working_dir, _env, nullptr, ec, nullptr);
        if (!ec) {
          BOOST_LOG(info) << "成功执行VDD " << kAction << " 命令";
          child.detach();
          return true;
        }

        auto delay = calculate_exponential_backoff(attempt);
        BOOST_LOG(warning) << "执行VDD " << kAction << " 命令失败 (尝试 "
                           << (attempt + 1) << "/" << kMaxRetryCount
                           << "): " << ec.message() << ". 将在 "
                           << delay.count() << "ms 后重试";
        std::this_thread::sleep_for(delay);
      }

      BOOST_LOG(error) << "执行VDD " << kAction << " 命令失败，已达到最大重试次数";
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
          std::ifstream input(settings_path, std::ios::binary);
          if (!input.is_open()) {
            throw std::runtime_error("unable to open VDD settings for reading");
          }
          pt::read_xml(input, root);
        }

        root.put("vdd_settings.cursor.HardwareCursor", enabled ? "true" : "false");

        auto setting = boost::property_tree::xml_writer_make_settings<std::string>(' ', 2);
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
          throw std::runtime_error("unable to open VDD settings for writing");
        }
        pt::write_xml(output, root, setting);
        output.flush();
        if (!output) {
          throw std::runtime_error("unable to write VDD settings");
        }
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Unable to persist VDD HardwareCursor setting: " << e.what();
        return false;
      }
    }

    bool
    hardware_cursor_export_enabled(std::string value) {
      boost::algorithm::trim(value);
      return boost::algorithm::iequals(value, "true") || value == "1";
    }

    bool
    ensure_hardware_cursor_enabled_for_capture(bool *changed) {
      if (changed) {
        *changed = false;
      }

      const auto settings_path = std::filesystem::path(platf::appdata()) / "vdd_settings.xml";
      bool persisted_enabled = false;

      try {
        if (std::filesystem::exists(settings_path)) {
          pt::ptree tree;
          std::ifstream input(settings_path, std::ios::binary);
          if (!input.is_open()) {
            throw std::runtime_error("unable to open VDD settings for reading");
          }
          pt::read_xml(input, tree);

          if (const auto value = tree.get_optional<std::string>("vdd_settings.cursor.HardwareCursor")) {
            persisted_enabled = hardware_cursor_export_enabled(*value);
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Unable to inspect VDD HardwareCursor setting; will request cursor export for direct capture: " << e.what();
      }

      if (!hardware_cursor_export_needs_enable(
            persisted_enabled,
            hardware_cursor_live_enable_confirmed.load(std::memory_order_acquire))) {
        BOOST_LOG(debug) << "VDD hardware cursor export is enabled in settings and confirmed by the live driver";
        return true;
      }

      BOOST_LOG(info) << "Enabling VDD hardware cursor export for direct capture";
      bool persisted = persisted_enabled;
      if (!persisted) {
        for (int attempt = 0; attempt < kMaxRetryCount; ++attempt) {
          if (persist_hardware_cursor_setting(true)) {
            persisted = true;
            break;
          }

          std::this_thread::sleep_for(calculate_exponential_backoff(attempt));
        }
      }

      if (!persisted) {
        BOOST_LOG(error) << "Failed to persist VDD hardware cursor export before reloading the driver";
        return false;
      }

      // The VDD command reloads the driver synchronously and the driver reads
      // HardwareCursor from this file during reload. Persist first so it cannot
      // observe the old value and keep cursor export disabled in memory.
      if (!set_hardware_cursor_enabled(true)) {
        hardware_cursor_live_enable_confirmed.store(false, std::memory_order_release);
        BOOST_LOG(error) << "VDD hardware cursor export is persisted, but the live driver reload failed";
        return false;
      }

      hardware_cursor_live_enable_confirmed.store(true, std::memory_order_release);
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
        std::stringstream ss(config::get_clients_config());
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
    disable_enable_vdd() {
      execute_vdd_disable_enable_command();
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
      DEVMODEW current_mode {};
      current_mode.dmSize = sizeof(current_mode);
      const bool orientation_known =
        EnumDisplaySettingsW(wide_display_name.c_str(), ENUM_CURRENT_SETTINGS, &current_mode) &&
        (current_mode.dmFields & DM_DISPLAYORIENTATION) != 0;
      const bool swaps_axes = orientation_known && orientation_swaps_axes(current_mode.dmDisplayOrientation);

      for (DWORD index = 0; index < 4096; ++index) {
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(wide_display_name.c_str(), index, &mode)) {
          break;
        }

        const auto match = classify_advertised_mode(
          mode.dmPelsWidth,
          mode.dmPelsHeight,
          mode.dmDisplayFrequency,
          requested_mode,
          swaps_axes);
        if (match == advertised_mode_match_e::rotation_equivalent) {
          BOOST_LOG(info) << "VDD mode "
                          << requested_mode.resolution.width << 'x' << requested_mode.resolution.height
                          << " is available through the current "
                          << orientation_degrees(current_mode.dmDisplayOrientation)
                          << "-degree display orientation; reusing the existing monitor";
        }
        if (match != advertised_mode_match_e::none) {
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
      const boost::optional<device_info_map_t> &pre_vdd_devices) {
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

      if (pre_vdd_devices) {
        // 使用 VDD 创建前保存的设备信息（可靠）
        for (const auto &[device_id, info] : *pre_vdd_devices) {
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

      BOOST_LOG(info) << "vdd_prep 目标拓扑: " << to_string(new_topology);

      const bool topology_applied = set_topology(new_topology);
      if (!topology_applied) {
        BOOST_LOG(error) << "设置拓扑失败";
        return false;
      }

      BOOST_LOG(info) << "成功应用vdd_prep设置";
      BOOST_LOG(debug) << "vdd_prep 执行后显示设备: " << to_string(enum_available_devices());
      return true;
    }
  }  // namespace vdd_utils
}  // namespace display_device

#else  // !_WIN32

// Linux virtual display backend (native).
//
// ZakoVDD is a Windows driver; on Linux the same session semantics are
// implemented directly: generate an EDID for the requested mode and override
// a spare connector with it via debugfs, then force the connector on so the
// compositor gains an extra output that KMS capture can target. Physical
// displays are never touched. Writing debugfs/sysfs requires elevated file
// capabilities on the sunshine binary (see the packaging install hook).

#include "vdd_utils.h"

#include <fcntl.h>
#include <sys/capability.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "src/logging.h"
#include "src/platform/linux/vdd_edid.h"

namespace display_device::vdd_utils {

  using namespace std::literals;

  namespace {
    namespace fs = std::filesystem;

    constexpr auto k_debugfs_dri = "/sys/kernel/debug/dri";

    std::mutex state_mutex;

    /**
     * @brief RAII: lift the permitted file capabilities (granted by the
     *        packaging install hook) into the effective set for the duration
     *        of the scope, restoring the previous state on destruction.
     */
    class elevated_caps {
    public:
      elevated_caps() {
        caps = cap_get_proc();
        if (!caps) {
          return;
        }
        const cap_value_t values[] = { CAP_SYS_ADMIN, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_SYS_PTRACE };
        if (cap_set_flag(caps, CAP_EFFECTIVE, 4, values, CAP_SET) || cap_set_proc(caps)) {
          BOOST_LOG(debug) << "vdd: failed to raise capabilities (unprivileged build?)"sv;
        }
      }

      ~elevated_caps() {
        if (!caps) {
          return;
        }
        const cap_value_t values[] = { CAP_SYS_ADMIN, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_SYS_PTRACE };
        if (cap_set_flag(caps, CAP_EFFECTIVE, 4, values, CAP_CLEAR) || cap_set_proc(caps)) {
          BOOST_LOG(debug) << "vdd: failed to drop capabilities"sv;
        }
        cap_free(caps);
      }

      elevated_caps(const elevated_caps &) = delete;
      elevated_caps &
      operator=(const elevated_caps &) = delete;

      cap_t caps {};
    };

    // Virtual display session state, guarded by state_mutex.
    bool active { false };
    std::string active_card;        // e.g. "card1"
    std::string active_connector;   // e.g. "DP-1"
    std::string active_status_path; // e.g. /sys/class/drm/card1-DP-1/status
    std::string active_edid_path;   // e.g. /sys/kernel/debug/dri/0000:01:00.0/DP-1/edid_override
    std::string active_edid_content;  // Last written EDID, for mode-switch dedup
    std::vector<std::string> offlined_physical_status_paths;  // Physicals powered off by display_off prep
    parsed_config_t::vdd_prep_e last_prep { parsed_config_t::vdd_prep_e::no_operation };
    unsigned int cached_width { 1920 };
    unsigned int cached_height { 1080 };
    unsigned int cached_refresh_hz { 60 };

    bool
    write_text_file(const std::string &path, const std::string &content) {
      std::ofstream out { path, std::ios::binary | std::ios::trunc };
      if (!out) {
        return false;
      }
      out << content;
      out.flush();
      return static_cast<bool>(out);
    }

    /**
     * @brief Power the physical connectors back on after an exclusive
     *        (display_off) session. Safe to call multiple times.
     */
    void
    restore_offlined_physicals() {
      for (const auto &status_path : offlined_physical_status_paths) {
        write_text_file(status_path, "on");
      }
      offlined_physical_status_paths.clear();
    }

    std::string
    connector_name_for_status(const std::string &status_path) {
      // status_path = /sys/class/drm/card1-DP-2/status
      const auto dir = status_path.substr(0, status_path.find_last_of('/'));    // /sys/class/drm/card1-DP-2
      const auto file = dir.substr(dir.find_last_of('/') + 1);                  // card1-DP-2
      return file.substr(file.find('-') + 1);                                   // DP-2
    }

    /**
     * @brief Run a command, capturing its output into the log.
     */
    void
    run_logged(const std::string &cmd) {
      std::string output;
      if (FILE *pipe = popen((cmd + " 2>&1").c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
          output += buf;
        }
        const int rc = pclose(pipe);
        BOOST_LOG(info) << "vdd: ["sv << cmd << "] exit="sv << rc << (output.empty() ? "" : " output: " + output);
      }
    }

    std::string_view
    kms_connector_prefix(std::uint32_t type) {
      switch (type) {
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP"sv;
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A"sv;
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B"sv;
        case DRM_MODE_CONNECTOR_eDP: return "eDP"sv;
        case DRM_MODE_CONNECTOR_VGA: return "VGA"sv;
        default: return "Unknown"sv;
      }
    }

    drmModeConnectorPtr
    find_connector_by_name(int fd, drmModeResPtr res, const std::string &connector) {
      for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
          continue;
        }
        const std::string name = std::string { kms_connector_prefix(conn->connector_type) } + "-" + std::to_string(conn->connector_type_id);
        if (name == connector) {
          return conn;
        }
        drmModeFreeConnector(conn);
      }
      return nullptr;
    }

    std::uint32_t
    find_free_crtc(int fd, drmModeResPtr res, const std::string &connector) {
      // A CRTC is free when no connected connector's encoder is driving it.
      (void) connector;
      for (int i = 0; i < res->count_crtcs; ++i) {
        const std::uint32_t crtc_id = res->crtcs[i];
        bool in_use = false;
        for (int j = 0; j < res->count_connectors && !in_use; ++j) {
          drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[j]);
          if (!conn) {
            continue;
          }
          if (conn->encoder_id) {
            if (drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id)) {
              in_use = enc->crtc_id == crtc_id;
              drmModeFreeEncoder(enc);
            }
          }
          drmModeFreeConnector(conn);
        }
        if (!in_use) {
          return crtc_id;
        }
      }
      return 0;
    }

    bool
    find_master_holder(const std::string &dev_path, pid_t *out_pid, int *out_fd) {
      std::vector<std::pair<pid_t, int>> candidates;
      std::error_code ec;
      for (const auto &proc : fs::directory_iterator { "/proc", ec }) {
        const auto name = proc.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
          continue;
        }
        const auto pid = static_cast<pid_t>(std::atoi(name.c_str()));
        if (pid == ::getpid()) {
          continue;
        }
        for (const auto &fd_entry : fs::directory_iterator { proc.path() / "fd", ec }) {
          std::error_code link_ec;
          const auto target = fs::read_symlink(fd_entry.path(), link_ec);
          if (link_ec || target.string() != dev_path) {
            continue;
          }
          candidates.emplace_back(pid, std::atoi(fd_entry.path().filename().c_str()));
        }
      }

      // The real DRM master is the candidate on which dropping master works.
      for (const auto &[pid, target_fd] : candidates) {
        const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd < 0) {
          continue;
        }
        const int dup_fd = static_cast<int>(::syscall(SYS_pidfd_getfd, pidfd, target_fd, 0));
        ::close(pidfd);
        if (dup_fd < 0) {
          continue;
        }
        if (drmIoctl(dup_fd, DRM_IOCTL_DROP_MASTER, nullptr) == 0) {
          drmIoctl(dup_fd, DRM_IOCTL_SET_MASTER, nullptr);  // restore while we keep looking
          BOOST_LOG(debug) << "vdd: DRM master held by pid "sv << pid << " fd "sv << target_fd;
          *out_pid = pid;
          *out_fd = target_fd;
          return true;
        }
        ::close(dup_fd);
      }
      return false;
    }

    /**
     * @brief Assign a CRTC to a connected connector: temporarily take DRM
     *        master from the compositor (pidfd borrow), create a dumb
     *        framebuffer in the connector's preferred mode and run the
     *        modeset. Compositor-agnostic. The dumb fb intentionally outlives
     *        the call - it backs the scanout of the virtual display.
     */
    bool
    force_crtc_assignment(const std::string &dev_path, const std::string &connector) {
      const int probe_fd = ::open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
      if (probe_fd < 0) {
        return false;
      }

      std::uint32_t connector_id = 0;
      std::uint32_t crtc_id = 0;
      drmModeModeInfo mode {};
      {
        drmModeResPtr res = drmModeGetResources(probe_fd);
        if (res) {
          if (drmModeConnectorPtr conn = find_connector_by_name(probe_fd, res, connector)) {
            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
              if (conn->encoder_id) {
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                ::close(probe_fd);
                return true;  // already lit
              }
              connector_id = conn->connector_id;
              mode = conn->modes[0];
              crtc_id = find_free_crtc(probe_fd, res, connector);
            }
            drmModeFreeConnector(conn);
          }
          drmModeFreeResources(res);
        }
        ::close(probe_fd);
      }

      if (!connector_id || !crtc_id) {
        return false;
      }

      pid_t holder_pid = -1;
      int holder_fd = -1;
      if (!find_master_holder(dev_path, &holder_pid, &holder_fd)) {
        BOOST_LOG(warning) << "vdd: no DRM master holder found on "sv << dev_path;
        return false;
      }

      const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, holder_pid, 0));
      if (pidfd < 0) {
        return false;
      }
      const int dup_fd = static_cast<int>(::syscall(SYS_pidfd_getfd, pidfd, holder_fd, 0));
      ::close(pidfd);
      if (dup_fd < 0) {
        return false;
      }

      // Drop the compositor's master and take over for the modeset.
      drmIoctl(dup_fd, DRM_IOCTL_DROP_MASTER, nullptr);
      const int own_fd = ::open(dev_path.c_str(), O_RDWR | O_CLOEXEC);
      bool assigned = false;
      if (own_fd >= 0 && drmIoctl(own_fd, DRM_IOCTL_SET_MASTER, nullptr) == 0) {
        drm_mode_create_dumb create {};
        create.width = mode.hdisplay;
        create.height = mode.vdisplay;
        create.bpp = 32;
        if (drmIoctl(own_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0) {
          drm_mode_fb_cmd fb_cmd {};
          fb_cmd.width = mode.hdisplay;
          fb_cmd.height = mode.vdisplay;
          fb_cmd.pitch = create.pitch;
          fb_cmd.bpp = 32;
          fb_cmd.depth = 24;
          fb_cmd.handle = create.handle;
          if (drmIoctl(own_fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) == 0) {
            assigned = drmModeSetCrtc(own_fd, crtc_id, fb_cmd.fb_id, 0, 0, &connector_id, 1, &mode) == 0;
          } else {
            drm_mode_destroy_dumb destroy {};
            destroy.handle = create.handle;
            drmIoctl(own_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
          }
        }
      }

      if (own_fd >= 0) {
        drmIoctl(own_fd, DRM_IOCTL_DROP_MASTER, nullptr);
        ::close(own_fd);
      }
      drmIoctl(dup_fd, DRM_IOCTL_SET_MASTER, nullptr);  // hand master back to the compositor
      ::close(dup_fd);

      if (assigned) {
        BOOST_LOG(info) << "vdd: assigned CRTC "sv << crtc_id << " to "sv << connector;
      } else {
        BOOST_LOG(warning) << "vdd: failed to assign a CRTC to "sv << connector;
      }
      return assigned;
    }

    /**
     * @brief Whether the named connector has been given an encoder/CRTC by
     *        the compositor. A non-master DRM open is enough to read this.
     */
    bool
    connector_has_crtc(const std::string &card, const std::string &connector) {
      const int fd = open(("/dev/dri/" + card).c_str(), O_RDWR);
      if (fd < 0) {
        return true;  // cannot verify - do not block the caller
      }

      bool has_crtc = false;
      if (drmModeResPtr res = drmModeGetResources(fd)) {
        for (int i = 0; i < res->count_connectors && !has_crtc; ++i) {
          drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
          if (!conn) {
            continue;
          }
          const std::string name = std::string { kms_connector_prefix(conn->connector_type) } + "-" + std::to_string(conn->connector_type_id);
          if (name == connector && conn->encoder_id) {
            has_crtc = true;
          }
          drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
      }
      close(fd);
      return has_crtc;
    }

    bool
    connector_status_is(const std::string &status_path, const std::string &value) {
      std::ifstream in { status_path };
      std::string current;
      std::getline(in, current);
      return current == value;
    }

    void
    for_each_connected_physical(const std::string &vd_connector, const std::function<void(const std::string &status_path, const std::string &connector)> &fn) {
      for (const auto &entry : std::filesystem::directory_iterator { "/sys/class/drm" }) {
        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') == std::string::npos) {
          continue;
        }
        const auto status_path = (entry.path() / "status").string();
        const auto connector = connector_name_for_status(status_path);
        if (connector == vd_connector) {
          continue;
        }
        if (connector_status_is(status_path, "connected")) {
          fn(status_path, connector);
        }
      }
    }

    /**
     * @brief Pick the card to host the virtual display: the one hosting the
     *        most connected outputs, falling back to the first card.
     * @return The /sys/class/drm card prefix, e.g. "card1".
     */
    std::string
    pick_card() {
      std::string best_card;
      std::size_t best_connected = 0;

      for (const auto &entry : fs::directory_iterator { "/sys/class/drm" }) {
        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) {
          continue;
        }

        std::size_t connected = 0;
        for (const auto &conn : fs::directory_iterator { "/sys/class/drm/" + name }) {
          auto conn_name = conn.path().filename().string();
          if (conn_name.rfind(name + "-", 0) != 0) {
            continue;
          }
          std::ifstream status { conn.path() / "status" };
          std::string value;
          std::getline(status, value);
          if (value == "connected") {
            ++connected;
          }
        }

        if (connected > best_connected) {
          best_connected = connected;
          best_card = name;
        }
      }

      if (best_card.empty() && fs::exists("/sys/class/drm/card0")) {
        best_card = "card0";
      }
      return best_card;
    }

    /**
     * @brief Locate the debugfs directory backing a card: the PCI name under
     *        /sys/kernel/debug/dri that hosts per-connector overrides.
     */
    std::string
    debugfs_dir_for_card(const std::string &card) {
      std::error_code ec;
      auto device = fs::canonical("/sys/class/drm/" + card + "/device", ec);
      if (ec) {
        return {};
      }

      const std::string pci_name = device.filename().string();
      if (fs::exists(fs::path { k_debugfs_dri } / pci_name)) {
        return std::string { k_debugfs_dri } + "/" + pci_name;
      }

      // Some kernels number the debugfs dri entries instead; the connector
      // scan in edid_override_path() covers that layout.
      return {};
    }

    std::string
    edid_override_path(const std::string &debugfs_dir, const std::string &connector) {
      fs::path direct = fs::path { debugfs_dir } / connector / "edid_override";
      if (fs::exists(direct)) {
        return direct.string();
      }

      // Fallback: scan every debugfs dri entry for a matching connector dir.
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator { k_debugfs_dri, ec }) {
        fs::path candidate = entry.path() / connector / "edid_override";
        if (fs::exists(candidate)) {
          return candidate.string();
        }
      }
      return {};
    }

    /**
     * @brief First disconnected hotpluggable connector on the card, DP first.
     */
    std::string
    pick_connector(const std::string &card) {
      static constexpr std::string_view preferred_types[] = { "DP", "HDMI-A", "HDMI", "eDP", "VGA" };

      std::string fallback;
      for (const auto &type : preferred_types) {
        for (const auto &entry : fs::directory_iterator { "/sys/class/drm" }) {
          const auto name = entry.path().filename().string();
          if (name.rfind(card + "-" + std::string(type) + "-", 0) != 0) {
            continue;
          }

          std::ifstream status { entry.path() / "status" };
          std::string value;
          std::getline(status, value);
          if (value != "disconnected") {
            continue;
          }
          const std::string connector = name.substr(card.size() + 1);
          if (type == "DP") {
            return connector;
          }
          if (fallback.empty()) {
            fallback = connector;
          }
        }
      }
      return fallback;
    }

    bool
    wait_for_status(const std::string &status_path, const std::string &value, int attempts, std::chrono::milliseconds delay) {
      for (int attempt = 0; attempt < attempts; ++attempt) {
        if (connector_status_is(status_path, value)) {
          return true;
        }
        std::this_thread::sleep_for(delay);
      }
      return connector_status_is(status_path, value);
    }

    /**
     * @brief Apply an EDID to the active connector and force it on. When the
     *        connector is already live with a different EDID, cycle it off
     *        first so the kernel re-reads the override.
     */
    bool
    apply_edid_and_enable(const std::string &edid_path, const std::string &status_path, const std::vector<std::uint8_t> &edid) {
      const std::string connector_name = connector_name_for_status(status_path);
      std::string current;
      {
        std::ifstream in { edid_path, std::ios::binary };
        if (in) {
          std::ostringstream buffer;
          buffer << in.rdbuf();
          current = buffer.str();
        }
      }

      const std::string new_content(reinterpret_cast<const char *>(edid.data()), edid.size());
      const bool needs_cycle = connector_status_is(status_path, "connected") && current != new_content;

      if (needs_cycle) {
        write_text_file(status_path, "off");
        wait_for_status(status_path, "disconnected", 10, std::chrono::milliseconds { 200 });
      }

      {
        std::ofstream out { edid_path, std::ios::binary | std::ios::trunc };
        if (!out) {
          BOOST_LOG(error) << "vdd: cannot open EDID override ["sv << edid_path << "]: "sv << strerror(errno);
          return false;
        }
        out.write(reinterpret_cast<const char *>(edid.data()), static_cast<std::streamsize>(edid.size()));
        out.flush();
        if (!out) {
          BOOST_LOG(error) << "vdd: cannot write EDID override ["sv << edid_path << "]"sv;
          return false;
        }
      }

      if (!write_text_file(status_path, "on")) {
        BOOST_LOG(error) << "vdd: cannot force connector on ["sv << status_path << "]: "sv << strerror(errno);
        return false;
      }

      return wait_for_status(status_path, "connected", 10, std::chrono::milliseconds { 300 });
    }
  }  // namespace

  std::string
  live_virtual_display_connector() {
    std::lock_guard lock { state_mutex };
    if (active && !active_connector.empty() && connector_status_is(active_status_path, "connected")) {
      return active_connector;
    }
    return {};
  }

  std::vector<std::string>
  offlined_physical_connectors() {
    std::lock_guard lock { state_mutex };
    std::vector<std::string> names;
    for (const auto &status_path : offlined_physical_status_paths) {
      names.emplace_back(connector_name_for_status(status_path));
    }
    return names;
  }

  const std::chrono::milliseconds kDefaultDebounceInterval { 2000 };

  bool
  is_mode_advertised(const std::string &device_id, const display_mode_t &) {
    // The generated EDID advertises exactly the requested mode once the
    // connector is live; there is no per-mode list to consult.
    return !device_id.empty() && !live_virtual_display_connector().empty();
  }

  bool
  wait_for_mode_publication(const std::string &, const display_mode_t &requested_mode) {
    elevated_caps caps;
    const auto refresh_hz = std::max(1u, (requested_mode.refresh_rate.numerator + requested_mode.refresh_rate.denominator / 2) / requested_mode.refresh_rate.denominator);

    bool applied = true;
    {
      std::lock_guard lock { state_mutex };
      cached_width = requested_mode.resolution.width;
      cached_height = requested_mode.resolution.height;
      cached_refresh_hz = refresh_hz;

      if (active) {
        // Live mode switch: cycle the connector with an EDID for the new mode.
        const auto edid = vdd_edid::generate_virtual_display_edid(cached_width, cached_height, refresh_hz, true, "Foundation VDD");
        applied = apply_edid_and_enable(active_edid_path, active_status_path, edid);
        if (applied) {
          const std::string connector = connector_name_for_status(active_status_path);
          if (!connector_has_crtc(active_card, connector)) {
            force_crtc_assignment("/dev/dri/" + active_card, connector);
          }
          run_logged("kscreen-doctor output." + connector + ".enable");
        }
      }
    }

    return applied && wait_for_status(active_status_path, "connected", 10, std::chrono::milliseconds { 300 });
  }

  vdd_status_t
  get_vdd_status() {
    elevated_caps caps;

    vdd_status_t status;
    // The native path is "installed" when the kernel exposes connector EDID
    // overrides at all. Use the error-code overload: stat may fail with
    // EACCES before the capabilities are proven to work.
    std::error_code ec;
    status.installed = fs::exists(k_debugfs_dri, ec) && !ec;
    status.running = status.installed;
    status.control_available = status.installed;
    status.monitor_active = !live_virtual_display_connector().empty();
    status.problem_code_valid = true;
    status.problem_code = 0;
    return status;
  }

  bool
  hardware_cursor_export_enabled(std::string value) {
    return value == "1" || value == "true" || value == "on";
  }

  bool
  ensure_hardware_cursor_enabled_for_capture(bool *) {
    // KMS capture reads the cursor plane directly; nothing to enable.
    return true;
  }

  set_vdd_result
  set_vdd_session_mode(const parsed_config_t &config, const VddSettings &) {
    elevated_caps caps;
    if (!config.resolution || !config.refresh_rate) {
      return set_vdd_result::invalid_config;
    }

    const auto refresh_hz = std::max(1u, (config.refresh_rate->numerator + config.refresh_rate->denominator / 2) / config.refresh_rate->denominator);
    std::lock_guard lock { state_mutex };
    cached_width = config.resolution->width;
    cached_height = config.resolution->height;
    cached_refresh_hz = refresh_hz;

    if (!active) {
      // The EDID is generated from the cached mode when the display is created.
      return set_vdd_result::ok;
    }

    const auto edid = vdd_edid::generate_virtual_display_edid(cached_width, cached_height, refresh_hz, true, "Foundation VDD");
    return apply_edid_and_enable(active_edid_path, active_status_path, edid) ? set_vdd_result::ok : set_vdd_result::failed;
  }

  std::string
  generate_client_guid(const std::string &) {
    // Per-client GUIDs are a ZakoVDD driver concept.
    return {};
  }

  physical_size_t
  get_client_physical_size(const std::string &) {
    return {};
  }

  bool
  create_vdd_monitor(const std::string &client_identifier, const hdr_brightness_t &, const physical_size_t &) {
    elevated_caps caps;
    BOOST_LOG(info) << "Creating virtual display " << cached_width << "x" << cached_height << "@" << cached_refresh_hz
                    << "Hz" << (client_identifier.empty() ? std::string {} : " (client: " + client_identifier + ")");

    std::lock_guard lock { state_mutex };

    if (active && connector_status_is(active_status_path, "connected")) {
      BOOST_LOG(debug) << "vdd: virtual display already active on "sv << active_connector;
      return true;
    }

    // Sweep virtual connectors left behind by crashed sessions or older
    // runs: any forced-on connector still carrying our VHD-signature EDID
    // is cleared so it does not accumulate and consume VRAM.
    {
      const std::uint8_t signature[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x56, 0x24 };
      for (const auto &entry : std::filesystem::directory_iterator { "/sys/class/drm" }) {
        const auto name = entry.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') == std::string::npos) {
          continue;
        }
        const std::string status_path = (entry.path() / "status").string();
        if (!connector_status_is(status_path, "connected")) {
          continue;
        }
        const std::string connector = name.substr(name.find('-') + 1);

        const auto edid_path = edid_override_path(debugfs_dir_for_card(name.substr(0, name.find('-'))), connector);
        if (edid_path.empty()) {
          continue;
        }
        std::ifstream in { edid_path, std::ios::binary };
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.size() < sizeof(signature) || std::memcmp(content.data(), signature, sizeof(signature)) != 0) {
          continue;
        }
        if (active && status_path == active_status_path) {
          continue;
        }

        BOOST_LOG(warning) << "vdd: cleaning stale virtual display on "sv << name;
        write_text_file(edid_path, "");
        write_text_file(status_path, "off");
      }
    }

    const std::string card = pick_card();
    if (card.empty()) {
      BOOST_LOG(error) << "vdd: no DRM card found"sv;
      return false;
    }

    const std::string debugfs_dir = debugfs_dir_for_card(card);
    const std::string connector = pick_connector(card);
    if (connector.empty()) {
      BOOST_LOG(error) << "vdd: no spare disconnected connector on "sv << card << " for the virtual display"sv;
      return false;
    }

    auto edid_path = edid_override_path(debugfs_dir, connector);
    if (edid_path.empty()) {
      BOOST_LOG(error) << "vdd: no EDID override node for "sv << connector << " (debugfs mounted?)"sv;
      return false;
    }

    const std::string status_path = "/sys/class/drm/" + card + "-" + connector + "/status";
    const auto edid = vdd_edid::generate_virtual_display_edid(cached_width, cached_height, cached_refresh_hz, true, "Foundation VDD");

    if (!apply_edid_and_enable(edid_path, status_path, edid)) {
      BOOST_LOG(error) << "vdd: failed to bring up "sv << card << '-' << connector;
      return false;
    }

    // NVIDIA does not emit a hotplug for a status-forced connector, so the
    // compositor sees the output but leaves it disabled with no CRTC. Assign
    // the CRTC ourselves at the DRM level (compositor-agnostic); the KDE
    // helper is only a fallback. Once the CRTC is up, enable the output so
    // the compositor starts painting it.
    bool crtc_assigned = connector_has_crtc(card, connector);
    for (int attempt = 0; attempt < 4 && !crtc_assigned; ++attempt) {
      if (force_crtc_assignment("/dev/dri/" + card, connector)) {
        crtc_assigned = true;
        break;
      }
      run_logged("kscreen-doctor output." + connector + ".enable");
      crtc_assigned = connector_has_crtc(card, connector);
      if (!crtc_assigned) {
        std::this_thread::sleep_for(std::chrono::milliseconds { 500 });
      }
    }

    if (!crtc_assigned) {
      BOOST_LOG(warning) << "vdd: compositor did not assign a CRTC to "sv << connector << " in time"sv;
    }
    else {
      run_logged("kscreen-doctor output." + connector + ".enable");
    }

    active = true;
    active_card = card;
    active_connector = connector;
    active_status_path = status_path;
    active_edid_path = edid_path;
    active_edid_content.assign(reinterpret_cast<const char *>(edid.data()), edid.size());

    BOOST_LOG(info) << "vdd: virtual display is live on "sv << card << '-' << connector;
    return true;
  }

  bool
  create_vdd_monitor_noninteractive() {
    return create_vdd_monitor("", hdr_brightness_t {}, physical_size_t {});
  }

  bool
  destroy_vdd_monitor() {
    elevated_caps caps;
    std::lock_guard lock { state_mutex };
    if (!active) {
      return true;
    }

    write_text_file(active_edid_path, "");
    write_text_file(active_status_path, "off");
    const bool gone = wait_for_status(active_status_path, "disconnected", 10, std::chrono::milliseconds { 200 });
    if (!gone) {
      BOOST_LOG(warning) << "vdd: connector "sv << active_connector << " did not turn off cleanly"sv;
    }

    restore_offlined_physicals();
    if (last_prep == parsed_config_t::vdd_prep_e::vdd_as_primary || last_prep == parsed_config_t::vdd_prep_e::vdd_as_secondary) {
      // Give the primary role back to the physical screen.
      for_each_connected_physical(active_connector, [](const auto &, const auto &connector) {
        std::system(("kscreen-doctor output." + connector + ".priority.1").c_str());
      });
    }

    BOOST_LOG(info) << "vdd: virtual display on "sv << active_connector << " destroyed"sv;
    active = false;
    active_connector.clear();
    return gone;
  }

  void
  destroy_vdd_monitor_nolog() {
    elevated_caps caps;
    std::lock_guard lock { state_mutex };
    if (!active) {
      return;
    }

    write_text_file(active_edid_path, "");
    write_text_file(active_status_path, "off");
    wait_for_status(active_status_path, "disconnected", 10, std::chrono::milliseconds { 200 });
    restore_offlined_physicals();
    active = false;
    active_connector.clear();
  }

  void
  disable_enable_vdd() {
    // Driver-level disable/enable has no native equivalent.
  }

  bool
  toggle_display_power() {
    if (is_display_on()) {
      return destroy_vdd_monitor();
    }
    return create_vdd_monitor("", hdr_brightness_t {}, physical_size_t {});
  }

  bool
  is_display_on() {
    return !live_virtual_display_connector().empty();
  }

  bool
  set_hdr_state(bool) {
    // The generated EDID advertises HDR statically; there is no runtime toggle.
    return true;
  }

  bool
  ensure_vdd_extended_mode(const std::string &, const std::vector<std::string> &) {
    return true;
  }

  bool
  apply_vdd_prep(const std::string &, parsed_config_t::vdd_prep_e vdd_prep, const boost::optional<device_info_map_t> &) {
    elevated_caps caps;
    std::lock_guard lock { state_mutex };
    last_prep = vdd_prep;

    const std::string vd_connector = active_connector;

    switch (vdd_prep) {
      case parsed_config_t::vdd_prep_e::vdd_as_primary: {
        // Mirrors the Windows topology: VDD first (primary), physicals as
        // extended. KWin models "primary" as the highest kscreen priority.
        if (!vd_connector.empty()) {
          run_logged("kscreen-doctor output." + vd_connector + ".priority.1");
          for_each_connected_physical(vd_connector, [](const auto &, const auto &connector) {
            std::system(("kscreen-doctor output." + connector + ".priority.2").c_str());
          });
        }
        return true;
      }

      case parsed_config_t::vdd_prep_e::vdd_as_secondary: {
        // Physicals stay primary, VDD extends as secondary.
        if (!vd_connector.empty()) {
          run_logged("kscreen-doctor output." + vd_connector + ".priority.2");
          for_each_connected_physical(vd_connector, [](const auto &, const auto &connector) {
            std::system(("kscreen-doctor output." + connector + ".priority.1").c_str());
          });
        }
        return true;
      }

      case parsed_config_t::vdd_prep_e::display_off: {
        // Exclusive mode: power off every connected physical connector for
        // the duration of the session. destroy_vdd_monitor() restores them.
        offlined_physical_status_paths.clear();
        for_each_connected_physical(vd_connector, [&](const auto &status_path, const auto &connector) {
          if (write_text_file(status_path, "off")) {
            offlined_physical_status_paths.emplace_back(status_path);
            BOOST_LOG(info) << "vdd: display_off prep powered off card-connector with status path "sv << status_path;
          }
        });
        return true;
      }

      case parsed_config_t::vdd_prep_e::no_operation:
      default:
        // no_operation: keep the current layout untouched. ensure_active is
        // folded here: the virtual display is enabled as part of creation.
        return true;
    }
  }


  VddSettings
  prepare_vdd_settings(const parsed_config_t &) {
    return {};
  }

}  // namespace display_device::vdd_utils
#endif
