/**
 * @file src/crash/crash_env_text.cpp
 * @brief The text rules of the machine description (see crash_env.h); no system calls, so the unit tests link it on its own.
 */
#include "crash_env.h"

#include <algorithm>
#include <cctype>

namespace crash::env {
  namespace {
    std::string
    lower(std::string_view s) {
      std::string out(s);
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::tolower(c); });
      return out;
    }
  }  // namespace

  std::string
  sanitize_model(std::string_view raw) {
    std::string out;
    bool space = true;
    for (unsigned char c : raw) {
      const bool ok = std::isalnum(c) || std::string_view(".+-_()/@").find((char) c) != std::string_view::npos;
      if (ok) {
        out += (char) c;
        space = false;
      }
      else if (!space) {
        out += ' ';
        space = true;
      }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 64) out.resize(64);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty() && !std::isalnum((unsigned char) out.front())) out.erase(0, out.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos ? out.size() : out.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"));
    return out;
  }

  std::string
  sanitize_version(std::string_view raw) {
    std::string out;
    for (unsigned char c : raw)
      if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '+') out += (char) c;
    if (out.size() > 32) out.resize(32);
    return !out.empty() && std::isalnum((unsigned char) out.front()) ? out : std::string();
  }

  std::string
  gpu_vendor_word(std::string_view vendor_or_name) {
    const auto v = lower(vendor_or_name);
    auto has = [&](const char *needle) { return v.find(needle) != std::string::npos; };
    if (has("nvidia") || has("geforce") || has("quadro")) return "nvidia";
    if (has("advanced micro") || has("amd") || has("radeon") || has("ati ")) return "amd";
    if (has("intel")) return "intel";
    if (has("apple")) return "apple";
    if (has("qualcomm") || has("adreno")) return "qualcomm";
    if (has("microsoft")) return "microsoft";
    return "other";
  }
}  // namespace crash::env
