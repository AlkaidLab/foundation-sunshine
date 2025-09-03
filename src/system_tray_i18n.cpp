#include "system_tray_i18n.h"
#include "config.h"

#ifdef _WIN32
  #include <windows.h>
#endif

namespace system_tray_i18n {
  // String key constants
  const std::string KEY_QUIT_TITLE = "quit_title";
  const std::string KEY_QUIT_MESSAGE = "quit_message";

  // Default English strings
  const std::map<std::string, std::string> DEFAULT_STRINGS = {
    { KEY_QUIT_TITLE, "Really Quit?" },
    { KEY_QUIT_MESSAGE, "You can't quit!\nDo you really want to quit? I can't help you, keep clicking~\n\nThis will also close the Sunshine GUI application." }
  };

  // Chinese strings
  const std::map<std::string, std::string> CHINESE_STRINGS = {
    { KEY_QUIT_TITLE, "真的要退出吗" },
    { KEY_QUIT_MESSAGE, "你不能退出!\n那么想退吗? 真拿你没办法呢, 继续点一下吧~\n\n这将同时关闭Sunshine GUI应用程序。" }
  };

  const std::map<std::string, std::string> JAPANESE_STRINGS = {
    { KEY_QUIT_TITLE, "本当に終了しますか？" },
    { KEY_QUIT_MESSAGE, "終了できません！\n本当に終了したいですか？\n\nこれによりSunshine GUIアプリケーションも閉じられます。" }
  };

  // Get current locale from config
  std::string
  get_current_locale() {
    // Try to get from config::sunshine.locale
    try {
      // Check if config is available
      if (!config::sunshine.locale.empty()) {
        return config::sunshine.locale;
      }
    }
    catch (...) {
      // If config is not available, fall back to default
    }

    // Default to Chinese for now
    return "zh";
  }

  // Get localized string
  std::string
  get_localized_string(const std::string &key) {
    std::string locale = get_current_locale();

    if (locale == "zh" || locale == "zh_CN" || locale == "zh_TW") {
      auto it = CHINESE_STRINGS.find(key);
      if (it != CHINESE_STRINGS.end()) {
        return it->second;
      }
    }

    if (locale == "ja" || locale == "ja_JP") {
      auto it = JAPANESE_STRINGS.find(key);
      if (it != JAPANESE_STRINGS.end()) {
        return it->second;
      }
    }

    // Fallback to English
    auto it = DEFAULT_STRINGS.find(key);
    if (it != DEFAULT_STRINGS.end()) {
      return it->second;
    }

    return key;  // Return key if not found
  }

  // Convert UTF-8 string to wide string
  std::wstring
  utf8_to_wstring(const std::string &utf8_str) {
    // Modern C++ approach: use Windows API on Windows, simple conversion on other platforms
  #ifdef _WIN32
    if (utf8_str.empty()) {
      return L"";
    }
    
    // Get required buffer size
    int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (wide_size == 0) {
      // Fallback: simple char-by-char conversion
      std::wstring result;
      result.reserve(utf8_str.length());
      for (char c : utf8_str) {
        result += static_cast<wchar_t>(c);
      }
      return result;
    }
    
    // Convert to wide string
    std::wstring result(wide_size - 1, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, &result[0], wide_size) == 0) {
      // Fallback: simple char-by-char conversion
      result.clear();
      result.reserve(utf8_str.length());
      for (char c : utf8_str) {
        result += static_cast<wchar_t>(c);
      }
    }
    return result;
  #else
    // On non-Windows platforms, use simple char-by-char conversion
    // This is not perfect for UTF-8, but it's a reasonable fallback
    std::wstring result;
    result.reserve(utf8_str.length());
    for (char c : utf8_str) {
      result += static_cast<wchar_t>(c);
    }
    return result;
  #endif
  }
}  // namespace system_tray_i18n
