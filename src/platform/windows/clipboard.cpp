/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Windows clipboard backend for Sunshine clipboard sync.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "clipboard.h"

#include "src/platform/common.h"
#include "src/stb_image.h"
#include "src/stb_image_write.h"

namespace platf::clipboard {
  namespace {
    constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    constexpr auto clipboard_retry_delay = std::chrono::milliseconds(8);
    constexpr int clipboard_retry_count = 8;
    constexpr std::size_t max_decoded_image_bytes = 64U * 1024U * 1024U;
    constexpr std::size_t max_text_clipboard_bytes = 1U * 1024U * 1024U;

    constexpr wchar_t clipboard_owner_window_class[] = L"SunshineClipboardOwnerWindow";
    std::mutex clipboard_owner_window_mutex;
    HWND clipboard_owner_window = nullptr;

    bool
    ensure_clipboard_owner_window() {
      std::lock_guard<std::mutex> lock(clipboard_owner_window_mutex);
      if (clipboard_owner_window != nullptr && IsWindow(clipboard_owner_window)) {
        return true;
      }

      const HINSTANCE instance = GetModuleHandleW(nullptr);
      WNDCLASSEXW wnd_class {};
      wnd_class.cbSize = sizeof(wnd_class);
      wnd_class.lpfnWndProc = DefWindowProcW;
      wnd_class.hInstance = instance;
      wnd_class.lpszClassName = clipboard_owner_window_class;

      if (RegisterClassExW(&wnd_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        clipboard_owner_window = nullptr;
        return false;
      }

      clipboard_owner_window = CreateWindowExW(0,
                                               clipboard_owner_window_class,
                                               L"Sunshine Clipboard Owner Window",
                                               0,
                                               0,
                                               0,
                                               0,
                                               0,
                                               HWND_MESSAGE,
                                               nullptr,
                                               instance,
                                               nullptr);
      return clipboard_owner_window != nullptr;
    }

    enum class clipboard_open_mode {
      read,
      write,
    };

    struct clipboard_guard_t {
      bool open = false;

      explicit clipboard_guard_t(clipboard_open_mode mode = clipboard_open_mode::read) {
        HWND owner = nullptr;
        if (mode == clipboard_open_mode::write) {
          if (!ensure_clipboard_owner_window()) {
            return;
          }

          owner = clipboard_owner_window;
        }

        for (int attempt = 0; attempt < clipboard_retry_count; ++attempt) {
          if (OpenClipboard(owner)) {
            open = true;
            break;
          }

          Sleep(static_cast<DWORD>(clipboard_retry_delay.count()));
        }
      }

      ~clipboard_guard_t() {
        if (open) {
          CloseClipboard();
        }
      }

      explicit operator bool() const {
        return open;
      }
    };

    std::wstring
    utf8_to_wide(const std::string_view value);

    std::string
    normalize_newlines_to_lf(const std::string_view value);

    std::string
    wide_to_utf8(const std::wstring &value) {
      if (value.empty()) {
        return {};
      }

      const int required =
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (required <= 0) {
        return {};
      }

      std::string result(static_cast<std::size_t>(required), '\0');
      WideCharToMultiByte(CP_UTF8,
                          0,
                          value.c_str(),
                          static_cast<int>(value.size()),
                          result.data(),
                          required,
                          nullptr,
                          nullptr);
      return result;
    }

    std::wstring
    utf8_to_wide(const std::string_view value) {
      if (value.empty()) {
        return {};
      }

      const int required =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (required <= 0) {
        return {};
      }

      std::wstring result(static_cast<std::size_t>(required), L'\0');
      MultiByteToWideChar(CP_UTF8,
                          0,
                          value.data(),
                          static_cast<int>(value.size()),
                          result.data(),
                          required);
      return result;
    }

    std::string
    normalize_newlines_to_lf(const std::string_view value) {
      std::string normalized;
      normalized.reserve(value.size());

      for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '\r') {
          if (i + 1 < value.size() && value[i + 1] == '\n') {
            ++i;
          }
          normalized.push_back('\n');
        }
        else {
          normalized.push_back(ch);
        }
      }

      return normalized;
    }

    std::wstring
    normalize_newlines_to_crlf(const std::string_view value) {
      const std::wstring wide = utf8_to_wide(normalize_newlines_to_lf(value));
      std::wstring normalized;
      normalized.reserve(wide.size() * 2);

      for (wchar_t ch: wide) {
        if (ch == L'\n') {
          normalized.push_back(L'\r');
          normalized.push_back(L'\n');
        }
        else {
          normalized.push_back(ch);
        }
      }

      return normalized;
    }

