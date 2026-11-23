/**
 * @file src/generated_cover.cpp
 * @brief Deterministic application cover selection and Windows rendering.
 *
 * The Windows path uses Direct2D's software render target over a WIC bitmap,
 * DirectWrite layouts, and WIC encoders. The selected facilities follow
 * installer-ms-direct2d-portal-20261107, installer-ms-direct2d-write-20261107,
 * installer-ms-directwrite-start-20261107, ms-wic-factory-20261110,
 * fs-ms-dwrite-createtextlayout-20261123, fs-ms-shellitemimagefactory-getimage-20261123,
 * and fs-ms-extracticonexw-20261123.
 */
#include "generated_cover.h"
#include "generated_cover_palette.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <openssl/evp.h>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <d2d1.h>
  #include <dwrite_2.h>
  #include <objidl.h>
  #include <shellapi.h>
  #include <shobjidl.h>
  #include <wincodec.h>
  #include <windows.h>
  #include <wrl/client.h>
#endif

namespace generated_cover {

  namespace {

    constexpr std::array<std::string_view, 6> kStyles {
      "tonal",
      "monogram",
      "icon",
      "editorial",
      "glass",
      "geometric",
    };

    std::string
    sha256(std::string_view value) {
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
      unsigned int length = 0;
      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
          EVP_DigestUpdate(context, value.data(), value.size()) != 1 ||
          EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
        EVP_MD_CTX_free(context);
        return {};
      }
      EVP_MD_CTX_free(context);
      static constexpr char digits[] = "0123456789abcdef";
      std::string result;
      result.reserve(length * 2);
      for (unsigned int index = 0; index < length; ++index) {
        result.push_back(digits[digest[index] >> 4]);
        result.push_back(digits[digest[index] & 0x0f]);
      }
      return result;
    }

