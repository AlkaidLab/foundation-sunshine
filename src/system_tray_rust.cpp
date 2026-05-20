/**
 * @file src/system_tray_rust.cpp
 * @brief System tray implementation using the Rust tray library
 *
 * This file provides a thin C++ wrapper around the Rust tray library.
 * All menu logic, i18n, and event handling is done in Rust.
 */

#if defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <tlhelp32.h>
  #define ICON_PATH_NORMAL WEB_DIR "images/sunshine.ico"
  #define ICON_PATH_PLAYING WEB_DIR "images/sunshine-playing.ico"
  #define ICON_PATH_PAUSING WEB_DIR "images/sunshine-pausing.ico"
  #define ICON_PATH_LOCKED WEB_DIR "images/sunshine-locked.ico"
#elif defined(__linux__) || defined(linux) || defined(__linux)
  #define ICON_PATH_NORMAL "sunshine-tray"
  #define ICON_PATH_PLAYING "sunshine-playing"
  #define ICON_PATH_PAUSING "sunshine-pausing"
  #define ICON_PATH_LOCKED "sunshine-locked"
#elif defined(__APPLE__) || defined(__MACH__)
  #define ICON_PATH_NORMAL WEB_DIR "images/logo-sunshine-16.png"
  #define ICON_PATH_PLAYING WEB_DIR "images/sunshine-playing-16.png"
  #define ICON_PATH_PAUSING WEB_DIR "images/sunshine-pausing-16.png"
  #define ICON_PATH_LOCKED WEB_DIR "images/sunshine-locked-16.png"
#endif

// Boost includes
#include <boost/filesystem.hpp>

// Local includes
#include "config.h"
#include "confighttp.h"
#include "display_device/display_device.h"
#include "display_device/session.h"
#include "entry_handler.h"
#include "globals.h"
#include "file_handler.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "version.h"

// Rust tray API
#include "rust_tray/include/rust_tray.h"

using namespace std::literals;

namespace system_tray {

  static std::atomic<bool> tray_initialized = false;
  static std::atomic<bool> end_tray_called = false;

  // VDD state management
  static std::atomic<bool> s_vdd_in_cooldown = false;

  // Forward declarations
  static void handle_tray_action(uint32_t action);

  /**
   * @brief Check if VDD is active
   */
  static bool is_vdd_active() {
    auto vdd_device_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
    return !vdd_device_id.empty();
  }

  /**
   * @brief Update VDD menu state in Rust tray
   */
  static void update_vdd_menu_state() {
    bool vdd_active = is_vdd_active();
    bool keep_enabled = config::video.vdd_keep_enabled;
    bool in_cooldown = s_vdd_in_cooldown.load();

    // Create: enabled when NOT active AND NOT in cooldown
    int can_create = (!vdd_active && !in_cooldown) ? 1 : 0;
    // Close: enabled when active AND NOT in cooldown AND NOT keep_enabled
    int can_close = (vdd_active && !in_cooldown && !keep_enabled) ? 1 : 0;

    tray_update_vdd_menu(can_create, can_close, keep_enabled ? 1 : 0, vdd_active ? 1 : 0);
  }

  /**
   * @brief Start VDD cooldown (10 seconds)
   */
  static void start_vdd_cooldown() {
    s_vdd_in_cooldown = true;
    update_vdd_menu_state();

    std::thread([]() {
      std::this_thread::sleep_for(10s);
      s_vdd_in_cooldown = false;
      update_vdd_menu_state();
    }).detach();
  }

  /**
   * @brief Handle tray actions from Rust (string-based)
   */
  static void handle_tray_action(const char* action_id) {
    if (!action_id) return;
    
    std::string action(action_id);
    
    if (action == TRAY_ACTION_OPEN_SUNSHINE) {
      launch_ui();
    }
    else if (action == TRAY_ACTION_VDD_CREATE) {
      BOOST_LOG(info) << "Creating VDD from system tray"sv;
      if (!s_vdd_in_cooldown && !is_vdd_active()) {
        if (display_device::session_t::get().toggle_display_power()) {
          start_vdd_cooldown();
        }
      }
    }
    else if (action == TRAY_ACTION_VDD_CLOSE) {
      BOOST_LOG(info) << "Closing VDD from system tray"sv;
      if (!s_vdd_in_cooldown && is_vdd_active() && !config::video.vdd_keep_enabled) {
        display_device::session_t::get().destroy_vdd_monitor();
        start_vdd_cooldown();
      }
    }
    else if (action == TRAY_ACTION_VDD_PERSISTENT) {
      BOOST_LOG(info) << "Toggling VDD persistent mode"sv;
      config::video.vdd_keep_enabled = !config::video.vdd_keep_enabled;
      config::update_config({{"vdd_keep_enabled", config::video.vdd_keep_enabled ? "true" : "false"}});
      update_vdd_menu_state();
    }
    else if (action == TRAY_ACTION_LANG_CHINESE) {
      config::update_config({{"tray_locale", "zh"}});
    }
    else if (action == TRAY_ACTION_LANG_ENGLISH) {
      config::update_config({{"tray_locale", "en"}});
    }
    else if (action == TRAY_ACTION_LANG_JAPANESE) {
      config::update_config({{"tray_locale", "ja"}});
    }
    else if (action == TRAY_ACTION_CLOSE_APP) {
      BOOST_LOG(info) << "Closing application from system tray"sv;
      proc::proc.terminate();
    }
    else if (action == TRAY_ACTION_STAR_PROJECT) {
      platf::open_url_in_browser("https://sunshine-foundation.vercel.app/");
    }
    else if (action == TRAY_ACTION_VISIT_SUNSHINE) {
      platf::open_url_in_browser("https://github.com/qiin2333/Sunshine-Foundation");
    }
    else if (action == TRAY_ACTION_VISIT_MOONLIGHT) {
      platf::open_url_in_browser("https://github.com/qiin2333/moonlight-vplus");
    }
    else if (action == TRAY_ACTION_RESET_DISPLAY) {
      BOOST_LOG(info) << "Resetting display device config"sv;
      display_device::session_t::get().reset_persistence();
    }
    else if (action == TRAY_ACTION_RESTART) {
      BOOST_LOG(info) << "Restarting from system tray"sv;
      platf::restart();
    }
    else if (action == TRAY_ACTION_QUIT) {
      BOOST_LOG(info) << "Quitting from system tray"sv;
#ifdef _WIN32
      terminate_gui_processes();
      if (GetConsoleWindow() == NULL) {
          lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      }
      else {
          lifetime::exit_sunshine(0, true);
      }
#else
      lifetime::exit_sunshine(0, true);
#endif
    }
    // Other actions (language, close_app) are handled entirely in Rust
  }