    std::uint64_t
    fnv1a_append(std::uint64_t hash, const void *data, std::size_t length) {
      const auto *bytes = static_cast<const std::uint8_t *>(data);
      for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= fnv_prime;
      }
      return hash;
    }

    std::uint64_t
    compute_item_hash(std::uint8_t type,
                      const std::vector<std::uint8_t> &data,
                      const std::string_view &name = {}) {
      std::uint64_t hash = fnv_offset_basis;
      hash = fnv1a_append(hash, &type, sizeof(type));
      if (!data.empty()) {
        hash = fnv1a_append(hash, data.data(), data.size());
      }
      if (!name.empty()) {
        hash = fnv1a_append(hash, name.data(), name.size());
      }
      return hash;
    }

    bool
    read_hglobal_bytes(HANDLE handle, std::vector<std::uint8_t> &bytes) {
      if (handle == nullptr) {
        return false;
      }

      const auto size = GlobalSize(handle);
      if (size == 0) {
        bytes.clear();
        return true;
      }

      const void *locked = GlobalLock(handle);
      if (locked == nullptr) {
        return false;
      }

      bytes.assign(static_cast<const std::uint8_t *>(locked),
                   static_cast<const std::uint8_t *>(locked) + size);
      GlobalUnlock(handle);
      return true;
    }

    bool
    write_hglobal_bytes(UINT format, const std::vector<std::uint8_t> &bytes) {
      HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
      if (mem == nullptr) {
        return false;
      }

      void *locked = GlobalLock(mem);
      if (locked == nullptr) {
        GlobalFree(mem);
        return false;
      }

      if (!bytes.empty()) {
        std::memcpy(locked, bytes.data(), bytes.size());
      }
      GlobalUnlock(mem);

      if (SetClipboardData(format, mem) == nullptr) {
        GlobalFree(mem);
        return false;
      }

      return true;
    }

    bool
    checked_mul(std::size_t lhs, std::size_t rhs, std::size_t &result) {
      if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
      }

      result = lhs * rhs;
      return true;
    }

    bool
    checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) {
      if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
      }

      result = lhs + rhs;
      return true;
    }

    bool
    decoded_image_size_valid(int width,
                             int height,
                             std::size_t bytes_per_pixel,
                             std::size_t &pixel_bytes) {
      if (width <= 0 || height <= 0 || bytes_per_pixel == 0) {
        return false;
      }

      std::size_t area = 0;
      if (!checked_mul(static_cast<std::size_t>(width),
                       static_cast<std::size_t>(height),
                       area) ||
          !checked_mul(area, bytes_per_pixel, pixel_bytes) ||
          pixel_bytes > max_decoded_image_bytes) {
        return false;
      }

      return true;
    }

    bool
    png_decoded_size_valid(const std::vector<std::uint8_t> &png_bytes,
                           std::size_t &pixel_bytes) {
      if (png_bytes.empty() ||
          png_bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
      }

      int width = 0;
      int height = 0;
      int components = 0;
      if (stbi_info_from_memory(png_bytes.data(),
                                static_cast<int>(png_bytes.size()),
                                &width,
                                &height,
                                &components) == 0) {
        return false;
      }

      return decoded_image_size_valid(width, height, 4, pixel_bytes);
    }

    bool
    dib_stride_valid(int width, int bit_count, std::size_t &stride) {
      if (width <= 0 || bit_count <= 0) {
        return false;
      }

      std::size_t bits_per_row = 0;
      if (!checked_mul(static_cast<std::size_t>(width),
                       static_cast<std::size_t>(bit_count),
                       bits_per_row)) {
        return false;
      }

      if (!checked_add(bits_per_row, 31, bits_per_row)) {
        return false;
      }
      bits_per_row /= 32;
      return checked_mul(bits_per_row, 4, stride);
    }


    bool
    encode_png_from_rgba(int width,
                         int height,
                         const std::vector<std::uint8_t> &rgba,
                         std::vector<std::uint8_t> &png_bytes) {
      if (width <= 0 || height <= 0) {
        return false;
      }

      png_bytes.clear();
      auto writer = [](void *context, void *data, int size) {
        auto *buffer = static_cast<std::vector<std::uint8_t> *>(context);
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        buffer->insert(buffer->end(), bytes, bytes + size);
      };

      return stbi_write_png_to_func(writer,
                                    &png_bytes,
                                    width,
                                    height,
                                    4,
                                    rgba.data(),
                                    width * 4) != 0;
    }

    bool
    decode_dib_to_png(HANDLE dib_handle, std::vector<std::uint8_t> &png_bytes) {
      if (dib_handle == nullptr) {
        return false;
      }

      const auto total_size = GlobalSize(dib_handle);
      if (total_size < sizeof(BITMAPINFOHEADER)) {
        return false;
      }

      const auto *header =
        static_cast<const BITMAPINFOHEADER *>(GlobalLock(dib_handle));
      if (header == nullptr) {
        return false;
      }

      const BITMAPINFOHEADER info = *header;
      if (info.biWidth <= 0 || info.biHeight == 0) {
        GlobalUnlock(dib_handle);
        return false;
      }
      if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biSize > total_size) {
        GlobalUnlock(dib_handle);
        return false;
      }

      const int width = info.biWidth;
      const auto raw_height = static_cast<std::int64_t>(info.biHeight);
      const auto absolute_height = raw_height > 0 ? raw_height : -raw_height;
      if (absolute_height > std::numeric_limits<int>::max()) {
        GlobalUnlock(dib_handle);
        return false;
      }
      const int height = static_cast<int>(absolute_height);
      const bool top_down = info.biHeight < 0;
      const int bit_count = info.biBitCount;
      const DWORD compression = info.biCompression;

      if (!((bit_count == 32 && (compression == BI_RGB || compression == BI_BITFIELDS)) ||
            (bit_count == 24 && compression == BI_RGB))) {
        GlobalUnlock(dib_handle);
        return false;
      }

      std::size_t pixel_offset = info.biSize;
      if (compression == BI_BITFIELDS) {
        if (!checked_add(pixel_offset, 3 * sizeof(DWORD), pixel_offset)) {
          GlobalUnlock(dib_handle);
          return false;
        }
      }

      std::size_t stride = 0;
      std::size_t pixel_data_bytes = 0;
      std::size_t required_size = 0;
      std::size_t rgba_bytes = 0;
      if (!dib_stride_valid(width, bit_count, stride) ||
          !checked_mul(stride, static_cast<std::size_t>(height), pixel_data_bytes) ||
          !checked_add(pixel_offset, pixel_data_bytes, required_size) ||
          required_size > total_size ||
          !decoded_image_size_valid(width, height, 4, rgba_bytes)) {
        GlobalUnlock(dib_handle);
        return false;
      }

      const auto *pixels = reinterpret_cast<const std::uint8_t *>(header) + pixel_offset;
      std::vector<std::uint8_t> rgba(rgba_bytes);
      bool has_non_zero_alpha = false;

      for (int y = 0; y < height; ++y) {
        const int src_y = top_down ? y : (height - 1 - y);
        const auto *src = pixels + stride * static_cast<std::size_t>(src_y);
        auto *dst = rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;

        for (int x = 0; x < width; ++x) {
          if (bit_count == 32) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
            has_non_zero_alpha = has_non_zero_alpha || src[x * 4 + 3] != 0;
          }
          else {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 0xFF;
          }
        }
      }

      if (bit_count == 32 && !has_non_zero_alpha) {
        for (std::size_t i = 3; i < rgba.size(); i += 4) {
          rgba[i] = 0xFF;
        }
      }

      GlobalUnlock(dib_handle);
      return encode_png_from_rgba(width, height, rgba, png_bytes);
    }

    bool
    read_text_item(item_t &item, std::string *reason) {
      HANDLE handle = GetClipboardData(CF_UNICODETEXT);
      if (handle == nullptr) {
        if (reason) {
          *reason = "CF_UNICODETEXT unavailable";
        }
        return false;
      }

      const auto *wide = static_cast<const wchar_t *>(GlobalLock(handle));
      if (wide == nullptr) {
        if (reason) {
          *reason = "CF_UNICODETEXT lock failed";
        }
        return false;
      }

      const auto size_bytes = GlobalSize(handle);
      const auto max_chars = size_bytes / sizeof(wchar_t);
      std::size_t length = 0;
      while (length < max_chars && wide[length] != L'\0') {
        ++length;
      }
      if (length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        GlobalUnlock(handle);
        if (reason) {
          *reason = "CF_UNICODETEXT exceeded supported length";
        }
        item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
        return true;
      }
      const int utf8_length = WideCharToMultiByte(CP_UTF8,
                                                  0,
                                                  wide,
                                                  static_cast<int>(length),
                                                  nullptr,
                                                  0,
                                                  nullptr,
                                                  nullptr);
      if ((length != 0 && utf8_length <= 0) ||
          static_cast<std::size_t>(utf8_length) > max_text_clipboard_bytes) {
        GlobalUnlock(handle);
        if (reason) {
          *reason = "CF_UNICODETEXT exceeded clipboard text size limit";
        }
        item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
        return true;
      }

      std::wstring text(wide, length);
      GlobalUnlock(handle);

      const std::string utf8 = normalize_newlines_to_lf(wide_to_utf8(text));
      item.type = LI_CLIPBOARD_ITEM_TYPE_TEXT;
      item.mime_type = "text/plain;charset=utf-8";
      item.name.clear();
      item.data.assign(utf8.begin(), utf8.end());
      item.content_hash = compute_item_hash(item.type, item.data);
      return true;
    }

    bool
    read_image_item(item_t &item, std::string *reason) {
      const UINT png_format = RegisterClipboardFormatW(L"PNG");
      HANDLE handle = png_format != 0 ? GetClipboardData(png_format) : nullptr;
      std::vector<std::uint8_t> png_bytes;

      if (handle != nullptr && read_hglobal_bytes(handle, png_bytes) && !png_bytes.empty()) {
        if (png_bytes.size() > image_size_limit) {
          if (reason) {
            *reason = "PNG clipboard item exceeded image size limit";
          }
          item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
          return true;
        }
        std::size_t pixel_bytes = 0;
        if (!png_decoded_size_valid(png_bytes, pixel_bytes)) {
          if (reason) {
            *reason = "PNG clipboard item exceeded decoded image size limit";
          }
          item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
          return true;
        }

        item.type = LI_CLIPBOARD_ITEM_TYPE_IMAGE;
        item.mime_type = "image/png";
        item.name.clear();
        item.data = std::move(png_bytes);
        item.content_hash = compute_item_hash(item.type, item.data);
        return true;
      }

      handle = GetClipboardData(CF_DIBV5);
      if (handle == nullptr) {
        handle = GetClipboardData(CF_DIB);
      }

      if (handle == nullptr) {
        if (reason) {
          *reason = "No PNG/DIB clipboard image available";
        }
        return false;
      }

      if (!decode_dib_to_png(handle, png_bytes)) {
        if (reason) {
          *reason = "Failed to convert DIB clipboard image to PNG";
        }
        return false;
      }

      if (png_bytes.size() > image_size_limit) {
        if (reason) {
          *reason = "DIB clipboard image exceeded image size limit after PNG normalization";
        }
        item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
        return true;
      }

      item.type = LI_CLIPBOARD_ITEM_TYPE_IMAGE;
      item.mime_type = "image/png";
      item.name.clear();
      item.data = std::move(png_bytes);
      item.content_hash = compute_item_hash(item.type, item.data);
      return true;
    }

    bool
    write_unicode_text(const item_t &item, std::string *reason) {
      const std::wstring text = normalize_newlines_to_crlf(std::string_view {
        reinterpret_cast<const char *>(item.data.data()),
        item.data.size(),
      });
      const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
      HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (mem == nullptr) {
        if (reason) {
          *reason = "GlobalAlloc failed for CF_UNICODETEXT";
        }
        return false;
      }

      void *locked = GlobalLock(mem);
      if (locked == nullptr) {
        GlobalFree(mem);
        if (reason) {
          *reason = "GlobalLock failed for CF_UNICODETEXT";
        }
        return false;
      }

      std::memcpy(locked, text.c_str(), bytes);
      GlobalUnlock(mem);

      if (!EmptyClipboard()) {
        GlobalFree(mem);
        if (reason) {
          *reason = "EmptyClipboard failed";
        }
        return false;
      }

      if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        GlobalFree(mem);
        if (reason) {
          *reason = "SetClipboardData(CF_UNICODETEXT) failed";
        }
        return false;
      }

      return true;
    }

    bool
    write_png_image(const item_t &item, std::string *reason) {
      if (item.data.size() > image_size_limit) {
        if (reason) {
          *reason = "PNG clipboard payload exceeded encoded image size limit";
        }
        return false;
      }
      std::size_t expected_pixel_bytes = 0;
      if (!png_decoded_size_valid(item.data, expected_pixel_bytes)) {
        if (reason) {
          *reason = "PNG clipboard payload exceeded decoded image size limit";
        }
        return false;
      }

      int width = 0;
      int height = 0;
      int components = 0;
      unsigned char *rgba =
        stbi_load_from_memory(item.data.data(),
                              static_cast<int>(item.data.size()),
                              &width,
                              &height,
                              &components,
                              4);
      if (rgba == nullptr || width <= 0 || height <= 0) {
        if (reason) {
          *reason = "Failed to decode PNG clipboard payload";
        }
        if (rgba != nullptr) {
          stbi_image_free(rgba);
        }
        return false;
      }

      std::size_t pixel_bytes = 0;
      std::size_t dib_size = 0;
      if (!decoded_image_size_valid(width, height, 4, pixel_bytes) ||
          !checked_add(sizeof(BITMAPV5HEADER), pixel_bytes, dib_size)) {
        stbi_image_free(rgba);
        if (reason) {
          *reason = "PNG clipboard payload exceeded decoded image size limit";
        }
        return false;
      }

      HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, dib_size);
      if (mem == nullptr) {
        stbi_image_free(rgba);
        if (reason) {
          *reason = "GlobalAlloc failed for CF_DIBV5";
        }
        return false;
      }

      auto *header = static_cast<BITMAPV5HEADER *>(GlobalLock(mem));
      if (header == nullptr) {
        GlobalFree(mem);
        stbi_image_free(rgba);
        if (reason) {
          *reason = "GlobalLock failed for CF_DIBV5";
        }
        return false;
      }

      std::memset(header, 0, sizeof(BITMAPV5HEADER));
      header->bV5Size = sizeof(BITMAPV5HEADER);
      header->bV5Width = width;
      header->bV5Height = -height;
      header->bV5Planes = 1;
      header->bV5BitCount = 32;
      header->bV5Compression = BI_BITFIELDS;
      header->bV5RedMask = 0x00FF0000;
      header->bV5GreenMask = 0x0000FF00;
      header->bV5BlueMask = 0x000000FF;
      header->bV5AlphaMask = 0xFF000000;
      header->bV5CSType = LCS_sRGB;
      header->bV5SizeImage = static_cast<DWORD>(pixel_bytes);

      auto *pixels = reinterpret_cast<std::uint8_t *>(header + 1);
      for (std::size_t i = 0; i < pixel_bytes; i += 4) {
        pixels[i + 0] = rgba[i + 2];
        pixels[i + 1] = rgba[i + 1];
        pixels[i + 2] = rgba[i + 0];
        pixels[i + 3] = rgba[i + 3];
      }

      GlobalUnlock(mem);
      stbi_image_free(rgba);

      if (!EmptyClipboard()) {
        GlobalFree(mem);
        if (reason) {
          *reason = "EmptyClipboard failed";
        }
        return false;
      }

      if (SetClipboardData(CF_DIBV5, mem) == nullptr) {
        GlobalFree(mem);
        if (reason) {
          *reason = "SetClipboardData(CF_DIBV5) failed";
        }
        return false;
      }

      const UINT png_format = RegisterClipboardFormatW(L"PNG");
      if (png_format != 0) {
        write_hglobal_bytes(png_format, item.data);
      }

      return true;
    }

  }  // namespace

  bool
  is_backend_available() {
    return true;
  }

  std::uint32_t
  supported_capabilities() {
    if (!is_backend_available() || !config::input.clipboard_sync) {
      return 0;
    }

    return platform_caps::clipboard_text |
           platform_caps::clipboard_image;
  }

  std::uint32_t
  current_sequence_number() {
    return GetClipboardSequenceNumber();
  }

  bool
  read_current_item(item_t &item, std::string *reason) {
    item = {};

    clipboard_guard_t clipboard;
    if (!clipboard) {
      if (reason) {
        *reason = "OpenClipboard failed";
      }
      return false;
    }

    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    if ((png_format != 0 && IsClipboardFormatAvailable(png_format)) ||
        IsClipboardFormatAvailable(CF_DIBV5) ||
        IsClipboardFormatAvailable(CF_DIB)) {
      return read_image_item(item, reason);
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      return read_text_item(item, reason);
    }

    if (reason) {
      *reason = "Clipboard did not contain a supported item";
    }
    item.type = LI_CLIPBOARD_ITEM_TYPE_NONE;
    return true;
  }

  bool
  write_item(const item_t &item, std::string *reason) {
    clipboard_guard_t clipboard { clipboard_open_mode::write };
    if (!clipboard) {
      if (reason) {
        *reason = "OpenClipboard failed";
      }
      return false;
    }

    switch (item.type) {
      case LI_CLIPBOARD_ITEM_TYPE_TEXT:
        return write_unicode_text(item, reason);
      case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
        return write_png_image(item, reason);
      case LI_CLIPBOARD_ITEM_TYPE_NONE:
        if (!EmptyClipboard()) {
          if (reason) {
            *reason = "EmptyClipboard failed";
          }
          return false;
        }
        return true;
      default:
        if (reason) {
          *reason = "Unsupported clipboard item type";
        }
        return false;
    }
  }
}  // namespace platf::clipboard
