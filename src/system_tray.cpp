/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #include <commdlg.h>  // 添加文件对话框支持
    #include <shellapi.h>  // 添加 ShellExecuteW 函数声明
    #include <shlobj.h>  // 添加 SHGetFolderPathW 函数声明
    #include <shobjidl.h>  // 添加 IFileDialog COM接口声明
    #include <tlhelp32.h>
    #include <windows.h>
    #define TRAY_ICON WEB_DIR "images/sunshine.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux)
    #define TRAY_ICON "sunshine-tray"
    #define TRAY_ICON_PLAYING "sunshine-playing"
    #define TRAY_ICON_PAUSING "sunshine-pausing"
    #define TRAY_ICON_LOCKED "sunshine-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-sunshine-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked-16.png"
    #include <dispatch/dispatch.h>
  #endif

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <csignal>
  #include <ctime>
  #include <fstream>
  #include <future>
  #include <string>
  #include <thread>

  // lib includes
  #include "tray/src/tray.h"
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>

  // local includes
  #include "confighttp.h"
  #include "display_device/session.h"
  #include "file_handler.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "platform/windows/misc.h"
  #include "process.h"
  #include "src/display_device/display_device.h"
  #include "src/entry_handler.h"
  #include "src/globals.h"
  #include "system_tray_i18n.h"
  #include "version.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic<bool> tray_initialized = false;

  // Threading variables for all platforms
  static std::thread tray_thread;
  static std::atomic tray_thread_running = false;
  static std::atomic tray_thread_should_exit = false;
  static std::atomic<bool> end_tray_called = false;

  // 前向声明全局变量
  extern struct tray_menu tray_menus[];
  extern struct tray tray;

  // 静态字符串变量用于存储本地化的菜单文本
  // 这些变量必须是静态的，以确保在 tray_menus 的生命周期内有效
  static std::string s_open_sunshine;
  static std::string s_vdd_base_display;
  static std::string s_vdd_create;
  static std::string s_vdd_close;
  static std::string s_vdd_persistent;
  static std::string s_import_config;
  static std::string s_export_config;
  static std::string s_reset_to_default;
  static std::string s_language;
  static std::string s_chinese;
  static std::string s_english;
  static std::string s_japanese;
  static std::string s_star_project;
  static std::string s_visit_project;
  static std::string s_visit_project_sunshine;
  static std::string s_visit_project_moonlight;
  static std::string s_advanced_settings;
  static std::string s_close_app;
  static std::string s_reset_display_device_config;
  static std::string s_restart;

  static bool s_vdd_in_cooldown = false;
  static std::string s_quit;

  // 用于存储子菜单的静态数组
  static struct tray_menu vdd_submenu[4];
  static struct tray_menu advanced_settings_submenu[7];
  static struct tray_menu visit_project_submenu[3];

  // 更新高级设置菜单项的文本
  static void update_advanced_settings_menu_text() {
    advanced_settings_submenu[0].text = s_import_config.c_str();
    advanced_settings_submenu[1].text = s_export_config.c_str();
    advanced_settings_submenu[2].text = s_reset_to_default.c_str();
    advanced_settings_submenu[3].text = "-";
    advanced_settings_submenu[4].text = s_close_app.c_str();
    advanced_settings_submenu[5].text = s_reset_display_device_config.c_str();
  }

  // 更新 VDD 子菜单项的文本
  static void update_vdd_submenu_text() {
    vdd_submenu[0].text = s_vdd_create.c_str();
    vdd_submenu[1].text = s_vdd_close.c_str();
    vdd_submenu[2].text = s_vdd_persistent.c_str();
  }

  // 更新访问项目地址子菜单项的文本
  static void tray_visit_project_submenu_text() {
    visit_project_submenu[0].text = s_visit_project_sunshine.c_str();
    visit_project_submenu[1].text = s_visit_project_moonlight.c_str();
  }

  // 初始化本地化字符串
  void
  init_localized_strings() {
    s_open_sunshine = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_OPEN_SUNSHINE);
    s_vdd_base_display = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_BASE_DISPLAY);
    s_vdd_create = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CREATE);
    s_vdd_close = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CLOSE);
    s_vdd_persistent = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_PERSISTENT);
    s_import_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_CONFIG);
    s_export_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_CONFIG);
    s_reset_to_default = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_TO_DEFAULT);
    s_language = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_LANGUAGE);
    s_chinese = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CHINESE);
    s_english = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ENGLISH);
    s_japanese = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_JAPANESE);
    s_star_project = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STAR_PROJECT);
    s_visit_project = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VISIT_PROJECT);
    s_visit_project_sunshine = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VISIT_PROJECT_SUNSHINE);
    s_visit_project_moonlight = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VISIT_PROJECT_MOONLIGHT);
    s_advanced_settings = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ADVANCED_SETTINGS);
    s_close_app = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CLOSE_APP);
    s_reset_display_device_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_DISPLAY_DEVICE_CONFIG);
    s_restart = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESTART);
    s_quit = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_QUIT);
  }

  // 更新所有菜单项的文本
  void
  update_menu_texts() {
    init_localized_strings();
    tray_menus[0].text = s_open_sunshine.c_str();
    tray_menus[2].text = s_vdd_base_display.c_str();
    update_vdd_submenu_text();  // 更新 VDD 子菜单文本
  #ifdef _WIN32
    tray_menus[3].text = s_advanced_settings.c_str();
    update_advanced_settings_menu_text();
  #endif
    tray_menus[5].text = s_language.c_str();
    tray_menus[5].submenu[0].text = s_chinese.c_str();
    tray_menus[5].submenu[1].text = s_english.c_str();
    tray_menus[5].submenu[2].text = s_japanese.c_str();
    tray_menus[7].text = s_star_project.c_str();
    tray_menus[8].text = s_visit_project.c_str();
    tray_visit_project_submenu_text();
  #ifdef _WIN32
    tray_menus[10].text = s_restart.c_str();
    tray_menus[11].text = s_quit.c_str();
  #else
    tray_menus[9].text = s_restart.c_str();
    tray_menus[10].text = s_quit.c_str();
  #endif
  }

  auto tray_open_ui_cb = [](struct tray_menu *item) {
    BOOST_LOG(debug) << "Opening UI from system tray"sv;
    launch_ui();
  };

  // 检查 VDD 是否存在
  static bool is_vdd_active() {
    auto vdd_device_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
    return !vdd_device_id.empty();
  }

  // 更新 VDD 菜单项的文本和状态
  static void update_vdd_menu_text() {
    bool vdd_active = is_vdd_active();
    bool keep_enabled = config::video.vdd_keep_enabled;
    
    // 1. 创建项：启用即勾选，启用后或冷却中禁止点击
    vdd_submenu[0].checked = vdd_active ? 1 : 0;
    vdd_submenu[0].disabled = (vdd_active || s_vdd_in_cooldown) ? 1 : 0;
    
    // 2. 关闭项：未启用即勾选，未启用、冷却中或保持启用模式下禁止点击
    vdd_submenu[1].checked = vdd_active ? 0 : 1;
    vdd_submenu[1].disabled = (!vdd_active || s_vdd_in_cooldown || keep_enabled) ? 1 : 0;
    
    // 3. 保持启用项
    vdd_submenu[2].checked = keep_enabled ? 1 : 0;
  }

  // 启动统一的 10 秒冷却
  static void start_vdd_cooldown() {
    s_vdd_in_cooldown = true;
    update_vdd_menu_text();
    tray_update(&tray);

    std::thread([&]() {
      std::this_thread::sleep_for(10s);
      s_vdd_in_cooldown = false;
      update_vdd_menu_text();
      tray_update(&tray);
    }).detach();
  }

  // 创建虚拟显示器回调
  auto tray_vdd_create_cb = [](struct tray_menu *item) {
    if (!tray_initialized) return;
    if (s_vdd_in_cooldown || is_vdd_active()) return;

    BOOST_LOG(info) << "Creating VDD from system tray (Separate Item)"sv;
    if (display_device::session_t::get().toggle_display_power()) {
      start_vdd_cooldown();
    }
  };

  // 关闭虚拟显示器回调
  auto tray_vdd_destroy_cb = [](struct tray_menu *item) {
    if (!tray_initialized) return;
    if (s_vdd_in_cooldown || !is_vdd_active() || config::video.vdd_keep_enabled) return;

    BOOST_LOG(info) << "Closing VDD from system tray (Separate Item)"sv;
    display_device::session_t::get().destroy_vdd_monitor();
    start_vdd_cooldown();
  };

  // 保持启用回调
  auto tray_vdd_persistent_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Toggling persistent VDD from system tray"sv;
    
    bool is_persistent = config::video.vdd_keep_enabled;
    
    if (!is_persistent) {
      // 启用保持启用模式前弹出确认
#ifdef _WIN32
      std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_PERSISTENT_CONFIRM_TITLE));
      std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_PERSISTENT_CONFIRM_MSG));

      if (MessageBoxW(NULL, message.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
        BOOST_LOG(info) << "User cancelled enabling VDD keep-enabled mode";
        return;
      }
#endif
      config::video.vdd_keep_enabled = true;
      BOOST_LOG(info) << "Enabled VDD keep-enabled mode (Auto-creation removed)";
    } else {
      // 禁用保持启用模式，但不自动关闭 VDD
      config::video.vdd_keep_enabled = false;
      BOOST_LOG(info) << "Disabled VDD keep-enabled mode (VDD remains if active)";
    }
    
    // 保存配置到文件
    config::update_config({{"vdd_keep_enabled", config::video.vdd_keep_enabled ? "true" : "false"}});
    
    update_vdd_menu_text();
    tray_update(&tray);
  };

  auto tray_close_app_cb = [](struct tray_menu *item) {
    if (!tray_initialized) return;

  #ifdef _WIN32
    std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CLOSE_APP_CONFIRM_TITLE));
    std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CLOSE_APP_CONFIRM_MSG));

    int msgboxID = MessageBoxW(
      NULL,
      message.c_str(),
      title.c_str(),
      MB_ICONQUESTION | MB_YESNO);

    if (msgboxID == IDYES) {
      BOOST_LOG(info) << "Clearing cache (terminating application) from system tray"sv;
      proc::proc.terminate();
    }
    else {
      BOOST_LOG(info) << "User cancelled clearing cache"sv;
    }
  #else
    // 非 Windows 平台，直接关闭
    BOOST_LOG(info) << "Closing application from system tray"sv;
    proc::proc.terminate();
  #endif
  };

  auto tray_reset_display_device_config_cb = [](struct tray_menu *item) {
    if (!tray_initialized) return;

  #ifdef _WIN32
    std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_DISPLAY_CONFIRM_TITLE));
    std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_DISPLAY_CONFIRM_MSG));

    int msgboxID = MessageBoxW(
      NULL,
      message.c_str(),
      title.c_str(),
      MB_ICONWARNING | MB_YESNO);

    if (msgboxID == IDYES) {
      BOOST_LOG(info) << "Resetting display device config from system tray"sv;
      display_device::session_t::get().reset_persistence();
    }
    else {
      BOOST_LOG(info) << "User cancelled resetting display device config"sv;
    }
  #else
    // 非 Windows 平台，直接重置
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;
    display_device::session_t::get().reset_persistence();
  #endif
  };

  auto tray_restart_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;
    platf::restart();
  };

  auto terminate_gui_processes = []() {
  #ifdef _WIN32
    BOOST_LOG(info) << "Terminating sunshine-gui.exe processes..."sv;

    // Find and terminate sunshine-gui.exe processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe32;
      pe32.dwSize = sizeof(PROCESSENTRY32W);

      if (Process32FirstW(snapshot, &pe32)) {
        do {
          // Check if this process is sunshine-gui.exe
          if (wcscmp(pe32.szExeFile, L"sunshine-gui.exe") == 0) {
            BOOST_LOG(info) << "Found sunshine-gui.exe (PID: " << pe32.th32ProcessID << "), terminating..."sv;

            // Open process handle
            HANDLE process_handle = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
            if (process_handle != NULL) {
              // Terminate the process
              if (TerminateProcess(process_handle, 0)) {
                BOOST_LOG(info) << "Successfully terminated sunshine-gui.exe"sv;
              }
              CloseHandle(process_handle);
            }
          }
        } while (Process32NextW(snapshot, &pe32));
      }
      CloseHandle(snapshot);
    }
  #else
    // For non-Windows platforms, this is a no-op
    BOOST_LOG(debug) << "GUI process termination not implemented for this platform"sv;
  #endif
  };

  auto tray_quit_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // Get localized strings
    std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_QUIT_TITLE));
    std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_QUIT_MESSAGE));

    int msgboxID = MessageBoxW(
      NULL,
      message.c_str(),
      title.c_str(),
      MB_ICONQUESTION | MB_YESNO);

    if (msgboxID == IDYES) {
      // First, terminate sunshine-gui.exe if it's running
      terminate_gui_processes();

      // If running as service (no console window), use ERROR_SHUTDOWN_IN_PROGRESS
      // Otherwise, use exit code 0 to prevent service restart
      if (GetConsoleWindow() == NULL) {
        lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      }
      else {
        lifetime::exit_sunshine(0, false);
      }
      return;
    }
  #else
    // For non-Windows platforms, just exit normally
    lifetime::exit_sunshine(0, true);
  #endif
  };

  auto tray_star_project_cb = [](struct tray_menu *item) {
    platf::open_url_in_browser("https://sunshine-foundation.vercel.app/");
  };

  auto tray_visit_project_sunshine_cb = [](struct tray_menu *item) {
    platf::open_url_in_browser("https://github.com/qiin2333/Sunshine-Foundation");
  };

  auto tray_visit_project_moonlight_cb = [](struct tray_menu *item) {
    platf::open_url_in_browser("https://github.com/qiin2333/moonlight-vplus");
  };


  // 文件对话框打开标志
  static bool file_dialog_open = false;

  // 安全验证：检查文件路径是否安全
  auto is_safe_config_path = [](const std::string &path) -> bool {
    try {
      std::filesystem::path p(path);

      // 检查文件是否存在
      if (!std::filesystem::exists(p)) {
        BOOST_LOG(warning) << "[tray_check_config] File does not exist: " << path;
        return false;
      }

      // 规范化路径（解析符号链接和..）
      std::filesystem::path canonical_path = std::filesystem::canonical(p);

      // 检查文件扩展名
      if (canonical_path.extension() != ".conf") {
        BOOST_LOG(warning) << "[tray_check_config] Invalid file extension: " << canonical_path.extension().string();
        return false;
      }

      // 防止符号链接攻击
      if (std::filesystem::is_symlink(p)) {
        BOOST_LOG(warning) << "[tray_check_config] Symlink not allowed: " << path;
        return false;
      }

      // 确保是常规文件
      if (!std::filesystem::is_regular_file(canonical_path)) {
        BOOST_LOG(warning) << "[tray_check_config] Not a regular file: " << path;
        return false;
      }

      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "[tray_check_config] Path validation error: " << e.what();
      return false;
    }
  };

  // 安全验证：检查配置文件内容是否安全
  auto is_safe_config_content = [](const std::string &content) -> bool {
    // 检查文件大小（最大1MB）
    const size_t MAX_CONFIG_SIZE = 1024 * 1024;
    if (content.size() > MAX_CONFIG_SIZE) {
      BOOST_LOG(warning) << "[tray_check_config] Config file too large: " << content.size() << " bytes";
      return false;
    }

    // 检查是否为空
    if (content.empty()) {
      BOOST_LOG(warning) << "[tray_check_config] Config file is empty";
      return false;
    }

    // 基本格式验证：尝试解析配置
    try {
      auto vars = config::parse_config(content);
      // 如果解析成功，说明格式基本正确
      BOOST_LOG(debug) << "[tray_check_config] Config validation passed, " << vars.size() << " entries found";
      return true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "[tray_check_config] Config parsing failed: " << e.what();
      return false;
    }
  };


  // 配置导入功能
  auto tray_import_config_cb = [](struct tray_menu *item) {
    if (file_dialog_open) {
      BOOST_LOG(warning) << "[tray_import_config] A file dialog is already open";
      return;
    }
    file_dialog_open = true;
    auto clear_flag = util::fail_guard([&]() {
      file_dialog_open = false;
    });

    BOOST_LOG(info) << "[tray_import_config] Importing configuration from system tray"sv;

  #ifdef _WIN32
    std::wstring file_path_wide;
    bool file_selected = false;

    // 直接显示文件对话框
    auto show_file_dialog = [&]() {
      // 初始化COM
      HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      bool com_initialized = SUCCEEDED(hr);
      auto com_cleanup = util::fail_guard([com_initialized]() {
        if (com_initialized) {
          CoUninitialize();
        }
      });
      
      IFileOpenDialog *pFileOpen = nullptr;
      hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[tray_import_config] Failed to create IFileOpenDialog: " << hr;
        return;
      }
      auto dialog_cleanup = util::fail_guard([pFileOpen]() {
        pFileOpen->Release();
      });
      
      // 设置对话框选项
      // FOS_FORCEFILESYSTEM: 强制只使用文件系统
      // FOS_DONTADDTORECENT: 不添加到最近文件列表
      // FOS_NOCHANGEDIR: 不改变当前工作目录
      // FOS_HIDEPINNEDPLACES: 隐藏固定的位置（导航面板中的快速访问等）
      // FOS_NOVALIDATE: 不验证文件路径（避免访问不存在的系统路径）
      DWORD dwFlags;
      pFileOpen->GetOptions(&dwFlags);
      pFileOpen->SetOptions(dwFlags | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | 
                            FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | 
                            FOS_NOCHANGEDIR | FOS_HIDEPINNEDPLACES | FOS_NOVALIDATE);
      
      // 设置文件类型过滤器
      std::wstring config_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_CONFIG_FILES));
      COMDLG_FILTERSPEC fileTypes[] = {
        { config_files.c_str(), L"*.conf" },
        { L"All Files", L"*.*" }
      };
      pFileOpen->SetFileTypes(2, fileTypes);
      pFileOpen->SetFileTypeIndex(1);
      
      // 设置对话框标题
      std::wstring dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_SELECT_IMPORT));
      pFileOpen->SetTitle(dialog_title.c_str());
      
      // 设置默认打开路径为应用程序配置目录
      IShellItem *psiDefault = NULL;
      std::wstring default_path = platf::appdata().wstring();
      hr = SHCreateItemFromParsingName(default_path.c_str(), NULL, IID_PPV_ARGS(&psiDefault));
      if (SUCCEEDED(hr)) {
        pFileOpen->SetFolder(psiDefault);
        psiDefault->Release();
      }
      
      // 手动添加驱动器到导航栏 (因为使用了 FOS_HIDEPINNEDPLACES)
      DWORD dwSize = GetLogicalDriveStringsW(0, NULL);
      if (dwSize > 0) {
        std::vector<wchar_t> buffer(dwSize + 1);
        if (GetLogicalDriveStringsW(dwSize, buffer.data())) {
          wchar_t* pDrive = buffer.data();
          while (*pDrive) {
            IShellItem *psiDrive = NULL;
            HRESULT hrDrive = SHCreateItemFromParsingName(pDrive, NULL, IID_PPV_ARGS(&psiDrive));
            if (SUCCEEDED(hrDrive)) {
              pFileOpen->AddPlace(psiDrive, FDAP_BOTTOM);
              psiDrive->Release();
            }
            pDrive += wcslen(pDrive) + 1;
          }
        }
      }
      
      // 添加"此电脑"到导航栏顶部
      IShellItem *psiComputer = NULL;
      hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiComputer));
      if (SUCCEEDED(hr)) {
        pFileOpen->AddPlace(psiComputer, FDAP_TOP);
        psiComputer->Release();
      }
      
      // 添加"网络"到导航栏
      IShellItem *psiNetwork = NULL;
      hr = SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiNetwork));
      if (SUCCEEDED(hr)) {
        pFileOpen->AddPlace(psiNetwork, FDAP_BOTTOM);
        psiNetwork->Release();
      }
      
      // 显示对话框
      hr = pFileOpen->Show(NULL);
      if (SUCCEEDED(hr)) {
        // 获取选择的文件
        IShellItem *pItem = nullptr;
        hr = pFileOpen->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
          PWSTR pszFilePath = nullptr;
          hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
          if (SUCCEEDED(hr)) {
            file_path_wide = pszFilePath;
            file_selected = true;
            CoTaskMemFree(pszFilePath);
          }
          pItem->Release();
        }
      }
      else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        BOOST_LOG(info) << "[tray_import_config] User cancelled file dialog"sv;
      }
      else {
        BOOST_LOG(warning) << "[tray_import_config] File dialog failed: 0x" << std::hex << hr << std::dec;
      }
    };

    // 直接显示文件对话框
    show_file_dialog();

    if (file_selected) {
      std::string file_path = platf::to_utf8(file_path_wide);

      // 安全验证：检查文件路径
      if (!is_safe_config_path(file_path)) {
        BOOST_LOG(error) << "[tray_import_config] Config import rejected: unsafe file path: " << file_path;
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
        std::wstring message = L"文件路径不安全或文件类型无效。\n只允许 .conf 文件，不允许符号链接。";
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return;
      }

      try {
        // 读取配置文件内容
        std::string config_content = file_handler::read_file(file_path.c_str());
        
        // 安全验证：检查配置内容
        if (!is_safe_config_content(config_content)) {
          BOOST_LOG(error) << "[tray_import_config] Config import rejected: unsafe content: " << file_path;
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
          std::wstring message = L"配置文件内容无效、太大或格式错误。\n最大文件大小：1MB";
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        // 备份当前配置（检查是否成功）
        std::string backup_path = config::sunshine.config_file + ".backup";
        std::string current_config = file_handler::read_file(config::sunshine.config_file.c_str());
        int backup_result = file_handler::write_file(backup_path.c_str(), current_config);
        
        if (backup_result != 0) {
          BOOST_LOG(error) << "[tray_import_config] Failed to create backup, aborting import";
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
          std::wstring message = L"无法创建配置备份，导入操作已中止。";
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        BOOST_LOG(info) << "[tray_import_config] Config backup created: " << backup_path;

        // 使用临时文件确保原子性写入
        std::string temp_path = config::sunshine.config_file + ".tmp";
        int temp_result = file_handler::write_file(temp_path.c_str(), config_content);
        
        if (temp_result != 0) {
          BOOST_LOG(error) << "[tray_import_config] Failed to write temporary config file";
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_WRITE));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        // 原子性替换：重命名临时文件为实际配置文件
        try {
          std::filesystem::rename(temp_path, config::sunshine.config_file);
          BOOST_LOG(info) << "[tray_import_config] Configuration imported successfully from: " << file_path;
          
          // 询问用户是否重启Sunshine以应用新配置
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_SUCCESS_TITLE));
          std::wstring message = L"配置导入成功！\n\n是否立即重启 Sunshine 以应用新配置？";
          int result = MessageBoxW(NULL, message.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION);
          
          if (result == IDYES) {
            BOOST_LOG(info) << "[tray_import_config] User chose to restart Sunshine"sv;
            // 重启Sunshine
            platf::restart();
          }
          else {
            BOOST_LOG(info) << "[tray_import_config] User chose not to restart Sunshine"sv;
          }
        }
        catch (const std::exception &e) {
          BOOST_LOG(error) << "[tray_import_config] Failed to rename temp file: " << e.what();
          // 清理临时文件
          std::filesystem::remove(temp_path);
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_WRITE));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "[tray_import_config] Exception during config import: " << e.what();
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_EXCEPTION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
    }
  #else
    // 非Windows平台的实现（可以后续添加）
    BOOST_LOG(info) << "[tray_import_config] Config import not implemented for this platform yet";
  #endif
  };

  // 配置导出功能
  auto tray_export_config_cb = [](struct tray_menu *item) {
    if (file_dialog_open) {
      BOOST_LOG(warning) << "[tray_export_config] A file dialog is already open";
      return;
    }
    file_dialog_open = true;
    auto clear_flag = util::fail_guard([&]() {
      file_dialog_open = false;
    });

    BOOST_LOG(info) << "[tray_export_config] Exporting configuration from system tray"sv;

  #ifdef _WIN32
    std::wstring file_path_wide;
    bool file_selected = false;

    // 直接显示文件对话框
    auto show_file_dialog = [&]() {
      // 初始化COM
      HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      bool com_initialized = SUCCEEDED(hr);
      auto com_cleanup = util::fail_guard([com_initialized]() {
        if (com_initialized) {
          CoUninitialize();
        }
      });

      IFileSaveDialog *pFileSave = nullptr;
      hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void**>(&pFileSave));
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[tray_export_config] Failed to create IFileSaveDialog: " << hr;
        return;
      }
      auto dialog_cleanup = util::fail_guard([pFileSave]() {
        pFileSave->Release();
      });

      // 设置对话框选项
      // FOS_FORCEFILESYSTEM: 强制只使用文件系统
      // FOS_DONTADDTORECENT: 不添加到最近文件列表
      // FOS_NOCHANGEDIR: 不改变当前工作目录
      // FOS_HIDEPINNEDPLACES: 隐藏固定的位置（导航面板中的快速访问等）
      // FOS_NOVALIDATE: 不验证文件路径（避免访问不存在的系统路径）
      DWORD dwFlags;
      pFileSave->GetOptions(&dwFlags);
      pFileSave->SetOptions(dwFlags | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | 
                            FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | 
                            FOS_NOCHANGEDIR | FOS_HIDEPINNEDPLACES | FOS_NOVALIDATE);

      // 设置文件类型过滤器
      std::wstring config_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_CONFIG_FILES));
      COMDLG_FILTERSPEC fileTypes[] = {
        { config_files.c_str(), L"*.conf" },
        { L"All Files", L"*.*" }
      };
      pFileSave->SetFileTypes(2, fileTypes);
      pFileSave->SetFileTypeIndex(1);
      pFileSave->SetDefaultExtension(L"conf");

      // 设置对话框标题
      std::wstring dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_SAVE_EXPORT));
      pFileSave->SetTitle(dialog_title.c_str());

      // 设置默认文件名
      std::string default_name = "sunshine_config_" + std::to_string(std::time(nullptr)) + ".conf";
      std::wstring wdefault_name(default_name.begin(), default_name.end());
      pFileSave->SetFileName(wdefault_name.c_str());

      // 设置默认保存路径为应用程序配置目录
      IShellItem *psiDefault = NULL;
      std::wstring default_path = platf::appdata().wstring();
      hr = SHCreateItemFromParsingName(default_path.c_str(), NULL, IID_PPV_ARGS(&psiDefault));
      if (SUCCEEDED(hr)) {
        pFileSave->SetFolder(psiDefault);
        psiDefault->Release();
      }

      // 手动添加驱动器到导航栏 (因为使用了 FOS_HIDEPINNEDPLACES)
      DWORD dwSize = GetLogicalDriveStringsW(0, NULL);
      if (dwSize > 0) {
        std::vector<wchar_t> buffer(dwSize + 1);
        if (GetLogicalDriveStringsW(dwSize, buffer.data())) {
          wchar_t* pDrive = buffer.data();
          while (*pDrive) {
            IShellItem *psiDrive = NULL;
            HRESULT hrDrive = SHCreateItemFromParsingName(pDrive, NULL, IID_PPV_ARGS(&psiDrive));
            if (SUCCEEDED(hrDrive)) {
              pFileSave->AddPlace(psiDrive, FDAP_BOTTOM);
              psiDrive->Release();
            }
            pDrive += wcslen(pDrive) + 1;
          }
        }
      }
      
      // 添加"此电脑"到导航栏顶部
      IShellItem *psiComputer = NULL;
      hr = SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiComputer));
      if (SUCCEEDED(hr)) {
        pFileSave->AddPlace(psiComputer, FDAP_TOP);
        psiComputer->Release();
      }

      // 添加"网络"到导航栏
      IShellItem *psiNetwork = NULL;
      hr = SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&psiNetwork));
      if (SUCCEEDED(hr)) {
        pFileSave->AddPlace(psiNetwork, FDAP_BOTTOM);
        psiNetwork->Release();
      }

      // 显示对话框
      hr = pFileSave->Show(NULL);
      if (SUCCEEDED(hr)) {
        // 获取选择的文件
        IShellItem *pItem = nullptr;
        hr = pFileSave->GetResult(&pItem);
        if (SUCCEEDED(hr)) {
          PWSTR pszFilePath = nullptr;
          hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
          if (SUCCEEDED(hr)) {
            file_path_wide = pszFilePath;
            file_selected = true;
            CoTaskMemFree(pszFilePath);
          }
          pItem->Release();
        }
      }
      else if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        BOOST_LOG(info) << "[tray_export_config] User cancelled file dialog"sv;
      }
      else {
        BOOST_LOG(warning) << "[tray_export_config] File dialog failed: 0x" << std::hex << hr << std::dec;
      }
    };

    // 直接显示文件对话框
    show_file_dialog();

    if (file_selected) {
      std::string file_path = platf::to_utf8(file_path_wide);

      // 安全验证：检查输出文件路径（基本检查）
      try {
        std::filesystem::path p(file_path);
        
        // 检查文件扩展名
        if (p.extension() != ".conf") {
          BOOST_LOG(warning) << "[tray_export_config] Config export rejected: invalid extension: " << p.extension().string();
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = L"只允许导出为 .conf 文件。";
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        // 如果文件已存在，检查是否为符号链接
        if (std::filesystem::exists(p) && std::filesystem::is_symlink(p)) {
          BOOST_LOG(warning) << "[tray_export_config] Config export rejected: target is symlink: " << file_path;
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = L"不允许导出到符号链接。";
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "[tray_export_config] Path validation error during export: " << e.what();
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
        std::wstring message = L"文件路径无效。";
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return;
      }

      try {
        // 读取当前配置
        std::string config_content = file_handler::read_file(config::sunshine.config_file.c_str());
        if (config_content.empty()) {
          BOOST_LOG(error) << "[tray_export_config] No configuration to export";
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_NO_CONFIG));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        // 使用临时文件确保原子性写入
        std::string temp_path = file_path + ".tmp";
        int temp_result = file_handler::write_file(temp_path.c_str(), config_content);
        
        if (temp_result != 0) {
          BOOST_LOG(error) << "[tray_export_config] Failed to write temporary export file";
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_WRITE));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          return;
        }

        // 原子性替换
        try {
          std::filesystem::rename(temp_path, file_path);
          BOOST_LOG(info) << "[tray_export_config] Configuration exported successfully to: " << file_path;
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_SUCCESS_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_SUCCESS_MSG));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        }
        catch (const std::exception &e) {
          BOOST_LOG(error) << "[tray_export_config] Failed to rename temp export file: " << e.what();
          // 清理临时文件
          std::filesystem::remove(temp_path);
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_WRITE));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "[tray_export_config] Exception during config export: " << e.what();
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_EXCEPTION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
    }
  #else
    BOOST_LOG(info) << "[tray_export_config] Config export not implemented for this platform yet";
  #endif
  };

  // 通用语言切换函数
  static auto change_tray_language = [](const std::string &locale, const std::string &language_name) {
    BOOST_LOG(info) << "Changing tray language to " << language_name << " from system tray"sv;
    system_tray_i18n::set_tray_locale(locale);

    // 保存到配置文件
    config::update_config({{"tray_locale", locale}});

    update_menu_texts();
    tray_update(&tray);
  };

  auto tray_language_chinese_cb = [](struct tray_menu *item) {
    change_tray_language("zh", "Chinese");
  };

  auto tray_language_english_cb = [](struct tray_menu *item) {
    change_tray_language("en", "English");
  };

  auto tray_language_japanese_cb = [](struct tray_menu *item) {
    change_tray_language("ja", "Japanese");
  };

  auto tray_reset_config_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting configuration from system tray"sv;

  #ifdef _WIN32
    // 获取本地化字符串
    std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_CONFIRM_TITLE));
    std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_CONFIRM_MSG));

    int msgboxID = MessageBoxW(
      NULL,
      message.c_str(),
      title.c_str(),
      MB_ICONWARNING | MB_YESNO);

    if (msgboxID == IDYES) {
      try {
        // 备份当前配置
        std::string backup_path = config::sunshine.config_file + ".backup";
        std::string current_config = file_handler::read_file(config::sunshine.config_file.c_str());
        if (!current_config.empty()) {
          file_handler::write_file(backup_path.c_str(), current_config);
        }

        // 创建空的配置文件（重置为默认值）
        std::ofstream config_file(config::sunshine.config_file);
        if (config_file.is_open()) {
          config_file.close();
          BOOST_LOG(info) << "Configuration reset successfully";
          std::wstring success_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_SUCCESS_TITLE));
          std::wstring success_msg = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_SUCCESS_MSG));
          MessageBoxW(NULL, success_msg.c_str(), success_title.c_str(), MB_OK | MB_ICONINFORMATION);
        }
        else {
          BOOST_LOG(error) << "Failed to reset configuration file";
          std::wstring error_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_ERROR_TITLE));
          std::wstring error_msg = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_ERROR_MSG));
          MessageBoxW(NULL, error_msg.c_str(), error_title.c_str(), MB_OK | MB_ICONERROR);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Exception during config reset: " << e.what();
        std::wstring error_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_ERROR_TITLE));
        std::wstring error_msg = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_ERROR_EXCEPTION));
        MessageBoxW(NULL, error_msg.c_str(), error_title.c_str(), MB_OK | MB_ICONERROR);
      }
    }
  #else
    BOOST_LOG(info) << "Config reset not implemented for this platform yet";
  #endif
  };

  // 菜单数组定义
  struct tray_menu tray_menus[] = {
    { .text = "Open Sunshine", .cb = tray_open_ui_cb },
    { .text = "-" },
  #ifdef _WIN32
    { .text = "Foundation Display", .submenu = vdd_submenu },
    { .text = "Advanced Settings", .submenu = advanced_settings_submenu },
  #endif
    { .text = "-" },
    { .text = "Language",
      .submenu =
        (struct tray_menu[]) {
          { .text = "中文", .cb = tray_language_chinese_cb },
          { .text = "English", .cb = tray_language_english_cb },
          { .text = "日本語", .cb = tray_language_japanese_cb },
          { .text = nullptr } } },
    { .text = "-" },
    { .text = "Star Project", .cb = tray_star_project_cb },
    { .text = "Visit Project", .submenu = visit_project_submenu },
    { .text = "-" },
    { .text = "Restart", .cb = tray_restart_cb },
    { .text = "Quit", .cb = tray_quit_cb },
    { .text = nullptr }
  };

  struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu = tray_menus,
    .iconPathCount = 4,
    .allIconPaths = { TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING },
  };

  int
  init_tray() {
    // 初始化本地化字符串并更新菜单文本
    update_menu_texts();

  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(),
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &old_dacl,
        nullptr,
        &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(),
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        new_dacl,
        nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }
  #endif

    // 初始化 VDD 子菜单 (创建, 关闭, 保持启用)
    vdd_submenu[0] = { .text = s_vdd_create.c_str(), .cb = tray_vdd_create_cb };
    vdd_submenu[1] = { .text = s_vdd_close.c_str(), .cb = tray_vdd_destroy_cb };
    vdd_submenu[2] = { .text = s_vdd_persistent.c_str(), .checked = 0, .cb = tray_vdd_persistent_cb };
    vdd_submenu[3] = { .text = nullptr };

  #ifdef _WIN32
    advanced_settings_submenu[0] = { .text = s_import_config.c_str(), .cb = tray_import_config_cb };
    advanced_settings_submenu[1] = { .text = s_export_config.c_str(), .cb = tray_export_config_cb };
    advanced_settings_submenu[2] = { .text = s_reset_to_default.c_str(), .cb = tray_reset_config_cb };
    advanced_settings_submenu[3] = { .text = "-" };
    advanced_settings_submenu[4] = { .text = s_close_app.c_str(), .cb = tray_close_app_cb };
    advanced_settings_submenu[5] = { .text = s_reset_display_device_config.c_str(), .cb = tray_reset_display_device_config_cb };
    advanced_settings_submenu[6] = { .text = nullptr };
  #endif

    // 初始化访问项目地址子菜单
    visit_project_submenu[0] = { .text = s_visit_project_sunshine.c_str(), .cb = tray_visit_project_sunshine_cb };
    visit_project_submenu[1] = { .text = s_visit_project_moonlight.c_str(), .cb = tray_visit_project_moonlight_cb };
    visit_project_submenu[2] = { .text = nullptr };

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }
    else {
      BOOST_LOG(info) << "System tray created"sv;
    }

    // 初始化时更新 VDD 菜单状态
    update_vdd_menu_text();
  #ifdef _WIN32
    // 初始化时更新高级设置菜单文本
    update_advanced_settings_menu_text();
  #endif
    // 初始化时更新访问项目地址子菜单文本
    tray_visit_project_submenu_text();
    tray_update(&tray);

    tray_initialized = true;
    return 0;
  }

  int
  process_tray_events() {
    if (!tray_initialized) {
      BOOST_LOG(error) << "System tray is not initialized"sv;
      return 1;
    }

    // Block until an event is processed or tray_quit() is called
    return tray_loop(1);
  }

  int
  end_tray() {
    // Use atomic exchange to ensure only one call proceeds
    if (end_tray_called.exchange(true)) {
      return 0;
    }

    if (!tray_initialized) {
      return 0;
    }

    tray_initialized = false;
    tray_exit();
    return 0;
  }

  void
  update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;

    // 使用本地化字符串（每次都重新获取以支持语言切换）
    static std::string title;
    static std::string msg;
    title = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STREAM_STARTED);
    std::string msg_template = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STREAMING_STARTED_FOR);

    // 使用 std::string 格式化消息
    char buffer[256];
    snprintf(buffer, sizeof(buffer), msg_template.c_str(), app_name.c_str());
    msg = buffer;

    tray.notification_title = title.c_str();
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
  }

  void
  update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);

    // 使用本地化字符串（每次都重新获取以支持语言切换）
    static std::string title;
    static std::string msg;
    title = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STREAM_PAUSED);
    std::string msg_template = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STREAMING_PAUSED_FOR);

    // 使用 std::string 格式化消息
    char buffer[256];
    snprintf(buffer, sizeof(buffer), msg_template.c_str(), app_name.c_str());
    msg = buffer;

    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = title.c_str();
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
  }

  void
  update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);

    // 使用本地化字符串（每次都重新获取以支持语言切换）
    static std::string title;
    static std::string msg;
    title = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_APPLICATION_STOPPED);
    std::string msg_template = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_APPLICATION_STOPPED_MSG);

    // 使用 std::string 格式化消息
    char buffer[256];
    snprintf(buffer, sizeof(buffer), msg_template.c_str(), app_name.c_str());
    msg = buffer;

    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = title.c_str();
    tray.notification_text = msg.c_str();
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void
  update_tray_require_pin(std::string pin_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = NULL;
    tray.notification_text = NULL;
    tray.notification_cb = NULL;
    tray.notification_icon = NULL;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    tray.icon = TRAY_ICON;

    // 使用本地化字符串（每次都重新获取以支持语言切换）
    static std::string title;
    static std::string notification_text;
    std::string title_template = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_INCOMING_PAIRING_REQUEST);
    notification_text = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CLICK_TO_COMPLETE_PAIRING);

    // 使用 std::string 格式化标题
    char buffer[256];
    snprintf(buffer, sizeof(buffer), title_template.c_str(), pin_name.c_str());
    title = buffer;

    tray.notification_title = title.c_str();
    tray.notification_text = notification_text.c_str();
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = pin_name.c_str();
    tray.notification_cb = []() {
      launch_ui_with_path("/pin");
    };
    tray_update(&tray);
  }
 
  void
  update_vdd_menu() {
    if (!tray_initialized) {
      return;
    }
    update_vdd_menu_text();
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void
  tray_thread_worker() {
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      return;
    }

    // Main tray event loop
    while (process_tray_events() == 0);

    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int
  init_tray_threaded() {
    // Reset the end_tray flag for new tray instance
    end_tray_called = false;

    try {
      auto tray_thread = std::thread(tray_thread_worker);

      // The tray thread doesn't require strong lifetime management.
      // It will exit asynchronously when tray_exit() is called.
      tray_thread.detach();

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

}  // namespace system_tray
#endif
