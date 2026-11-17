/**
 * @file src/doctor/doctor_text.cpp
 * @brief Presentation-only punctuation normalization.
 */
#include "doctor.h"

namespace doctor {
  std::string
  zh_punctuation(std::string_view text) {
    // UTF-8 code points, so a mark can look at its neighbours
    std::vector<char32_t> cps;
    for (std::size_t i = 0; i < text.size();) {
      const auto c = static_cast<unsigned char>(text[i]);
      const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 :
                                 (c >> 4) == 0xE   ? 3 :
                                 (c >> 3) == 0x1E  ? 4 :
                                                     1;
      char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) :
                               n == 3   ? (c & 0x0F) :
                                          (c & 0x07);
      for (int k = 1; k < n && i + k < text.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
      cps.push_back(cp);
      i += n;
    }
    auto wide = [](char32_t cp) { return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF) || cp == 0x201C || cp == 0x201D; };
    for (std::size_t i = 0; i < cps.size(); ++i) {
      const bool before = i > 0 && wide(cps[i - 1]);
      const bool after = i + 1 < cps.size() && wide(cps[i + 1]);
      switch (cps[i]) {
        case U',':
          if (before || after) cps[i] = U'，';
          break;
        case U';':
          if (before || after) cps[i] = U'；';
          break;
        case U':':
          if (before || after) cps[i] = U'：';
          break;
        case U'?':
          if (before) cps[i] = U'？';
          break;
        case U'(':
          if (before || after) cps[i] = U'（';
          break;
        case U')':
          if (before) cps[i] = U'）';
          break;
        default:
          break;
      }
    }
    // a full-width opening bracket pairs with the next ")"
    bool open = false;
    for (auto &cp : cps) {
      if (cp == U'（')
        open = true;
      else if (cp == U')' && open) {
        cp = U'）';
        open = false;
      }
      else if (cp == U'）')
        open = false;
    }
    std::string out;
    for (const auto cp : cps) {
      if (cp < 0x80)
        out += static_cast<char>(cp);
      else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    return out;
  }

}  // namespace doctor
