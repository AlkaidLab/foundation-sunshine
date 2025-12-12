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
  static std::string s_vdd_monitor_toggle;
  static std::string s_configuration;
  static std::string s_import_config;
  static std::string s_export_config;
  static std::string s_reset_to_default;
  static std::string s_language;
  static std::string s_chinese;
  static std::string s_english;
  static std::string s_japanese;
  static std::string s_star_project;
  static std::string s_help_us;
  static std::string s_developer_yundi339;
  static std::string s_developer_qiin;
  static std::string s_reset_display_device_config;
  static std::string s_restart;
  static std::string s_quit;

  // 初始化本地化字符串
  void
  init_localized_strings() {
    s_open_sunshine = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_OPEN_SUNSHINE);
    s_vdd_monitor_toggle = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_MONITOR_TOGGLE);
    s_configuration = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CONFIGURATION);
    s_import_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_CONFIG);
    s_export_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_CONFIG);
    s_reset_to_default = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_TO_DEFAULT);
    s_language = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_LANGUAGE);
    s_chinese = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_CHINESE);
    s_english = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ENGLISH);
    s_japanese = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_JAPANESE);
    s_star_project = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_STAR_PROJECT);
    s_help_us = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_HELP_US);
    s_developer_yundi339 = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_DEVELOPER_YUNDI339);
    s_developer_qiin = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_DEVELOPER_QIIN);
    s_reset_display_device_config = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESET_DISPLAY_DEVICE_CONFIG);
    s_restart = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_RESTART);
    s_quit = system_tray_i18n::get_localized_string(system_tray_i18n::KEY_QUIT);
  }

  // 更新所有菜单项的文本
  void
  update_menu_texts() {
    init_localized_strings();
    tray_menus[0].text = s_open_sunshine.c_str();
    tray_menus[2].text = s_vdd_monitor_toggle.c_str();
    tray_menus[4].text = s_configuration.c_str();
    tray_menus[4].submenu[0].text = s_import_config.c_str();
    tray_menus[4].submenu[1].text = s_export_config.c_str();
    tray_menus[4].submenu[2].text = s_reset_to_default.c_str();
    tray_menus[6].text = s_language.c_str();
    tray_menus[6].submenu[0].text = s_chinese.c_str();
    tray_menus[6].submenu[1].text = s_english.c_str();
    tray_menus[6].submenu[2].text = s_japanese.c_str();
    tray_menus[8].text = s_star_project.c_str();
    tray_menus[9].text = s_help_us.c_str();
    tray_menus[9].submenu[0].text = s_developer_yundi339.c_str();
    tray_menus[9].submenu[1].text = s_developer_qiin.c_str();
  #ifdef _WIN32
    tray_menus[11].text = s_reset_display_device_config.c_str();
    tray_menus[12].text = s_restart.c_str();
    tray_menus[13].text = s_quit.c_str();
  #else
    tray_menus[11].text = s_restart.c_str();
    tray_menus[12].text = s_quit.c_str();
  #endif
  }

  auto tray_open_ui_cb = [](struct tray_menu *item) {
    BOOST_LOG(debug) << "Opening UI from system tray"sv;
    launch_ui();
  };

  auto tray_toggle_display_cb = [](struct tray_menu *item) {
    // 添加状态检查和日志
    if (!tray_initialized) {
      BOOST_LOG(warning) << "Tray not initialized, ignoring toggle";
      return;
    }

    if (tray_menus[2].disabled) {
      BOOST_LOG(info) << "Toggle display is in cooldown, ignoring request";
      return;
    }

    BOOST_LOG(info) << "Toggling display power from system tray"sv;
    display_device::session_t::get().toggle_display_power();

    // 添加10秒禁用状态
    tray_menus[2].disabled = 1;
    tray_update(&tray);

    // use thread to restore button state
    std::thread([&]() {
      std::this_thread::sleep_for(10s);
      tray_menus[2].disabled = 0;
      tray_update(&tray);
    }).detach();
  };

  auto tray_reset_display_device_config_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;
    display_device::session_t::get().reset_persistence();
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
    platf::open_url_in_browser("https://github.com/qiin2333/Sunshine-Foundation");
  };

  auto tray_donate_yundi339_cb = [](struct tray_menu *item) {
    platf::open_url_in_browser("https://www.ifdian.net/a/Yundi339");
  };

  auto tray_donate_qiin_cb = [](struct tray_menu *item) {
    platf::open_url_in_browser("https://www.ifdian.net/a/qiin2333");
  };

  // 配置导入功能
  auto tray_import_config_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Importing configuration from system tray"sv;

  #ifdef _WIN32
    std::wstring file_path_wide;
    bool file_selected = false;

    // 如果以SYSTEM身份运行，需要模拟用户身份来避免访问系统配置文件夹的错误
    HANDLE user_token = NULL;
    if (platf::is_running_as_system()) {
      user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(warning) << "Unable to retrieve user token for file dialog";
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ERROR_NO_USER_SESSION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return;
      }
    }

    // 定义文件对话框逻辑为lambda，以便在模拟用户上下文中执行
    auto show_file_dialog = [&]() {
      // 初始化COM
      HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      bool com_initialized = SUCCEEDED(hr);

      IFileOpenDialog *pFileOpen = NULL;

      // 创建文件打开对话框
      hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void **>(&pFileOpen));

      if (SUCCEEDED(hr)) {
        // 设置文件类型过滤器
        std::wstring config_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_CONFIG_FILES));
        std::wstring all_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_ALL_FILES));
        COMDLG_FILTERSPEC rgSpec[] = {
          { config_files.c_str(), L"*.conf" },
          { all_files.c_str(), L"*.*" }
        };
        pFileOpen->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
        pFileOpen->SetFileTypeIndex(1);
        std::wstring dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_SELECT_IMPORT));
        pFileOpen->SetTitle(dialog_title.c_str());

        // 设置选项：禁用所有可能导致访问系统位置的功能
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        // FOS_FORCEFILESYSTEM: 强制只使用文件系统
        // FOS_DONTADDTORECENT: 不添加到最近文件列表
        // FOS_NOCHANGEDIR: 不改变当前工作目录
        // FOS_HIDEPINNEDPLACES: 隐藏固定的位置（导航面板中的快速访问等）
        // FOS_NOVALIDATE: 不验证文件路径（避免访问不存在的系统路径）
        pFileOpen->SetOptions(dwFlags | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT |
                              FOS_NOCHANGEDIR | FOS_HIDEPINNEDPLACES | FOS_NOVALIDATE);

        // 显示对话框
        hr = pFileOpen->Show(NULL);

        if (SUCCEEDED(hr)) {
          IShellItem *pItem;
          hr = pFileOpen->GetResult(&pItem);
          if (SUCCEEDED(hr)) {
            PWSTR pszFilePath;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
            if (SUCCEEDED(hr)) {
              file_path_wide = pszFilePath;
              file_selected = true;
              CoTaskMemFree(pszFilePath);
            }
            pItem->Release();
          }
        }
        pFileOpen->Release();
      }

      if (com_initialized) {
        CoUninitialize();
      }
    };

    // 如果有用户令牌，在模拟用户身份的上下文中显示对话框
    if (user_token) {
      platf::impersonate_current_user(user_token, show_file_dialog);
      CloseHandle(user_token);
    }
    else {
      // 否则直接显示
      show_file_dialog();
    }

    if (file_selected) {
      std::string file_path = platf::to_utf8(file_path_wide);

      try {
        // 读取配置文件内容
        std::string config_content = file_handler::read_file(file_path.c_str());
        if (!config_content.empty()) {
          // 备份当前配置
          std::string backup_path = config::sunshine.config_file + ".backup";
          std::string current_config = file_handler::read_file(config::sunshine.config_file.c_str());
          file_handler::write_file(backup_path.c_str(), current_config);

          // 写入新配置
          int result = file_handler::write_file(config::sunshine.config_file.c_str(), config_content);
          if (result == 0) {
            BOOST_LOG(info) << "Configuration imported successfully from: " << file_path;
            std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_SUCCESS_TITLE));
            std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_SUCCESS_MSG));
            MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
          }
          else {
            BOOST_LOG(error) << "Failed to write imported configuration";
            std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
            std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_WRITE));
            MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          }
        }
        else {
          BOOST_LOG(error) << "Failed to read configuration file: " << file_path;
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_READ));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Exception during config import: " << e.what();
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_IMPORT_ERROR_EXCEPTION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
    }
  #else
    // 非Windows平台的实现（可以后续添加）
    BOOST_LOG(info) << "Config import not implemented for this platform yet";
  #endif
  };

  // 配置导出功能
  auto tray_export_config_cb = [](struct tray_menu *item) {
    BOOST_LOG(info) << "Exporting configuration from system tray"sv;

  #ifdef _WIN32
    std::wstring file_path_wide;
    bool file_selected = false;

    // 如果以SYSTEM身份运行，需要模拟用户身份来避免访问系统配置文件夹的错误
    HANDLE user_token = NULL;
    if (platf::is_running_as_system()) {
      user_token = platf::retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(warning) << "Unable to retrieve user token for file dialog";
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_ERROR_NO_USER_SESSION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        return;
      }
    }

    // 定义文件对话框逻辑为lambda，以便在模拟用户上下文中执行
    auto show_file_dialog = [&]() {
      // 初始化COM
      HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      bool com_initialized = SUCCEEDED(hr);

      IFileSaveDialog *pFileSave = NULL;

      // 创建文件保存对话框
      hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_ALL, IID_IFileSaveDialog, reinterpret_cast<void **>(&pFileSave));

      if (SUCCEEDED(hr)) {
        // 设置文件类型过滤器
        std::wstring config_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_CONFIG_FILES));
        std::wstring all_files = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_ALL_FILES));
        COMDLG_FILTERSPEC rgSpec[] = {
          { config_files.c_str(), L"*.conf" },
          { all_files.c_str(), L"*.*" }
        };
        pFileSave->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
        pFileSave->SetFileTypeIndex(1);
        pFileSave->SetDefaultExtension(L"conf");
        std::wstring dialog_title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_FILE_DIALOG_SAVE_EXPORT));
        pFileSave->SetTitle(dialog_title.c_str());

        // 设置默认文件名
        std::string default_name = "sunshine_config_" + std::to_string(std::time(nullptr)) + ".conf";
        std::wstring wdefault_name(default_name.begin(), default_name.end());
        pFileSave->SetFileName(wdefault_name.c_str());

        // 设置选项：禁用所有可能导致访问系统位置的功能
        DWORD dwFlags;
        pFileSave->GetOptions(&dwFlags);
        pFileSave->SetOptions(dwFlags | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT |
                              FOS_NOCHANGEDIR | FOS_HIDEPINNEDPLACES | FOS_NOVALIDATE);

        // 显示对话框
        hr = pFileSave->Show(NULL);

        if (SUCCEEDED(hr)) {
          IShellItem *pItem;
          hr = pFileSave->GetResult(&pItem);
          if (SUCCEEDED(hr)) {
            PWSTR pszFilePath;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
            if (SUCCEEDED(hr)) {
              file_path_wide = pszFilePath;
              file_selected = true;
              CoTaskMemFree(pszFilePath);
            }
            pItem->Release();
          }
        }
        pFileSave->Release();
      }

      if (com_initialized) {
        CoUninitialize();
      }
    };

    // 如果有用户令牌，在模拟用户身份的上下文中显示对话框
    if (user_token) {
      platf::impersonate_current_user(user_token, show_file_dialog);
      CloseHandle(user_token);
    }
    else {
      // 否则直接显示
      show_file_dialog();
    }

    if (file_selected) {
      std::string file_path = platf::to_utf8(file_path_wide);

      try {
        // 读取当前配置
        std::string config_content = file_handler::read_file(config::sunshine.config_file.c_str());
        if (!config_content.empty()) {
          // 写入到选择的文件
          int result = file_handler::write_file(file_path.c_str(), config_content);
          if (result == 0) {
            BOOST_LOG(info) << "Configuration exported successfully to: " << file_path;
            std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_SUCCESS_TITLE));
            std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_SUCCESS_MSG));
            MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
          }
          else {
            BOOST_LOG(error) << "Failed to write exported configuration";
            std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
            std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_WRITE));
            MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
          }
        }
        else {
          BOOST_LOG(error) << "No configuration to export";
          std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
          std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_NO_CONFIG));
          MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Exception during config export: " << e.what();
        std::wstring title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_TITLE));
        std::wstring message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_EXPORT_ERROR_EXCEPTION));
        MessageBoxW(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
      }
    }
  #else
    BOOST_LOG(info) << "Config export not implemented for this platform yet";
  #endif
  };

  // 通用语言切换函数
  static auto change_tray_language = [](const std::string &locale, const std::string &language_name) {
    BOOST_LOG(info) << "Changing tray language to " << language_name << " from system tray"sv;
    system_tray_i18n::set_tray_locale(locale);

    // 保存到配置文件
    try {
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      std::stringstream configStream;

      // 更新或添加 tray_locale 配置项
      vars["tray_locale"] = locale;
      for (const auto &[key, value] : vars) {
        if (!value.empty() && value != "null") {
          configStream << key << " = " << value << std::endl;
        }
      }

      file_handler::write_file(config::sunshine.config_file.c_str(), configStream.str());
      BOOST_LOG(info) << "Tray language setting saved to config file"sv;
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Failed to save tray language setting: "sv << e.what();
    }

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
    { .text = "VDD Monitor Toggle", .checked = 0, .cb = tray_toggle_display_cb },
    { .text = "-" },
    { .text = "Configuration",
      .submenu =
        (struct tray_menu[]) {
          { .text = "Import Config", .cb = tray_import_config_cb },
          { .text = "Export Config", .cb = tray_export_config_cb },
          { .text = "Reset to Default", .cb = tray_reset_config_cb },
          { .text = nullptr } } },
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
    { .text = "Help Us",
      .submenu =
        (struct tray_menu[]) {
          { .text = "Developer: Yundi339", .cb = tray_donate_yundi339_cb },
          { .text = "Developer: Qiin", .cb = tray_donate_qiin_cb },
          { .text = nullptr } } },
    { .text = "-" },
  #ifdef _WIN32
    { .text = "Reset Display Memory", .cb = tray_reset_display_device_config_cb },
  #endif
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

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }
    else {
      BOOST_LOG(info) << "System tray created"sv;
    }

    // 初始化时获取实际显示器状态
    tray_menus[2].checked = display_device::session_t::get().is_display_on() ? 1 : 0;
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
  update_tray_vmonitor_checked(int checked) {
    if (!tray_initialized) {
      return;
    }
    // 更新显示器切换菜单项的勾选状态
    tray_menus[2].checked = checked;
    // 同时更新禁用状态（冷却期间保持禁用）
    tray_menus[2].disabled = checked ? 0 : tray_menus[2].disabled;
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