  void terminate_gui_processes() {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe32;
      pe32.dwSize = sizeof(PROCESSENTRY32W);

      if (Process32FirstW(snapshot, &pe32)) {
        do {
          if (wcscmp(pe32.szExeFile, L"sunshine-gui.exe") == 0) {
            HANDLE process_handle = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
            if (process_handle != NULL) {
              TerminateProcess(process_handle, 0);
              CloseHandle(process_handle);
            }
          }
        } while (Process32NextW(snapshot, &pe32));
      }
      CloseHandle(snapshot);
    }
#endif
  }

  int init_tray() {
    if (tray_initialized.exchange(true)) {
      BOOST_LOG(warning) << "Tray already initialized"sv;
      return 0;
    }

    // Get locale from config
    std::string locale = "zh";  // Default to Chinese
    try {
      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      if (vars.count("tray_locale") > 0) {
        locale = vars["tray_locale"];
      }
    } catch (...) {
      // Ignore errors, use default locale
    }

    // Create tooltip with version
    std::string tooltip = "Sunshine "s + PROJECT_VER;

    // Initialize the Rust tray
    int result = tray_init_ex(
      ICON_PATH_NORMAL,
      ICON_PATH_PLAYING,
      ICON_PATH_PAUSING,
      ICON_PATH_LOCKED,
      tooltip.c_str(),
      locale.c_str(),
      handle_tray_action
    );

    if (result != 0) {
      BOOST_LOG(error) << "Failed to initialize Rust tray"sv;
      tray_initialized = false;
      return -1;
    }

    // Initialize VDD menu state
    update_vdd_menu_state();

    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      return -1;
    }
    return tray_loop(0);  // Non-blocking
  }

  int end_tray() {
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

  int init_tray_threaded() {
    // Reset the end_tray flag for new tray instance
    end_tray_called = false;

    std::thread tray_thread([]() {
      if (init_tray() != 0) {
        return;
      }

      // Main tray event loop
      while (process_tray_events() == 0);
    });

    // The tray thread doesn't require strong lifetime management.
    // It will exit asynchronously when tray_exit() is called.
    tray_thread.detach();

    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) return;

    tray_set_icon(TRAY_ICON_TYPE_PLAYING);

    std::string tooltip = "Sunshine - Playing: " + app_name;
    tray_set_tooltip(tooltip.c_str());

    // Show localized notification
    tray_show_localized_notification(TRAY_NOTIFICATION_STREAM_STARTED, app_name.c_str());
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) return;

    tray_set_icon(TRAY_ICON_TYPE_PAUSING);

    std::string tooltip = "Sunshine - Paused: " + app_name;
    tray_set_tooltip(tooltip.c_str());

    // Show localized notification
    tray_show_localized_notification(TRAY_NOTIFICATION_STREAM_PAUSED, app_name.c_str());
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) return;

    tray_set_icon(TRAY_ICON_TYPE_NORMAL);

    std::string tooltip = "Sunshine "s + PROJECT_VER;
    tray_set_tooltip(tooltip.c_str());

    // Show localized notification for application stopped
    if (!app_name.empty()) {
      tray_show_localized_notification(TRAY_NOTIFICATION_APP_STOPPED, app_name.c_str());
    }
  }

  void update_tray_require_pin(std::string pin_name) {
    if (!tray_initialized) return;

    // Show localized pairing request notification
    tray_show_localized_notification(TRAY_NOTIFICATION_PAIRING_REQUEST, pin_name.c_str());
  }

  void update_tray_vmonitor_checked(int checked) {
    if (!tray_initialized) return;
    // Use the unified VDD menu update function
    update_vdd_menu_state();
  }

  void update_vdd_menu() {
    if (!tray_initialized) return;
    // Update VDD menu state (called by vdd_utils)
    update_vdd_menu_state();
  }

}  // namespace system_tray

#endif  // SUNSHINE_TRAY