    std::string
    lower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    }

    // The paths the host hands over are UTF-8 (as everywhere in the host): a path made of the bytes with the constructor of a string is read in the code page of the file system.
    std::filesystem::path
    from_utf8(const std::string &text) {
      return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    bool
    regular_file(const std::filesystem::path &path) {
      std::error_code error;
      return std::filesystem::is_regular_file(path, error);
    }

    std::string
    url_for(const app_t &app, orientation_e orientation) {
      return "/appasset?appid=" + app.id + "&type=" + (orientation == orientation_e::portrait ? "poster" : "hero");
    }

    // a path as the person or an older version wrote it: no quotes, forward slashes, lower case
    std::string
    normalized(std::string value) {
      const auto first = value.find_first_not_of(" \t\"");
      if (first == std::string::npos) return {};
      value = value.substr(first, value.find_last_not_of(" \t\"") - first + 1);
      std::replace(value.begin(), value.end(), '\\', '/');
      return lower(std::move(value));
    }

    bool
    same_file(const std::string &left, const std::string &right) {
      if (left.empty() || right.empty()) return false;
      std::error_code error;
      if (std::filesystem::equivalent(from_utf8(left), from_utf8(right), error) && !error) return true;
      return normalized(left) == normalized(right);
    }

    // The placeholder is the host's own assets/box.png. The default Xbox entry writes it as "box.png" and older versions as "./assets/box.png";
    // an absolute path counts only when it is that very file: a picture of the person's that is called box.png somewhere else is a poster.
    bool
    is_box_placeholder(const app_t &app) {
      auto configured = normalized(app.image_path);
      if (configured.empty()) return true;
      while (configured.rfind("./", 0) == 0) configured.erase(0, 2);
      if (configured.rfind("assets/", 0) == 0) configured.erase(0, 7);
      if (configured == "box.png") return true;
      if (same_file(app.image_path, app.placeholder_path)) return true;
      // the poster lookup fell back to the host's picture: the configured file is not there (or is not an image the host can use)
      return same_file(app.poster_path, app.placeholder_path);
    }

    bool
    has_real_poster(const app_t &app) {
      if (app.poster_pending) return true;
      if (is_box_placeholder(app)) return app.xbox_app;
      return regular_file(from_utf8(app.poster_path));
    }

#ifdef _WIN32

    using Microsoft::WRL::ComPtr;

    struct com_scope_t {
      HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      ~com_scope_t() {
        if (SUCCEEDED(result)) CoUninitialize();
      }
      explicit
      operator bool() const noexcept { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
    };

    std::wstring
    wide_from_utf8(std::string_view value) {
      if (value.empty()) return {};
      const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (length <= 0) return {};
      std::wstring result(static_cast<std::size_t>(length), L'\0');
      if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length) != length) return {};
      return result;
    }

    std::wstring
    first_command_argument(std::string_view command) {
      std::string value(command);
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
      if (value.empty()) return {};
      if (value.front() == '"') {
        const auto end = value.find('"', 1);
        return wide_from_utf8(value.substr(1, end == std::string::npos ? std::string::npos : end - 1));
      }
      const auto end = value.find_first_of(" \t");
      return wide_from_utf8(value.substr(0, end == std::string::npos ? std::string::npos : end));
    }

    // the programs that start other programs: their icon says nothing about the application, so the cover falls back to the tonal style
    bool
    generic_launcher(std::string name) {
      if (name.size() > 4 && name.ends_with(".exe")) name.resize(name.size() - 4);
      static constexpr std::array names {
        "cmd", "powershell", "powershell_ise", "pwsh", "explorer", "rundll32", "notepad", "conhost", "wscript", "cscript", "mshta",
        "regsvr32", "msiexec", "wt", "wsl", "bash", "control", "runas", "schtasks", "taskkill", "curl", "start", "sc", "net", "timeout",
      };
      return std::find(names.begin(), names.end(), name) != names.end();
    }

    // the built-in Desktop application streams the desktop: it may show the icon of Explorer
    bool
    is_desktop_app(const app_t &app) {
      const auto image = lower(app.image_path);
      return image == "desktop" || image == "desktop.png";
    }

    std::optional<std::filesystem::path>
    resolve_executable(const app_t &app) {
      const bool desktop = is_desktop_app(app);
      const auto usable = [desktop](const std::string &file_name) {
        const auto name = lower(file_name);
        return !generic_launcher(name) || (desktop && name == "explorer.exe");
      };
      std::vector<std::string> commands;
      if (!app.command.empty()) commands.push_back(app.command);
      commands.insert(commands.end(), app.detached.begin(), app.detached.end());
      for (const auto &command : commands) {
        // a command that opens an address (steam://..., shell:..., start xbox:) has no program of its own; an address among the arguments of a program (steam.exe steam://...) does not matter
        const auto argument = first_command_argument(command);
        if (argument.empty()) continue;
        const auto lower_program = lower(std::filesystem::path(argument).string());
        if (lower_program.find("://") != std::string::npos || lower_program.starts_with("shell:") || lower(command).find("start xbox:") != std::string::npos) continue;
        if (!usable(std::filesystem::path(argument).filename().string())) continue;

        std::filesystem::path candidate(argument);
        if (!candidate.is_absolute() && !app.working_dir.empty()) candidate = std::filesystem::path(wide_from_utf8(app.working_dir)) / candidate;
        if (!candidate.is_absolute()) {
          wchar_t resolved[MAX_PATH] {};
          const auto length = SearchPathW(nullptr, candidate.c_str(), nullptr, ARRAYSIZE(resolved), resolved, nullptr);
          if (length > 0 && length < ARRAYSIZE(resolved)) candidate = resolved;
        }
        std::error_code error;
        candidate = std::filesystem::weakly_canonical(candidate, error);
        if (!error && usable(candidate.filename().string()) && std::filesystem::is_regular_file(candidate, error)) return candidate;
      }
      if (desktop) {
        wchar_t windows[MAX_PATH] {};
        const auto length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
        if (length > 0 && length < ARRAYSIZE(windows)) {
          std::error_code error;
          const auto explorer = std::filesystem::path(windows) / L"explorer.exe";
          if (std::filesystem::is_regular_file(explorer, error)) return explorer;
        }
      }
      return std::nullopt;
    }

    std::int64_t
    file_mtime(const std::filesystem::path &path) {
      std::error_code error;
      const auto value = std::filesystem::last_write_time(path, error);
      return error ? 0 : value.time_since_epoch().count();
    }

    std::optional<ComPtr<IWICBitmap>>
    extract_icon(IWICImagingFactory *imaging, const std::filesystem::path &executable) {
      const auto path = executable.wstring();
      if (ExtractIconExW(path.c_str(), -1, nullptr, nullptr, 0) == 0) return std::nullopt;

      ComPtr<IShellItem> item;
      if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return std::nullopt;
      ComPtr<IShellItemImageFactory> factory;
      if (FAILED(item.As(&factory))) return std::nullopt;
      HBITMAP bitmap = nullptr;
      const SIZE requested { 256, 256 };
      if (FAILED(factory->GetImage(requested, static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK), &bitmap)) || bitmap == nullptr) return std::nullopt;
      ComPtr<IWICBitmap> result;
      const auto status = imaging->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &result);
      DeleteObject(bitmap);
      if (FAILED(status)) return std::nullopt;
      return result;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // Drawing. The six styles follow the cover design (colours are Material 3 HCT tonal colours of the hue of the name's FNV hash; gradients, grain, glass
    // and text follow the CSS of the design), drawn with Direct2D over a WIC bitmap. Everything is deterministic: the same name gives the same cover.
    // ---------------------------------------------------------------------------------------------------------------------

    using palette::spec_e;

    D2D1_COLOR_F
    rgb(std::uint32_t value, float alpha = 1.0f) {
      return D2D1::ColorF(static_cast<float>((value >> 16) & 0xff) / 255.0f, static_cast<float>((value >> 8) & 0xff) / 255.0f, static_cast<float>(value & 0xff) / 255.0f, alpha);
    }

    D2D1_COLOR_F
    tone(spec_e spec, int hue, float alpha = 1.0f) {
      return rgb(palette::hct(spec, hue), alpha);
    }

    D2D1_COLOR_F
    with_alpha(const D2D1_COLOR_F &colour, float alpha) {
      return D2D1::ColorF(colour.r, colour.g, colour.b, alpha);
    }

    ComPtr<ID2D1SolidColorBrush>
    solid(ID2D1RenderTarget *target, const D2D1_COLOR_F &colour) {
      ComPtr<ID2D1SolidColorBrush> brush;
      target->CreateSolidColorBrush(colour, &brush);
      return brush;
    }

    void
    fill(ID2D1RenderTarget *target, const D2D1_RECT_F &box, const D2D1_COLOR_F &colour) {
      if (const auto brush = solid(target, colour)) target->FillRectangle(box, brush.Get());
    }

    void
    fill_rounded(ID2D1RenderTarget *target, const D2D1_RECT_F &box, float radius, const D2D1_COLOR_F &colour) {
      if (const auto brush = solid(target, colour)) target->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush.Get());
    }

    void
    fill_ellipse(ID2D1RenderTarget *target, float cx, float cy, float rx, float ry, const D2D1_COLOR_F &colour) {
      if (const auto brush = solid(target, colour)) target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush.Get());
    }

    struct stop_t {
      float at;
      D2D1_COLOR_F colour;
    };

    ComPtr<ID2D1GradientStopCollection>
    stops_of(ID2D1RenderTarget *target, std::initializer_list<stop_t> stops) {
      std::vector<D2D1_GRADIENT_STOP> list;
      for (const auto &stop : stops) list.push_back(D2D1_GRADIENT_STOP { stop.at, stop.colour });
      ComPtr<ID2D1GradientStopCollection> collection;
      // colours are interpolated as they are written, like CSS does (not in linear light)
      target->CreateGradientStopCollection(list.data(), static_cast<UINT32>(list.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection);
      return collection;
    }

    /// linear-gradient(<degrees>, stops...) over a box, with the gradient line of CSS Images (the angle, the length that touches the corners)
    void
    fill_linear(ID2D1RenderTarget *target, const D2D1_RECT_F &box, float degrees, std::initializer_list<stop_t> stops) {
      const float width = box.right - box.left, height = box.bottom - box.top;
      const float radians = degrees * 3.14159265f / 180.0f;
      const float dx = std::sin(radians), dy = -std::cos(radians);
      const float length = std::fabs(width * dx) + std::fabs(height * dy);
      const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
      const auto collection = stops_of(target, stops);
      if (!collection) return;
      ComPtr<ID2D1LinearGradientBrush> brush;
      if (FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - dx * length / 2, cy - dy * length / 2), D2D1::Point2F(cx + dx * length / 2, cy + dy * length / 2)), collection.Get(), &brush))) return;
      target->FillRectangle(box, brush.Get());
    }

    /// radial-gradient(<rx> <ry> at <cx> <cy>, stops...) with stop positions as fractions of the radii
    void
    fill_radial(ID2D1RenderTarget *target, const D2D1_RECT_F &box, float cx, float cy, float rx, float ry, std::initializer_list<stop_t> stops) {
      const auto collection = stops_of(target, stops);
      if (!collection) return;
      ComPtr<ID2D1RadialGradientBrush> brush;
      if (FAILED(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry), collection.Get(), &brush))) return;
      target->FillRectangle(box, brush.Get());
    }

    // ---- text ----

    struct font_t {
      const wchar_t *family;
      DWRITE_FONT_WEIGHT weight;
      bool serif;  ///< Han characters come from a serif face (SimSun): the system has no heavy one
    };
    // CSS weights 800 and 900 are both the Black face of the design's sans (Noto Sans CJK has no 800); 700 is Bold. Windows: Segoe UI Black, Segoe UI Bold, and the
    // fallback faces of Microsoft YaHei UI (Bold is the heaviest it has) for Han, kana and hangul.
    constexpr font_t kSansHeavy { L"Segoe UI", DWRITE_FONT_WEIGHT_BLACK, false };
    constexpr font_t kSansBold { L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, false };
    constexpr font_t kSerifHeavy { L"Georgia", DWRITE_FONT_WEIGHT_BOLD, true };

    struct text_env_t {
      IDWriteFactory *write = nullptr;
      ComPtr<IDWriteFontFallback> serif_fallback;
    };

    ComPtr<IDWriteFontFallback>
    make_serif_fallback(IDWriteFactory *write) {
      ComPtr<IDWriteFactory2> factory2;
      if (FAILED(write->QueryInterface(IID_PPV_ARGS(&factory2)))) return nullptr;
      ComPtr<IDWriteFontFallbackBuilder> builder;
      if (FAILED(factory2->CreateFontFallbackBuilder(&builder))) return nullptr;
      // Han and the punctuation of the CJK blocks come from SimSun (the one serif Chinese face every Windows has), before the system's own fallback
      const DWRITE_UNICODE_RANGE ranges[] = { { 0x3000, 0x303f }, { 0x3400, 0x4dbf }, { 0x4e00, 0x9fff }, { 0xff01, 0xff60 } };
      const WCHAR *families[] = { L"SimSun" };
      if (FAILED(builder->AddMapping(ranges, ARRAYSIZE(ranges), families, 1, nullptr, nullptr, nullptr, 1.0f))) return nullptr;
      ComPtr<IDWriteFontFallback> system;
      if (SUCCEEDED(factory2->GetSystemFontFallback(&system)) && system) builder->AddMappings(system.Get());
      ComPtr<IDWriteFontFallback> fallback;
      if (FAILED(builder->CreateFontFallback(&fallback))) return nullptr;
      return fallback;
    }

    ComPtr<IDWriteTextLayout>
    make_layout(const text_env_t &env, std::wstring_view text, const font_t &font, float size, float line_height, float spacing_em, float width, DWRITE_TEXT_ALIGNMENT align, bool wrap = true) {
      ComPtr<IDWriteTextFormat> format;
      if (FAILED(env.write->CreateTextFormat(font.family, nullptr, font.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return nullptr;
      format->SetTextAlignment(align);
      format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
      // CSS line-height: every line is as tall as line-height x size, the glyphs sit in the middle of it (half-leading); 0.436 em is where the fonts of the design (Noto CJK: ascent 1.16 em, descent 0.288 em) put the middle of the glyphs below the middle of the line; the layouts here use the same baseline, whatever face is chosen
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_height * size, (line_height * 0.5f + 0.436f) * size);
      if (font.serif && env.serif_fallback) {
        ComPtr<IDWriteTextFormat1> format1;
        if (SUCCEEDED(format.As(&format1))) format1->SetFontFallback(env.serif_fallback.Get());
      }
      ComPtr<IDWriteTextLayout> layout;
      if (FAILED(env.write->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), width, 100000.0f, &layout))) return nullptr;
      if (spacing_em != 0.0f) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1))) layout1->SetCharacterSpacing(0.0f, spacing_em * size, 0.0f, DWRITE_TEXT_RANGE { 0, static_cast<UINT32>(text.size()) });
      }
      return layout;
    }

    struct fitted_t {
      ComPtr<IDWriteTextLayout> layout;
      float size = 0;
      float width = 0;   ///< the width the lines were set in (narrower than the box when balanced)
      float height = 0;
      UINT32 lines = 0;
    };

    /// Is a word of Latin letters wider than the box at this size (a word that is cut inside by the wrapping)?
    bool
    word_overflows(const text_env_t &env, std::wstring_view text, const font_t &font, float size, float spacing_em, float width) {
      std::size_t start = 0;
      while (start < text.size()) {
        while (start < text.size() && (text[start] == L' ' || text[start] == L'\t')) ++start;
        auto end = start;
        bool latin = true;
        while (end < text.size() && text[end] != L' ' && text[end] != L'\t') {
          if (text[end] >= 0x2e80) latin = false;
          ++end;
        }
        if (latin && end > start) {
          if (const auto word = make_layout(env, text.substr(start, end - start), font, size, 1.0f, spacing_em, 100000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false)) {
            DWRITE_TEXT_METRICS metrics {};
            word->GetMetrics(&metrics);
            // a word that fills the line to the last pixel is cut by the wrapping, which counts the side bearings too: it needs a few pixels of room
            if (metrics.width > width - 4.0f) return true;
          }
        }
        start = end;
      }
      return false;
    }

    /// The `fit` of the design: the largest size (steps of 2) at which the text is at most max_height tall and no word is cut; when none fits down to 1.3 x min, the largest
    /// size at which the text fits while words may break; at the end min. With `balance` the lines are as even as they can be (text-wrap: balance).
    fitted_t
    fit_text(const text_env_t &env, std::wstring_view text, const font_t &font, float width, float max_height, float max_size, float min_size,
      float line_height, float spacing_em, DWRITE_TEXT_ALIGNMENT align, bool balance = true) {
      const auto lay = [&](std::wstring_view content, float size, float at_width) {
        fitted_t result;
        result.layout = make_layout(env, content, font, size, line_height, spacing_em, at_width, align);
        result.size = size;
        result.width = at_width;
        if (result.layout) {
          DWRITE_TEXT_METRICS metrics {};
          result.layout->GetMetrics(&metrics);
          result.height = metrics.height;
          result.lines = metrics.lineCount;
        }
        return result;
      };
      fitted_t chosen;
      bool found = false;
      for (float size = max_size; size >= min_size * 1.3f - 0.01f && !found; size -= 2.0f) {
        auto candidate = lay(text, size, width);
        if (candidate.layout && candidate.height <= max_height + 0.5f && !word_overflows(env, text, font, size, spacing_em, width)) {
          chosen = std::move(candidate);
          found = true;
        }
      }
      for (float size = max_size; size > min_size && !found; size -= 2.0f) {
        auto candidate = lay(text, size, width);
        if (candidate.layout && candidate.height <= max_height + 0.5f) {
          chosen = std::move(candidate);
          found = true;
        }
      }
      if (!found) chosen = lay(text, min_size, width);
      if (balance && chosen.layout && chosen.lines > 1 && chosen.lines <= 6) {
        // the narrowest box that keeps the number of lines and cuts no word
        float low = width * 0.3f, high = width;
        for (int step = 0; step < 12; ++step) {
          const float middle = (low + high) / 2;
          const auto probe = lay(text, chosen.size, middle);
          if (probe.layout && probe.lines == chosen.lines && !word_overflows(env, text, font, chosen.size, spacing_em, middle)) high = middle;
          else low = middle;
        }
        if (auto balanced = lay(text, chosen.size, high); balanced.layout && balanced.lines == chosen.lines) chosen = std::move(balanced);
        // a line that ends after the fullwidth punctuation reads better than one that ends a character later ("崩坏：" / "星穹铁道")
        UINT32 line_count = 0;
        chosen.layout->GetLineMetrics(nullptr, 0, &line_count);
        std::vector<DWRITE_LINE_METRICS> lines(line_count);
        if (line_count > 1 && SUCCEEDED(chosen.layout->GetLineMetrics(lines.data(), line_count, &line_count))) {
          const std::size_t first_line = lines.front().length;
          for (std::size_t at = first_line > 1 ? first_line - 2 : 0; at < first_line && at < text.size(); ++at) {
            const wchar_t mark = text[at];
            if ((mark == L'：' || mark == L'，' || mark == L'；' || mark == L'、' || mark == L'。') && at + 1 < first_line) {
              std::wstring broken(text.substr(0, at + 1));
              broken.push_back(L'\n');
              broken.append(text.substr(at + 1));
              if (auto moved = lay(broken, chosen.size, chosen.width); moved.layout && moved.lines == chosen.lines && moved.height <= max_height + 0.5f) chosen = std::move(moved);
              break;
            }
          }
        }
      }
      return chosen;
    }

    /// the width of a line of text at a size, and the largest size (steps of 4, from max to min) at which it is at most width wide (`fitw` of the design)
    fitted_t
    fit_line(const text_env_t &env, std::wstring_view text, const font_t &font, float width, float max_size, float min_size, float line_height) {
      fitted_t chosen;
      float size = max_size;
      for (; size > min_size; size -= 4.0f) {
        chosen.layout = make_layout(env, text, font, size, line_height, 0.0f, 100000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        DWRITE_TEXT_METRICS metrics {};
        if (chosen.layout) chosen.layout->GetMetrics(&metrics);
        if (chosen.layout && metrics.width <= width) {
          chosen.size = size;
          chosen.width = metrics.width;
          chosen.height = metrics.height;
          return chosen;
        }
      }
      chosen.layout = make_layout(env, text, font, min_size, line_height, 0.0f, 100000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false);
      DWRITE_TEXT_METRICS metrics {};
      if (chosen.layout) chosen.layout->GetMetrics(&metrics);
      chosen.size = min_size;
      chosen.width = metrics.width;
      chosen.height = metrics.height;
      return chosen;
    }

    void
    draw_layout(ID2D1RenderTarget *target, IDWriteTextLayout *layout, float x, float y, const D2D1_COLOR_F &colour) {
      if (layout == nullptr) return;
      if (const auto brush = solid(target, colour)) target->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    /// a fitted block of text in a box [left, left + width]: where its layout begins, for the alignment of the box (a balanced block is narrower than the box)
    float
    block_x(float left, float width, const fitted_t &text, DWRITE_TEXT_ALIGNMENT align) {
      if (align == DWRITE_TEXT_ALIGNMENT_CENTER) return left + (width - text.width) / 2;
      if (align == DWRITE_TEXT_ALIGNMENT_TRAILING) return left + width - text.width;
      return left;
    }

    // ---- grain (feTurbulence noise, desaturated, multiplied): a fine darkening noise over the whole cover ----
    void
    draw_grain(ID2D1RenderTarget *target, float width, float height, float opacity) {
      const UINT columns = static_cast<UINT>(width), rows = static_cast<UINT>(height);
      std::vector<std::uint32_t> pixels(static_cast<std::size_t>(columns) * rows);
      std::uint32_t state = 0x9e3779b9u;
      const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state & 0xffffff) / 16777215.0f;
      };
      for (auto &pixel : pixels) {
        // the noise of the design is grey around the middle with a small spread and an alpha of about one half; multiplied at `opacity` it darkens by opacity x 0.5 x (1 - grey)
        const float grey = 0.5f + (next() - 0.5f) * 0.4f;
        const auto alpha = static_cast<std::uint32_t>(std::lround(255.0f * opacity * 0.5f * (1.0f - grey)));
        pixel = alpha << 24;
      }
      ComPtr<ID2D1Bitmap> bitmap;
      if (FAILED(target->CreateBitmap(D2D1::SizeU(columns, rows), pixels.data(), columns * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap))) return;
      target->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0, width, height), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // ---- pixels on the CPU: the blurs the render target cannot do (Direct2D effects need a device context, a WIC target has none) ----
    struct pixmap_t {
      UINT width = 0, height = 0;
      std::vector<float> px;  ///< premultiplied RGBA, 0..1
    };

    pixmap_t
    pixmap_from_bgra(const std::vector<std::uint8_t> &bytes, UINT width, UINT height) {
      pixmap_t result;
      result.width = width;
      result.height = height;
      result.px.resize(static_cast<std::size_t>(width) * height * 4);
      for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index) {
        result.px[index * 4 + 0] = bytes[index * 4 + 2] / 255.0f;
        result.px[index * 4 + 1] = bytes[index * 4 + 1] / 255.0f;
        result.px[index * 4 + 2] = bytes[index * 4 + 0] / 255.0f;
        result.px[index * 4 + 3] = bytes[index * 4 + 3] / 255.0f;
      }
      return result;
    }

    std::optional<pixmap_t>
    pixmap_from_wic(IWICBitmapSource *source) {
      UINT width = 0, height = 0;
      if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0) return std::nullopt;
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
      if (FAILED(source->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes.size()), bytes.data()))) return std::nullopt;
      return pixmap_from_bgra(bytes, width, height);
    }

    /// box average down to a smaller size (premultiplied, so a transparent edge does not bleed colour)
    pixmap_t
    downscale(const pixmap_t &from, UINT width, UINT height) {
      pixmap_t to;
      to.width = width;
      to.height = height;
      to.px.assign(static_cast<std::size_t>(width) * height * 4, 0.0f);
      for (UINT y = 0; y < height; ++y) {
        const UINT y0 = static_cast<UINT>(static_cast<std::uint64_t>(y) * from.height / height), y1 = std::max(y0 + 1, static_cast<UINT>(static_cast<std::uint64_t>(y + 1) * from.height / height));
        for (UINT x = 0; x < width; ++x) {
          const UINT x0 = static_cast<UINT>(static_cast<std::uint64_t>(x) * from.width / width), x1 = std::max(x0 + 1, static_cast<UINT>(static_cast<std::uint64_t>(x + 1) * from.width / width));
          float sum[4] {};
          for (UINT yy = y0; yy < std::min(y1, from.height); ++yy) {
            for (UINT xx = x0; xx < std::min(x1, from.width); ++xx) {
              for (int channel = 0; channel < 4; ++channel) sum[channel] += from.px[(static_cast<std::size_t>(yy) * from.width + xx) * 4 + channel];
            }
          }
          const float count = static_cast<float>((std::min(y1, from.height) - y0) * (std::min(x1, from.width) - x0));
          for (int channel = 0; channel < 4; ++channel) to.px[(static_cast<std::size_t>(y) * width + x) * 4 + channel] = sum[channel] / std::max(1.0f, count);
        }
      }
      return to;
    }

    /// separable Gaussian blur; outside the picture is transparent (a CSS filter blur) or the edge pixel repeats (a backdrop)
    void
    gaussian_blur(pixmap_t &image, float sigma, bool clamp_edges) {
      if (sigma < 0.3f) return;
      const int radius = static_cast<int>(std::ceil(sigma * 3));
      std::vector<float> kernel(static_cast<std::size_t>(radius) * 2 + 1);
      float total = 0;
      for (int index = -radius; index <= radius; ++index) total += kernel[static_cast<std::size_t>(index + radius)] = std::exp(-(index * index) / (2 * sigma * sigma));
      for (auto &value : kernel) value /= total;
      std::vector<float> scratch(image.px.size());
      for (int pass = 0; pass < 2; ++pass) {
        const int span = pass == 0 ? static_cast<int>(image.width) : static_cast<int>(image.height);
        const int lines = pass == 0 ? static_cast<int>(image.height) : static_cast<int>(image.width);
        for (int line = 0; line < lines; ++line) {
          for (int position = 0; position < span; ++position) {
            float sum[4] {};
            for (int offset = -radius; offset <= radius; ++offset) {
              int at = position + offset;
              if (at < 0 || at >= span) {
                if (!clamp_edges) continue;
                at = std::clamp(at, 0, span - 1);
              }
              const std::size_t index = pass == 0 ? (static_cast<std::size_t>(line) * image.width + at) * 4 : (static_cast<std::size_t>(at) * image.width + line) * 4;
              const float weight = kernel[static_cast<std::size_t>(offset + radius)];
              for (int channel = 0; channel < 4; ++channel) sum[channel] += image.px[index + channel] * weight;
            }
            const std::size_t out = pass == 0 ? (static_cast<std::size_t>(line) * image.width + position) * 4 : (static_cast<std::size_t>(position) * image.width + line) * 4;
            for (int channel = 0; channel < 4; ++channel) scratch[out + channel] = sum[channel];
          }
        }
        image.px.swap(scratch);
      }
    }

    /// CSS filter saturate() and brightness() on premultiplied pixels
    void
    adjust_colours(pixmap_t &image, float saturation, float brightness) {
      for (std::size_t index = 0; index < static_cast<std::size_t>(image.width) * image.height; ++index) {
        float *pixel = &image.px[index * 4];
        const float alpha = pixel[3];
        if (alpha <= 0.0001f) continue;
        const float r = pixel[0] / alpha, g = pixel[1] / alpha, b = pixel[2] / alpha;
        const float s = saturation;
        float nr = (0.213f + 0.787f * s) * r + (0.715f - 0.715f * s) * g + (0.072f - 0.072f * s) * b;
        float ng = (0.213f - 0.213f * s) * r + (0.715f + 0.285f * s) * g + (0.072f - 0.072f * s) * b;
        float nb = (0.213f - 0.213f * s) * r + (0.715f - 0.715f * s) * g + (0.072f + 0.928f * s) * b;
        pixel[0] = std::clamp(nr * brightness, 0.0f, 1.0f) * alpha;
        pixel[1] = std::clamp(ng * brightness, 0.0f, 1.0f) * alpha;
        pixel[2] = std::clamp(nb * brightness, 0.0f, 1.0f) * alpha;
      }
    }

    ComPtr<ID2D1Bitmap>
    bitmap_from_pixmap(ID2D1RenderTarget *target, const pixmap_t &image) {
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(image.width) * image.height * 4);
      for (std::size_t index = 0; index < static_cast<std::size_t>(image.width) * image.height; ++index) {
        const auto channel = [&](int at) { return static_cast<std::uint8_t>(std::lround(std::clamp(image.px[index * 4 + at], 0.0f, 1.0f) * 255.0f)); };
        bytes[index * 4 + 0] = channel(2);
        bytes[index * 4 + 1] = channel(1);
        bytes[index * 4 + 2] = channel(0);
        bytes[index * 4 + 3] = channel(3);
      }
      ComPtr<ID2D1Bitmap> bitmap;
      target->CreateBitmap(D2D1::SizeU(image.width, image.height), bytes.data(), image.width * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap);
      return bitmap;
    }

    // ---- the covers ----

    struct canvas_t {
      ID2D1RenderTarget *target = nullptr;
      ID2D1Factory *d2d = nullptr;
      IWICBitmap *surface = nullptr;
      const text_env_t *text = nullptr;
      float width = 0, height = 0;
      bool landscape = false;
      std::wstring name;
      std::wstring monogram;
      std::uint32_t hash = 0;
      int hue = 0;
      const std::optional<ComPtr<IWICBitmap>> *icon = nullptr;
      mutable bool drawn = true;  ///< (the drawing functions take the canvas as const) false when an EndDraw reported an error: such a canvas is not encoded or cached
    };

    D2D1_RECT_F
    rect_of(float left, float top, float right, float bottom) {
      return D2D1::RectF(left, top, right, bottom);
    }

    /// The title of the styles that stand at the bottom: set in the box and bottom-aligned (the block grows upwards, as `bottom: Npx` does)
    fitted_t
    title_block(const canvas_t &c, const font_t &font, float left, float width, float max_height, float max_size, float min_size, float line_height, float spacing_em, DWRITE_TEXT_ALIGNMENT align) {
      return fit_text(*c.text, c.name, font, width, max_height, max_size, min_size, line_height, spacing_em, align);
    }

    // A: tonal gradient, monogram chip, dash and a big title
    void
    draw_tonal(const canvas_t &c) {
      const int h = c.hue;
      const float s = c.landscape ? 1.0f : 0.0f;
      const float chip_left = c.landscape ? 96 : 44, chip_top = c.landscape ? 96 : 92, chip = c.landscape ? 160 : 128, chip_radius = c.landscape ? 50 : 40, chip_font = c.landscape ? 78 : 62;
      const float title_left = c.landscape ? 96 : 44, title_width = c.landscape ? 1500 : 512, bottom = c.landscape ? 108 : 92;
      const float dash_w = c.landscape ? 96 : 72, dash_h = c.landscape ? 12 : 10, dash_gap = c.landscape ? 36 : 30;
      (void) s;
      const auto box = rect_of(0, 0, c.width, c.height);
      fill_linear(c.target, box, 160, { { 0, tone(palette::C52T38, h) }, { 1, tone(palette::C40T22, h) } });
      const auto glow = tone(palette::C60T56, h);
      fill_radial(c.target, box, 0, 0, c.width * 1.3f * 0.62f, c.height * 0.7f * 0.62f, { { 0, with_alpha(glow, 0.5f) }, { 1, with_alpha(glow, 0.0f) } });
      fill_rounded(c.target, rect_of(chip_left, chip_top, chip_left + chip, chip_top + chip), chip_radius, tone(palette::C30T90, h));
      if (const auto mono = make_layout(*c.text, c.monogram, kSansHeavy, chip_font, chip / chip_font, 0.0f, chip, DWRITE_TEXT_ALIGNMENT_CENTER)) draw_layout(c.target, mono.Get(), chip_left, chip_top, tone(palette::C50T22, h));
      const auto title = title_block(c, kSansHeavy, title_left, title_width, c.landscape ? 560 : 420, c.landscape ? 156 : 108, c.landscape ? 56 : 40, 1.06f, -0.01f, DWRITE_TEXT_ALIGNMENT_LEADING);
      const float title_top = c.height - bottom - title.height;
      fill_rounded(c.target, rect_of(title_left, title_top - dash_gap - dash_h, title_left + dash_w, title_top - dash_gap), dash_h / 2, tone(palette::C60T82, h + 55));
      draw_layout(c.target, title.layout.Get(), block_x(title_left, title_width, title, DWRITE_TEXT_ALIGNMENT_LEADING), title_top, tone(palette::C4T98, h));
    }

    // B: monogram poster. Centred (owner): the circle with the monogram, the dash and the name are one group in the middle of the cover.
    void
    draw_monogram(const canvas_t &c) {
      const int h = c.hue;
      const float circle = c.landscape ? 520 : 480, gap = c.landscape ? 52 : 44, dash_w = c.landscape ? 120 : 96, dash_h = c.landscape ? 8 : 6, dash_gap = c.landscape ? 30 : 24;
      const float title_width = c.landscape ? 1500 : 504, title_max_height = c.landscape ? 240 : 200;
      fill(c.target, rect_of(0, 0, c.width, c.height), tone(palette::C12T94, h));
      const auto title = fit_text(*c.text, c.name, kSansBold, title_width, title_max_height, c.landscape ? 84 : 64, c.landscape ? 44 : 38, 1.12f, 0.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
      const float group = circle + gap + dash_h + dash_gap + title.height;
      const float top = (c.height - group) / 2;
      const float cx = c.width / 2, cy = top + circle / 2;
      fill_ellipse(c.target, cx, cy, circle / 2, circle / 2, tone(palette::C34T86, h));
      draw_grain(c.target, c.width, c.height, 0.2f);
      const auto ink = tone(palette::C60T34, h);
      const auto mono = fit_line(*c.text, c.monogram, kSansHeavy, 0.0f, c.landscape ? 150 : 120, c.landscape ? 150 : 120, 1.15f);
      draw_layout(c.target, mono.layout.Get(), cx - mono.width / 2, cy - mono.height / 2, ink);
      const float dash_top = top + circle + gap;
      fill(c.target, rect_of(cx - dash_w / 2, dash_top, cx + dash_w / 2, dash_top + dash_h), ink);
      draw_layout(c.target, title.layout.Get(), block_x((c.width - title_width) / 2, title_width, title, DWRITE_TEXT_ALIGNMENT_CENTER), dash_top + dash_h + dash_gap, tone(palette::C24T16, h));
    }

    // C: the icon of the program, large, over its own colours spread soft; the name under it
    void
    draw_icon(const canvas_t &c) {
      const auto &icon = *c.icon;
      fill(c.target, rect_of(0, 0, c.width, c.height), rgb(0x1b1b1f));
      auto source = pixmap_from_wic(icon->Get());
      if (!source) return;
      const float back = c.landscape ? 2400.0f : 1440.0f;
      const D2D1_RECT_F back_rect = c.landscape ? rect_of(-240, -660, -240 + back, -660 + back) : rect_of(-420, -300, -420 + back, -300 + back);
      {
        // blur(90px) saturate(1.7) brightness(0.8) of the icon drawn 1440 px wide, worked on a 64 x 64 copy (sigma 90 px = 4 px there)
        auto soft = downscale(*source, 64, 64);
        gaussian_blur(soft, 90.0f * 64.0f / back, false);
        adjust_colours(soft, 1.7f, 0.8f);
        if (const auto bitmap = bitmap_from_pixmap(c.target, soft)) c.target->DrawBitmap(bitmap.Get(), back_rect, 0.95f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
      fill_linear(c.target, rect_of(0, 0, c.width, c.height), 180, { { 0, D2D1::ColorF(0, 0, 0, 0.05f) }, { 0.42f, D2D1::ColorF(0, 0, 0, 0.2f) }, { 1, D2D1::ColorF(0, 0, 0, 0.62f) } });
      const float size = c.landscape ? 400.0f : 320.0f;
      const float left = (c.width - size) / 2, top = c.landscape ? 120.0f : 196.0f;
      {
        // drop-shadow(0 22px 28px rgba(0,0,0,.45)): the alpha of the icon, blurred (sigma 14 px), moved down 22 px
        const float scale = size / 256.0f;
        const float pad = 90.0f;  // px of the cover around the icon that the blur may reach
        const UINT cells = 160;
        const float reach = size + pad * 2;
        auto shadow = downscale(*source, static_cast<UINT>(size / reach * cells), static_cast<UINT>(size / reach * cells));
        pixmap_t padded;
        padded.width = padded.height = cells;
        padded.px.assign(static_cast<std::size_t>(cells) * cells * 4, 0.0f);
        const UINT inner = shadow.width, offset = (cells - inner) / 2;
        for (UINT y = 0; y < inner; ++y) {
          for (UINT x = 0; x < inner; ++x) {
            const float alpha = shadow.px[(static_cast<std::size_t>(y) * inner + x) * 4 + 3] * 0.45f;
            padded.px[(static_cast<std::size_t>(y + offset) * cells + x + offset) * 4 + 3] = alpha;
          }
        }
        gaussian_blur(padded, 14.0f * cells / reach, false);
        if (const auto bitmap = bitmap_from_pixmap(c.target, padded)) c.target->DrawBitmap(bitmap.Get(), rect_of(left - pad, top - pad + 22, left - pad + reach, top - pad + 22 + reach), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        (void) scale;
      }
      if (const auto bitmap = [&]() { ComPtr<ID2D1Bitmap> value; c.target->CreateBitmapFromWicBitmap(icon->Get(), nullptr, &value); return value; }()) {
        c.target->DrawBitmap(bitmap.Get(), rect_of(left, top, left + size, top + size), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      }
      const float title_left = 44, title_width = c.width - 88, bottom = c.landscape ? 110 : 92;
      const auto title = fit_text(*c.text, c.name, kSansBold, title_width, c.landscape ? 240 : 190, c.landscape ? 96 : 72, c.landscape ? 44 : 38, 1.1f, 0.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
      const float title_top = c.height - bottom - title.height;
      const float x = block_x(title_left, title_width, title, DWRITE_TEXT_ALIGNMENT_CENTER);
      // text-shadow: 0 2px 14px rgba(0,0,0,.35): a soft dark halo, then the name
      struct halo_t {
        float dx, dy, alpha;
      };
      static constexpr std::array<halo_t, 5> kHalo { { { 0, 2, 0.14f }, { 0, 6, 0.10f }, { 0, -3, 0.07f }, { 4, 4, 0.06f }, { -4, 4, 0.06f } } };
      for (const auto &halo : kHalo) draw_layout(c.target, title.layout.Get(), x + halo.dx, title_top + halo.dy, D2D1::ColorF(0, 0, 0, halo.alpha));
      draw_layout(c.target, title.layout.Get(), x, title_top, D2D1::ColorF(1, 1, 1, 1));
    }

    // D: editorial. A serif title, a double rule, a dotted panel and a sticker that carries the monogram.
    void
    draw_editorial(const canvas_t &c) {
      const int h = c.hue;
      const auto ink = tone(palette::C8T10, h);
      const float margin = c.landscape ? 96 : 48;
      const float title_top = c.landscape ? 80 : 90, title_width = c.width - margin * 2;
      const float rule = c.landscape ? 560 : 510;
      const float panel_top = c.landscape ? 604 : 544, panel_bottom = c.height - (c.landscape ? 100 : 84);
      fill(c.target, rect_of(0, 0, c.width, c.height), tone(palette::C10T94, h));
      draw_grain(c.target, c.width, c.height, 0.16f);
      const auto title = fit_text(*c.text, c.name, kSerifHeavy, title_width, c.landscape ? 420 : 400, c.landscape ? 200 : 116, c.landscape ? 70 : 46, 1.02f, -0.015f, DWRITE_TEXT_ALIGNMENT_LEADING);
      draw_layout(c.target, title.layout.Get(), block_x(margin, title_width, title, DWRITE_TEXT_ALIGNMENT_LEADING), title_top, ink);
      fill(c.target, rect_of(margin, rule, c.width - margin, rule + 3), ink);
      fill(c.target, rect_of(margin, rule + 10, c.width - margin, rule + 11), ink);
      const auto panel = rect_of(margin, panel_top, c.width - margin, panel_bottom);
      fill(c.target, panel, tone(palette::C34T82, h));
      c.target->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      const auto dot = tone(palette::C40T66, h, 0.55f);
      if (const auto brush = solid(c.target, dot)) {
        for (float y = panel_top + 11; y < panel_bottom + 11; y += 22) {
          for (float x = margin + 11; x < c.width - margin + 11; x += 22) c.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 2.8f, 2.8f), brush.Get());
        }
      }
      c.target->PopAxisAlignedClip();
      const float size = c.landscape ? 250 : 210, right = c.landscape ? 60 : 30, bottom = c.landscape ? 28 : 24;
      const float sx = c.width - margin - right - size, sy = panel_bottom - bottom - size;
      const float cx = sx + size / 2, cy = sy + size / 2;
      const int kind = static_cast<int>((c.hash >> 9) % 3);
      c.target->SetTransform(D2D1::Matrix3x2F::Rotation(-11.0f, D2D1::Point2F(cx, cy)));
      const auto sticker = tone(palette::C56T90, h + 170);
      if (kind == 0) {
        fill_ellipse(c.target, cx, cy, size / 2, size / 2, ink);
        fill_ellipse(c.target, cx, cy, size / 2 - 5, size / 2 - 5, sticker);
      }
      else if (kind == 1) {
        fill_rounded(c.target, rect_of(sx, sy, sx + size, sy + size), 34, ink);
        fill_rounded(c.target, rect_of(sx + 5, sy + 5, sx + size - 5, sy + size - 5), 29, sticker);
      }
      else {
        // a badge with twelve points (the clip-path of the design, in percent of the sticker)
        static constexpr float kBadge[][2] = { { 50, 0 }, { 61, 12 }, { 77, 7 }, { 82, 23 }, { 98, 28 }, { 91, 43 }, { 100, 57 }, { 86, 66 }, { 86, 82 }, { 70, 82 }, { 61, 96 }, { 50, 87 }, { 39, 96 }, { 30, 82 }, { 14, 82 }, { 14, 66 }, { 0, 57 }, { 9, 43 }, { 2, 28 }, { 18, 23 }, { 23, 7 }, { 39, 12 } };
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(c.d2d->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
          bool first = true;
          for (const auto &point : kBadge) {
            const auto at = D2D1::Point2F(sx + size * point[0] / 100, sy + size * point[1] / 100);
            if (first) sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
            else sink->AddLine(at);
            first = false;
          }
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          sink->Close();
          if (const auto brush = solid(c.target, sticker)) c.target->FillGeometry(geometry.Get(), brush.Get());
        }
      }
      if (const auto mono = fit_line(*c.text, c.monogram, kSerifHeavy, size - 24, c.landscape ? 112 : 96, 40, 1.0f); mono.layout) draw_layout(c.target, mono.layout.Get(), cx - mono.width / 2, cy - mono.height / 2, ink);
      c.target->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // E: colour blobs under a frosted glass panel (the backdrop of the panel is blurred and made more vivid), a huge monogram and the name
    void
    draw_glass(const canvas_t &c) {
      const int h = c.hue;
      const float w = c.width, ht = c.height;
      const float k = c.landscape ? 1.0f : 0.0f;
      (void) k;
      fill(c.target, rect_of(0, 0, w, ht), tone(palette::C40T24, h));
      const auto box = rect_of(0, 0, w, ht);
      const float r1 = c.landscape ? 640.0f : 420.0f, r2 = c.landscape ? 700.0f : 460.0f, r3 = c.landscape ? 670.0f : 440.0f;
      // background layers: the last one in the CSS list is the lowest
      fill_radial(c.target, box, w * 0.22f, ht * 0.92f, r3, r3, { { 0, tone(palette::C62T56, h - 60) }, { 0.7f, tone(palette::C62T56, h - 60, 0.0f) } });
      fill_radial(c.target, box, w * 0.92f, ht * 0.42f, r2, r2, { { 0, tone(palette::C66T62, h + 70) }, { 0.7f, tone(palette::C66T62, h + 70, 0.0f) } });
      fill_radial(c.target, box, w * 0.15f, ht * 0.14f, r1, r1, { { 0, tone(palette::C70T66, h) }, { 0.7f, tone(palette::C70T66, h, 0.0f) } });
      const float letter_size = c.landscape ? 440.0f : 330.0f, letter_top = c.landscape ? 90.0f : 128.0f;
      if (const auto mono = make_layout(*c.text, c.monogram, kSansHeavy, letter_size, 1.0f, 0.0f, 100000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, false)) {
        DWRITE_TEXT_METRICS metrics {};
        mono->GetMetrics(&metrics);
        const float x = (w - metrics.width) / 2;
        draw_layout(c.target, mono.Get(), x, letter_top + 10, D2D1::ColorF(0, 0, 0, 0.05f));  // text-shadow 0 10px 40px rgba(0,0,0,.18), as a soft hint
        draw_layout(c.target, mono.Get(), x, letter_top, D2D1::ColorF(1, 1, 1, 0.92f));
      }
      const float panel_left = c.landscape ? 100 : 34, panel_right = w - panel_left, panel_height = c.landscape ? 420 : 380, panel_bottom = ht - (c.landscape ? 90 : 70);
      const auto panel = rect_of(panel_left, panel_bottom - panel_height, panel_right, panel_bottom);
      const float radius = c.landscape ? 56 : 46;
      if (FAILED(c.target->EndDraw())) c.drawn = false;
      // the backdrop: what is under the panel, blurred (sigma 34 px) and saturated 1.5
      ComPtr<ID2D1Bitmap> blurred;
      float factor = 1.0f;
      if (const auto backdrop = pixmap_from_wic(c.surface)) {
        factor = c.landscape ? 8.0f : 4.0f;
        auto small = downscale(*backdrop, static_cast<UINT>(w / factor), static_cast<UINT>(ht / factor));
        gaussian_blur(small, 34.0f * (c.landscape ? 1.8f : 1.0f) / factor, true);
        adjust_colours(small, 1.5f, 1.0f);
        blurred = bitmap_from_pixmap(c.target, small);
      }
      c.target->BeginDraw();
      // box-shadow: 0 24px 60px rgba(0,0,0,.28): layers of a growing, fading rounded rectangle under the panel
      for (int layer = 0; layer < 14; ++layer) {
        const float grow = layer * 4.0f;
        fill_rounded(c.target, rect_of(panel.left - grow, panel.top - grow + 24, panel.right + grow, panel.bottom + grow + 24), radius + grow, D2D1::ColorF(0, 0, 0, 0.28f / 14.0f * (1.0f - layer / 15.0f) * 1.6f));
      }
      const auto rounded = D2D1::RoundedRect(panel, radius, radius);
      if (blurred) {
        ComPtr<ID2D1BitmapBrush> brush;
        if (SUCCEEDED(c.target->CreateBitmapBrush(blurred.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush))) {
          brush->SetTransform(D2D1::Matrix3x2F::Scale(factor, factor));
          c.target->FillRoundedRectangle(rounded, brush.Get());
        }
      }
      // background: linear-gradient(160deg, rgba(255,255,255,.22), rgba(20,18,36,.46) 60%) over the glass
      {
        const float radians = 160.0f * 3.14159265f / 180.0f;
        const float dx = std::sin(radians), dy = -std::cos(radians);
        const float pw = panel.right - panel.left, ph = panel.bottom - panel.top;
        const float length = std::fabs(pw * dx) + std::fabs(ph * dy);
        const float cx = (panel.left + panel.right) / 2, cy = (panel.top + panel.bottom) / 2;
        if (const auto collection = stops_of(c.target, { { 0, D2D1::ColorF(1, 1, 1, 0.22f) }, { 0.6f, D2D1::ColorF(20 / 255.0f, 18 / 255.0f, 36 / 255.0f, 0.46f) }, { 1, D2D1::ColorF(20 / 255.0f, 18 / 255.0f, 36 / 255.0f, 0.46f) } })) {
          ComPtr<ID2D1LinearGradientBrush> brush;
          if (SUCCEEDED(c.target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - dx * length / 2, cy - dy * length / 2), D2D1::Point2F(cx + dx * length / 2, cy + dy * length / 2)), collection.Get(), &brush))) {
            c.target->FillRoundedRectangle(rounded, brush.Get());
          }
        }
      }
      // border: 1.5px rgba(255,255,255,.42) and the inner highlight of the top edge
      if (const auto brush = solid(c.target, D2D1::ColorF(1, 1, 1, 0.42f))) c.target->DrawRoundedRectangle(D2D1::RoundedRect(rect_of(panel.left + 0.75f, panel.top + 0.75f, panel.right - 0.75f, panel.bottom - 0.75f), radius, radius), brush.Get(), 1.5f);
      if (const auto brush = solid(c.target, D2D1::ColorF(1, 1, 1, 0.34f))) c.target->DrawLine(D2D1::Point2F(panel.left + radius * 0.6f, panel.top + 1.5f), D2D1::Point2F(panel.right - radius * 0.6f, panel.top + 1.5f), brush.Get(), 1.0f);
      const float inset = c.landscape ? 60 : 34;
      const float title_left = panel.left + inset, title_width = panel.right - panel.left - inset * 2;
      const float title_bottom = c.landscape ? ht - 128 : ht - 98, title_height = c.landscape ? 340 : 320;
      const auto title = fit_text(*c.text, c.name, kSansBold, title_width, title_height, c.landscape ? 120 : 78, c.landscape ? 44 : 38, 1.1f, 0.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
      draw_layout(c.target, title.layout.Get(), title_left, title_bottom - title.height, D2D1::ColorF(1, 1, 1, 1));
    }

    // F: a grid of four shapes (the geometry of the design, chosen by bits of the hash) and the name on a band
    void
    draw_geometric(const canvas_t &c) {
      const int h = c.hue;
      const std::array<D2D1_COLOR_F, 5> pal { tone(palette::C64T62, h), tone(palette::C56T66, h + 120), tone(palette::C44T82, h + 240), tone(palette::C12T95, h), tone(palette::C40T28, h) };
      fill(c.target, rect_of(0, 0, c.width, c.height), pal[4]);
      const float scale = c.landscape ? 1.6f : 1.0f;
      const float cell = 300 * scale;
      for (int index = 0; index < 4; ++index) {
        const std::uint32_t bits = (c.hash >> (index * 7)) ^ (c.hash >> (index * 3 + 5));
        const auto &background = pal[bits % 4];
        const auto &foreground = pal[(bits % 4 + 1 + ((bits >> 3) % 3)) % 5];
        const int kind = static_cast<int>((bits >> 5) % 6);
        const float rotation = static_cast<float>(((bits >> 8) % 4) * 90);
        const float x = c.landscape ? index * cell : (index % 2) * cell, y = c.landscape ? 0 : (index / 2) * cell;
        const auto area = rect_of(x, y, x + cell, y + cell);
        c.target->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fill(c.target, area, background);
        c.target->SetTransform(D2D1::Matrix3x2F::Rotation(rotation, D2D1::Point2F(150, 150)) * D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y));
        const auto brush = solid(c.target, foreground);
        const auto path = [&](auto &&build) {
          ComPtr<ID2D1PathGeometry> geometry;
          ComPtr<ID2D1GeometrySink> sink;
          if (SUCCEEDED(c.d2d->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
            build(sink.Get());
            sink->Close();
            c.target->FillGeometry(geometry.Get(), brush.Get());
          }
        };
        if (brush) {
          switch (kind) {
            case 0:
              c.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(150, 150), 108, 108), brush.Get());
              break;
            case 1:  // a quarter of a disc from the corner
              path([](ID2D1GeometrySink *sink) {
                sink->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(300, 0));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0, 300), D2D1::SizeF(300, 300), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
              });
              break;
            case 2:  // the upper half of a disc
              path([](ID2D1GeometrySink *sink) {
                sink->BeginFigure(D2D1::Point2F(30, 150), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(270, 150), D2D1::SizeF(120, 120), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
              });
              break;
            case 3:  // a right triangle on the lower left
              path([](ID2D1GeometrySink *sink) {
                sink->BeginFigure(D2D1::Point2F(0, 300), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(300, 300));
                sink->AddLine(D2D1::Point2F(0, 0));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
              });
              break;
            case 4:  // a ring
              c.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(150, 150), 92, 92), brush.Get(), 44.0f);
              break;
            default:  // a diamond
              c.target->SetTransform(D2D1::Matrix3x2F::Rotation(45.0f, D2D1::Point2F(150, 150)) * D2D1::Matrix3x2F::Rotation(rotation, D2D1::Point2F(150, 150)) * D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(x, y));
              c.target->FillRectangle(rect_of(70, 70, 230, 230), brush.Get());
              break;
          }
        }
        c.target->SetTransform(D2D1::Matrix3x2F::Identity());
        c.target->PopAxisAlignedClip();
      }
      const float band_top = c.landscape ? cell : 600.0f;
      const float left = c.landscape ? 96 : 46, width = c.width - left * 2;
      const float box_top = c.landscape ? band_top : 632.0f, box_height = c.landscape ? c.height - band_top : 190.0f;
      const auto title = fit_text(*c.text, c.name, kSansHeavy, width, box_height - (c.landscape ? 80 : 0), c.landscape ? 170 : 62, c.landscape ? 56 : 38, 1.1f, 0.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
      draw_layout(c.target, title.layout.Get(), left, box_top + (box_height - title.height) / 2, D2D1::ColorF(1, 1, 1, 1));
    }

    bool
    render_style(ID2D1RenderTarget *target, IDWriteFactory *write, IWICImagingFactory *imaging, ID2D1Factory *d2d, IWICBitmap *surface,
      const app_t &app, style_e style, orientation_e orientation, const std::optional<ComPtr<IWICBitmap>> &icon) {
      (void) imaging;
      text_env_t text;
      text.write = write;
      text.serif_fallback = make_serif_fallback(write);
      canvas_t canvas;
      canvas.target = target;
      canvas.d2d = d2d;
      canvas.surface = surface;
      canvas.text = &text;
      canvas.landscape = orientation == orientation_e::landscape;
      canvas.width = canvas.landscape ? 1920.0f : 600.0f;
      canvas.height = canvas.landscape ? 1080.0f : 900.0f;
      canvas.name = wide_from_utf8(app.name);
      canvas.monogram = wide_from_utf8(monogram(app.name));
      canvas.hash = name_hash(app.name);
      canvas.hue = static_cast<int>(canvas.hash % 360);
      canvas.icon = &icon;
      target->BeginDraw();
      target->Clear(D2D1::ColorF(0, 0, 0, 1));
      switch (style) {
        case style_e::monogram:
          draw_monogram(canvas);
          break;
        case style_e::icon:
          if (icon) draw_icon(canvas);
          else draw_tonal(canvas);
          break;
        case style_e::editorial:
          draw_editorial(canvas);
          break;
        case style_e::glass:
          draw_glass(canvas);
          break;
        case style_e::geometric:
          draw_geometric(canvas);
          break;
        default:
          draw_tonal(canvas);
          break;
      }
      return SUCCEEDED(target->EndDraw()) && canvas.drawn;
    }

    std::optional<std::vector<unsigned char>>
    encode(IWICImagingFactory *imaging, IWICBitmapSource *bitmap, REFGUID container, float quality) {
      ComPtr<IStream> stream;
      if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return std::nullopt;
      ComPtr<IWICBitmapEncoder> encoder;
      if (FAILED(imaging->CreateEncoder(container, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return std::nullopt;
      ComPtr<IWICBitmapFrameEncode> frame;
      ComPtr<IPropertyBag2> properties;
      if (FAILED(encoder->CreateNewFrame(&frame, &properties))) return std::nullopt;
      if (container == GUID_ContainerFormatJpeg) {
        PROPBAG2 option {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = quality;
        properties->Write(1, &option, &value);
        VariantClear(&value);
      }
      if (FAILED(frame->Initialize(properties.Get()))) return std::nullopt;
      UINT width = 0, height = 0;
      bitmap->GetSize(&width, &height);
      if (FAILED(frame->SetSize(width, height))) return std::nullopt;
      const WICPixelFormatGUID desired = container == GUID_ContainerFormatJpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
      WICPixelFormatGUID format = desired;
      if (FAILED(frame->SetPixelFormat(&format)) || format != desired) return std::nullopt;
      ComPtr<IWICFormatConverter> converter;
      IWICBitmapSource *source = bitmap;
      if (container == GUID_ContainerFormatJpeg) {
        if (FAILED(imaging->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(bitmap, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return std::nullopt;
        source = converter.Get();
      }
      if (FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return std::nullopt;
      STATSTG stats {};
      if (FAILED(stream->Stat(&stats, STATFLAG_NONAME)) || stats.cbSize.QuadPart <= 0 || stats.cbSize.QuadPart > std::numeric_limits<std::size_t>::max()) return std::nullopt;
      LARGE_INTEGER origin {};
      stream->Seek(origin, STREAM_SEEK_SET, nullptr);
      std::vector<unsigned char> bytes(static_cast<std::size_t>(stats.cbSize.QuadPart));
      ULONG read = 0;
      if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size()) return std::nullopt;
      return bytes;
    }

    bool
    write_atomically(const std::filesystem::path &path, const std::vector<unsigned char> &bytes) {
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      // two requests that draw the same cover at the same time must not share a temporary file
      static std::atomic<std::uint64_t> sequence { 0 };
      const auto temporary = path.string() + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence.fetch_add(1));
      bool written = false;
      {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();  // a disk that cannot finish the file says so when it is closed
        written = !output.fail();
      }
      if (!written) {
        std::filesystem::remove(temporary, error);
        return false;
      }
      std::filesystem::rename(temporary, path, error);
      if (!error) return true;
      std::filesystem::remove(temporary, error);
      // another request published the same cover first (the name says what is in it)
      return regular_file(path);
    }

    // The name of a file this generator wrote: gen_ + the SHA-256 of its key (64 hex digits) + .jpg or .png. The folder is also the one the pictures of the person are read from.
    bool
    is_generated_cache_name(const std::string &name) {
      if (name.size() != 4 + 64 + 4 || name.rfind("gen_", 0) != 0) return false;
      const auto extension = lower(name.substr(name.size() - 4));
      if (extension != ".jpg" && extension != ".png") return false;
      return std::all_of(name.begin() + 4, name.begin() + 4 + 64, [](unsigned char character) { return std::isdigit(character) || (character >= 'a' && character <= 'f'); });
    }

    void
    prune_old(const std::filesystem::path &directory) {
      const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30);
      std::error_code listing_error;
      std::filesystem::directory_iterator entries(directory, listing_error);
      for (const std::filesystem::directory_iterator end; !listing_error && entries != end; entries.increment(listing_error)) {
        std::error_code error;  // of this file only
        const auto &entry = *entries;
        if (!entry.is_regular_file(error) || error) continue;
        if (!is_generated_cache_name(entry.path().filename().string())) continue;
        const auto written = entry.last_write_time(error);
        if (!error && written < cutoff) std::filesystem::remove(entry.path(), error);
      }
    }

    std::optional<asset_t>
    render(const app_t &app, style_e requested_style, orientation_e orientation, const std::filesystem::path &appdata) {
      com_scope_t com;
      if (!com) return std::nullopt;
      ComPtr<IWICImagingFactory> imaging;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)))) return std::nullopt;
      ComPtr<ID2D1Factory> d2d;
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return std::nullopt;
      ComPtr<IDWriteFactory> write;
      if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(write.GetAddressOf())))) return std::nullopt;

      // only the icon style looks at the program (its icon): the other styles do not stat anything
      const auto executable = requested_style == style_e::icon ? resolve_executable(app) : std::nullopt;
      const auto key_of = [&](style_e style, const std::optional<std::filesystem::path> &program) {
        return cache_hash(style_name(style), orientation, app.name, program ? program->string() : std::string {}, program ? file_mtime(*program) : 0);
      };
      const auto directory = appdata / "covers";
      const auto cached = [&](const std::string &name) -> std::optional<asset_t> {
        for (const auto extension : { ".jpg", ".png" }) {
          const auto candidate = directory / ("gen_" + name + extension);
          if (regular_file(candidate)) return asset_t { candidate, {}, extension == std::string_view { ".jpg" } ? "image/jpeg" : "image/png", name };
        }
        return std::nullopt;
      };
      auto hash = key_of(requested_style, executable);
      if (const auto hit = cached(hash)) return hit;

      const UINT width = orientation == orientation_e::portrait ? 600 : 1920;
      const UINT height = orientation == orientation_e::portrait ? 900 : 1080;
      ComPtr<IWICBitmap> bitmap;
      if (FAILED(imaging->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return std::nullopt;
      D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
      ComPtr<ID2D1RenderTarget> target;
      if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), &properties, &target))) return std::nullopt;
      std::optional<ComPtr<IWICBitmap>> icon;
      if (requested_style == style_e::icon && executable) icon = extract_icon(imaging.Get(), *executable);
      const auto actual_style = requested_style == style_e::icon && !icon ? style_e::tonal : requested_style;
      if (actual_style != requested_style && executable) {
        // the program is there but its icon could not be read now: the picture drawn instead is a tonal cover, kept under the key of the tonal cover, so the next request tries the icon again
        hash = key_of(style_e::tonal, std::nullopt);
        if (const auto hit = cached(hash)) return hit;
      }
      if (!render_style(target.Get(), write.Get(), imaging.Get(), d2d.Get(), bitmap.Get(), app, actual_style, orientation, icon)) return std::nullopt;
      const auto jpeg = encode(imaging.Get(), bitmap.Get(), GUID_ContainerFormatJpeg, 0.85f);
      const auto png = encode(imaging.Get(), bitmap.Get(), GUID_ContainerFormatPng, 0.0f);
      if (!jpeg && !png) return std::nullopt;
      const bool use_png = png && (!jpeg || png->size() < jpeg->size());
      const auto extension = use_png ? ".png" : ".jpg";
      const auto &bytes = use_png ? *png : *jpeg;
      const auto path = directory / ("gen_" + hash + extension);
      if (!write_atomically(path, bytes)) return std::nullopt;
      prune_old(directory);
      return asset_t { path, {}, use_png ? "image/png" : "image/jpeg", hash };
    }

