/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Sunshine clipboard bridge wrapper over the AlkaidLab Win32 backend.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "clipboard.h"

#include <string>

#include "alkaidlab/clipboard_sync/win32_clipboard_backend.h"
#include "src/config.h"
#include "src/platform/common.h"

namespace platf::clipboard {
  namespace backend = alkaidlab::backend::clipboard_win32;

  namespace {
    constexpr std::uint32_t backend_cap_text_utf8 = 0x00000001u;
    constexpr std::uint32_t backend_cap_png_image = 0x00000002u;

    backend::item_t
    to_backend_item(const item_t &item) {
      backend::item_t backend_item;
      backend_item.type = item.type;
      backend_item.data = item.data;
      backend_item.mime_type = item.mime_type;
      backend_item.name = item.name;
      backend_item.content_hash = item.content_hash;
      return backend_item;
    }

    item_t
    from_backend_item(const backend::item_t &item) {
      item_t product_item;
      product_item.type = item.type;
      product_item.data = item.data;
      product_item.mime_type = item.mime_type;
      product_item.name = item.name;
      product_item.content_hash = item.content_hash;
      return product_item;
    }
  }  // namespace

  bool
  is_backend_available() {
    return backend::is_backend_available();
  }

  std::uint32_t
  supported_capabilities() {
    if (!is_backend_available() || !config::input.clipboard_sync) {
      return 0;
    }

    const auto backend_caps = backend::supported_capabilities();
    std::uint32_t caps = 0;
    if ((backend_caps & backend_cap_text_utf8) != 0) {
      caps |= platform_caps::clipboard_text;
    }
    if ((backend_caps & backend_cap_png_image) != 0) {
      caps |= platform_caps::clipboard_image;
    }
    return caps;
  }

  std::uint32_t
  current_sequence_number() {
    return backend::current_sequence_number();
  }

  bool
  read_current_item(item_t &item, std::string *reason) {
    backend::item_t backend_item;
    if (!backend::read_current_item(backend_item, reason)) {
      return false;
    }

    item = from_backend_item(backend_item);
    return true;
  }

  bool
  write_item(const item_t &item, std::string *reason) {
    backend::item_t backend_item = to_backend_item(item);
    return backend::write_item(backend_item, reason);
  }
}  // namespace platf::clipboard
