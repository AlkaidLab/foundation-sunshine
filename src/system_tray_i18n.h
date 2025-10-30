#pragma once

#include <string>
#include <map>

namespace system_tray_i18n {
  // String key constants
  extern const std::string KEY_QUIT_TITLE;
  extern const std::string KEY_QUIT_MESSAGE;
  
  // Menu item keys
  extern const std::string KEY_OPEN_SUNSHINE;
  extern const std::string KEY_VDD_MONITOR_TOGGLE;
  extern const std::string KEY_CONFIGURATION;
  extern const std::string KEY_IMPORT_CONFIG;
  extern const std::string KEY_EXPORT_CONFIG;
  extern const std::string KEY_RESET_TO_DEFAULT;
  extern const std::string KEY_LANGUAGE;
  extern const std::string KEY_CHINESE;
  extern const std::string KEY_ENGLISH;
  extern const std::string KEY_JAPANESE;
  extern const std::string KEY_STAR_PROJECT;
  extern const std::string KEY_HELP_US;
  extern const std::string KEY_DEVELOPER_YUNDI339;
  extern const std::string KEY_DEVELOPER_QIIN;
  extern const std::string KEY_RESET_DISPLAY_DEVICE_CONFIG;
  extern const std::string KEY_RESTART;
  extern const std::string KEY_QUIT;
  
  // Notification message keys
  extern const std::string KEY_STREAM_STARTED;
  extern const std::string KEY_STREAMING_STARTED_FOR;
  extern const std::string KEY_STREAM_PAUSED;
  extern const std::string KEY_STREAMING_PAUSED_FOR;
  extern const std::string KEY_APPLICATION_STOPPED;
  extern const std::string KEY_APPLICATION_STOPPED_MSG;
  extern const std::string KEY_INCOMING_PAIRING_REQUEST;
  extern const std::string KEY_CLICK_TO_COMPLETE_PAIRING;
  
  // MessageBox keys
  extern const std::string KEY_ERROR_TITLE;
  extern const std::string KEY_ERROR_NO_USER_SESSION;
  extern const std::string KEY_IMPORT_SUCCESS_TITLE;
  extern const std::string KEY_IMPORT_SUCCESS_MSG;
  extern const std::string KEY_IMPORT_ERROR_TITLE;
  extern const std::string KEY_IMPORT_ERROR_WRITE;
  extern const std::string KEY_IMPORT_ERROR_READ;
  extern const std::string KEY_IMPORT_ERROR_EXCEPTION;
  extern const std::string KEY_EXPORT_SUCCESS_TITLE;
  extern const std::string KEY_EXPORT_SUCCESS_MSG;
  extern const std::string KEY_EXPORT_ERROR_TITLE;
  extern const std::string KEY_EXPORT_ERROR_WRITE;
  extern const std::string KEY_EXPORT_ERROR_NO_CONFIG;
  extern const std::string KEY_EXPORT_ERROR_EXCEPTION;
  extern const std::string KEY_RESET_CONFIRM_TITLE;
  extern const std::string KEY_RESET_CONFIRM_MSG;
  extern const std::string KEY_RESET_SUCCESS_TITLE;
  extern const std::string KEY_RESET_SUCCESS_MSG;
  extern const std::string KEY_RESET_ERROR_TITLE;
  extern const std::string KEY_RESET_ERROR_MSG;
  extern const std::string KEY_RESET_ERROR_EXCEPTION;
  extern const std::string KEY_FILE_DIALOG_SELECT_IMPORT;
  extern const std::string KEY_FILE_DIALOG_SAVE_EXPORT;
  extern const std::string KEY_FILE_DIALOG_CONFIG_FILES;
  extern const std::string KEY_FILE_DIALOG_ALL_FILES;
  
  // Get localized string
  std::string get_localized_string(const std::string& key);
  
  // Set tray locale
  void set_tray_locale(const std::string& locale);
  
  // Convert UTF-8 string to wide string
  std::wstring utf8_to_wstring(const std::string& utf8_str);
}