#endif

  }  // namespace

  bool
  is_style_name(std::string_view value) noexcept {
    return std::find(kStyles.begin(), kStyles.end(), value) != kStyles.end();
  }

  style_e
  style_from_name(std::string_view value) noexcept {
    for (std::size_t index = 0; index < kStyles.size(); ++index)
      if (kStyles[index] == value) return static_cast<style_e>(index);
    return style_e::tonal;
  }

  std::string_view
  style_name(style_e style) noexcept {
    const auto index = static_cast<std::size_t>(style);
    return index < kStyles.size() ? kStyles[index] : kStyles[0];
  }

  std::string_view
  source_name(source_e source) noexcept {
    switch (source) {
      case source_e::poster:
        return "poster";
      case source_e::xbox:
        return "xbox";
      case source_e::generated:
        return "generated";
    }
    return "generated";
  }

  style_e
  effective_style(const app_t &app, std::string_view global_style) noexcept {
    if (is_style_name(app.cover_style) && app.cover_style != "auto") return style_from_name(app.cover_style);
    return is_style_name(global_style) ? style_from_name(global_style) : style_e::tonal;
  }

  std::uint32_t
  name_hash(std::string_view name) noexcept {
    // FNV-1a over the UTF-8 bytes: stable across machines and builds, as the cover design needs
    std::uint32_t hash = 0x811c9dc5u;
    for (const unsigned char byte : name) {
      hash ^= byte;
      hash *= 0x01000193u;
    }
    return hash;
  }

  namespace {

    // The monogram works on Unicode scalar values: the text is decoded once, grouped into graphemes (a scalar with the marks, joiners and
    // modifiers that follow it), and the groups that are letters or digits (not pictographs) are the ones that count.
    std::u32string
    decode_utf8(std::string_view text) {
      std::u32string result;
      for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 1;
        if (index + length > text.size()) length = 1;
        char32_t value = length == 1 ? lead : lead & (0xff >> (length + 1));
        for (std::size_t offset = 1; offset < length; ++offset) value = (value << 6) | (static_cast<unsigned char>(text[index + offset]) & 0x3f);
        result.push_back(value);
        index += length;
      }
      return result;
    }

    void
    append_utf8(std::string &out, char32_t value) {
      if (value < 0x80) out.push_back(static_cast<char>(value));
      else if (value < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
      }
      else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
      }
      else {
        out.push_back(static_cast<char>(0xf0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3f)));
      }
    }

    bool
    is_pictograph(char32_t c) {
      return (c >= 0x1f000 && c <= 0x1faff) || (c >= 0x2600 && c <= 0x27bf) || (c >= 0x2300 && c <= 0x23ff) || (c >= 0x2b00 && c <= 0x2bff) ||
             c == 0xa9 || c == 0xae || c == 0x203c || c == 0x2049 || c == 0x2122 || c == 0x2139 || c == 0x3030 || c == 0x303d || c == 0x3297 || c == 0x3299;
    }

    bool
    is_cjk_letter(char32_t c) {
      return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x2fa1f) ||  // Han
             (c >= 0x3040 && c <= 0x30ff && c != 0x30a0 && c != 0x30fb) || (c >= 0x31f0 && c <= 0x31ff) || (c >= 0xff66 && c <= 0xff9f) ||  // Hiragana, Katakana
             (c >= 0xac00 && c <= 0xd7af) || (c >= 0x1100 && c <= 0x11ff) || (c >= 0x3130 && c <= 0x318f);  // Hangul
    }

    bool
    is_letter_or_digit(char32_t c) {
      if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (is_cjk_letter(c)) return true;
      return (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) || (c >= 0x370 && c <= 0x52f) || (c >= 0x5d0 && c <= 0x5ea) || (c >= 0x620 && c <= 0x64a) ||
             (c >= 0x900 && c <= 0xdff) || (c >= 0xe00 && c <= 0xe7f) || (c >= 0xff10 && c <= 0xff19) || (c >= 0xff21 && c <= 0xff3a) || (c >= 0xff41 && c <= 0xff5a);
    }

    bool
    extends_grapheme(char32_t c) {
      return c == 0x200d || c == 0xfe0f || c == 0xfe0e || (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0x300 && c <= 0x36f) || (c >= 0x20d0 && c <= 0x20ff) || c == 0x3099 || c == 0x309a;
    }

    std::vector<std::u32string>
    graphemes(const std::u32string &text) {
      std::vector<std::u32string> result;
      for (std::size_t index = 0; index < text.size();) {
        std::u32string cluster(1, text[index++]);
        const bool flag = cluster[0] >= 0x1f1e6 && cluster[0] <= 0x1f1ff;
        if (flag && index < text.size() && text[index] >= 0x1f1e6 && text[index] <= 0x1f1ff) cluster.push_back(text[index++]);
        while (index < text.size() && extends_grapheme(text[index])) {
          const bool joiner = text[index] == 0x200d;
          cluster.push_back(text[index++]);
          if (joiner && index < text.size()) cluster.push_back(text[index++]);
        }
        result.push_back(std::move(cluster));
      }
      return result;
    }

    bool
    is_word_grapheme(const std::u32string &cluster) {
      return !is_pictograph(cluster[0]) && is_letter_or_digit(cluster[0]);
    }

    bool
    is_word_separator(char32_t c) {
      return c == ' ' || c == '\t' || c == '\n' || c == 0xa0 || c == 0x3000 || c == ':' || c == 0xff1a || c == '-' || c == 0x2013 || c == 0x2014 || c == '_' || c == '.';
    }

    char32_t
    upper(char32_t c) {
      if (c >= 'a' && c <= 'z') return c - 32;
      if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
      if (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) return c - 32;
      if (c >= 0x430 && c <= 0x44f) return c - 32;
      if (c == 0x451) return 0x401;  // ё -> Ё: outside the а-я run
      return c;
    }

  }  // namespace

  std::string
  monogram(std::string_view name) {
    const auto text = decode_utf8(name);
    const auto clusters = graphemes(text);
    std::vector<const std::u32string *> words;
    for (const auto &cluster : clusters) {
      if (is_word_grapheme(cluster)) words.push_back(&cluster);
    }
    std::string result;
    if (words.empty()) {
      // nothing but pictographs and punctuation: the first thing that is there
      if (clusters.empty()) return "?";
      for (const auto value : clusters.front()) append_utf8(result, value);
      return result;
    }
    // Han, kana and hangul: the first character stands for the whole name
    if (is_cjk_letter((*words.front())[0])) {
      for (const auto value : *words.front()) append_utf8(result, value);
      return result;
    }
    // other scripts: the first letters of the first two words
    std::vector<std::u32string> initials;
    std::size_t index = 0;
    while (index <= text.size()) {
      std::size_t end = index;
      while (end < text.size() && !is_word_separator(text[end])) ++end;
      if (end > index) {
        for (const auto &cluster : graphemes(text.substr(index, end - index))) {
          if (is_word_grapheme(cluster)) {
            initials.push_back(cluster);
            break;
          }
        }
      }
      index = end + 1;
    }
    if (initials.empty()) initials.push_back(*words.front());
    for (std::size_t item = 0; item < std::min<std::size_t>(initials.size(), 2); ++item) {
      for (const auto value : initials[item]) append_utf8(result, upper(value));
    }
    return result;
  }

  std::string
  cache_hash(std::string_view style, orientation_e orientation, std::string_view app_name,
    std::string_view icon_path, std::int64_t icon_mtime) {
    std::string key;
    key.reserve(generator_version.size() + style.size() + app_name.size() + icon_path.size() + 64);
    key.append(generator_version);
    key.push_back('|');
    key.append(style);
    key.push_back('|');
    key.append(orientation == orientation_e::portrait ? "portrait" : "landscape");
    key.push_back('|');
    key.append(app_name);
    key.push_back('|');
    key.append(icon_path);
    key.push_back('|');
    key.append(std::to_string(icon_mtime));
    return sha256(key);
  }

  std::string
  orientation_name(orientation_e orientation) noexcept {
    return orientation == orientation_e::portrait ? "portrait" : "landscape";
  }

  bool
  shows_placeholder(const app_t &app) {
    // no picture of the person's that the host can use, and not the Xbox entry
    return !has_real_poster(app);
  }

