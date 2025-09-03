#pragma once

#include <string>
#include <map>

namespace system_tray_i18n {
  // String key constants
  extern const std::string KEY_QUIT_TITLE;
  extern const std::string KEY_QUIT_MESSAGE;
  
  // Get localized string
  std::string get_localized_string(const std::string& key);
  
  // Convert UTF-8 string to wide string
  std::wstring utf8_to_wstring(const std::string& utf8_str);
}