#ifdef _WIN32
  namespace {
    // The key that names the cached file of a cover (what render() looks at), worked out without drawing: only the icon style looks at the program.
    std::string
    cover_key(const app_t &app, style_e style, orientation_e orientation, const std::optional<std::filesystem::path> &program) {
      return cache_hash(style_name(style), orientation, app.name, program ? program->string() : std::string {}, program ? file_mtime(*program) : 0);
    }
  }  // namespace
#endif

  std::string
  art_fact(const app_t &app, std::string_view global_style) {
#ifdef _WIN32
    if (has_real_poster(app)) return {};
    const auto style = effective_style(app, global_style);
    const auto program = style == style_e::icon ? resolve_executable(app) : std::nullopt;
    // a custom landscape picture is served as it is: only the portrait cover is the generated one then
    const bool own_hero = regular_file(from_utf8(app.hero_path));
    return "cover:" + cover_key(app, style, orientation_e::portrait, program) + ':' + (own_hero ? std::string {} : cover_key(app, style, orientation_e::landscape, program)) + ';';
#else
    // the renderer is the Windows host's: elsewhere no cover is generated, and the art is what it was
    (void) app;
    (void) global_style;
    return {};
#endif
  }

  cover_t
  describe(const app_t &app, std::string_view global_style, const std::filesystem::path &appdata, bool draw) {
    cover_t result;
    result.style = effective_style(app, global_style);
    result.style_name = std::string(style_name(result.style));
    if (has_real_poster(app)) {
      result.source = app.xbox_app && is_box_placeholder(app) ? source_e::xbox : source_e::poster;
      result.source_name = std::string(source_name(result.source));
      result.portrait = asset_t { from_utf8(app.poster_path), url_for(app, orientation_e::portrait), "", {} };
      const auto hero = from_utf8(app.hero_path);
      result.landscape = asset_t { regular_file(hero) ? hero : from_utf8(app.poster_path), url_for(app, orientation_e::landscape), "", {} };
      return result;
    }
    result.source = source_e::generated;
    result.source_name = std::string(source_name(result.source));
    // a landscape picture of the person's stays the landscape cover, as before: only the portrait one replaces the placeholder
    const bool own_hero = regular_file(from_utf8(app.hero_path));
    if (own_hero) result.landscape = asset_t { from_utf8(app.hero_path), url_for(app, orientation_e::landscape), "", {} };
    // the address of the covers is known without drawing them: a caller that must not wait (the list) asks with draw = false and the picture is drawn when it is asked for
    result.portrait.url = url_for(app, orientation_e::portrait);
    if (!own_hero) result.landscape.url = url_for(app, orientation_e::landscape);
#ifdef _WIN32
    if (!draw) {
      return result;
    }
    if (const auto portrait = render(app, result.style, orientation_e::portrait, appdata)) {
      auto value = *portrait;
      value.url = url_for(app, orientation_e::portrait);
      result.portrait = std::move(value);
    }
    if (own_hero) {
      // nothing to draw
    }
    else if (const auto landscape = render(app, result.style, orientation_e::landscape, appdata)) {
      auto value = *landscape;
      value.url = url_for(app, orientation_e::landscape);
      result.landscape = std::move(value);
    }
#else
    (void) appdata;
    (void) draw;
#endif
    return result;
  }

  asset_t
  preview(const app_t &app, style_e style, orientation_e orientation, const std::filesystem::path &appdata) {
#ifdef _WIN32
    if (const auto result = render(app, style, orientation, appdata)) {
      auto value = *result;
      value.url = url_for(app, orientation);
      return value;
    }
#else
    (void) app;
    (void) style;
    (void) orientation;
    (void) appdata;
#endif
    return {};
  }

}  // namespace generated_cover
