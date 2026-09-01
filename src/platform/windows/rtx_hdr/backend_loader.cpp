/**
 * @file src/platform/windows/rtx_hdr/backend_loader.cpp
 * @brief Safe loader for the optional TrueHDR backend DLL.
 */

#include "backend_loader.h"

#include <utility>

namespace platf::dxgi::rtx_hdr {
  backend_loader_t::backend_loader_t(backend_loader_t &&other) noexcept:
      module_ { std::exchange(other.module_, nullptr) },
      api_ { std::exchange(other.api_, nullptr) },
      error_ { std::move(other.error_) } {}

  backend_loader_t &
  backend_loader_t::operator=(backend_loader_t &&other) noexcept {
    if (this != &other) {
      unload();
      module_ = std::exchange(other.module_, nullptr);
      api_ = std::exchange(other.api_, nullptr);
      error_ = std::move(other.error_);
    }
    return *this;
  }

  backend_loader_t::~backend_loader_t() {
    unload();
  }

  bool
  backend_loader_t::load(const std::filesystem::path &absolute_path) {
    unload();
    error_.clear();
    if (!absolute_path.is_absolute()) {
      error_ = "backend_path_not_absolute";
      return false;
    }

    module_ = LoadLibraryExW(
      absolute_path.c_str(),
      nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
      const auto load_error = GetLastError();
      error_ = "backend_load_failed:" + std::to_string(load_error);
      return false;
    }

    const auto get_api = reinterpret_cast<foundation_truehdr_get_api_fn>(
      GetProcAddress(module_, FOUNDATION_TRUEHDR_GET_API_EXPORT));
    if (!get_api) {
      error_ = "backend_export_missing";
      unload();
      return false;
    }

    api_ = get_api(FOUNDATION_TRUEHDR_ABI_VERSION);
    if (!api_ || api_->abi_version != FOUNDATION_TRUEHDR_ABI_VERSION ||
        api_->struct_size < sizeof(foundation_truehdr_api_t)) {
      error_ = "backend_abi_mismatch";
      unload();
      return false;
    }
    if (!api_->create || !api_->process || !api_->flush || !api_->destroy) {
      error_ = "backend_api_incomplete";
      unload();
      return false;
    }
    return true;
  }

  void
  backend_loader_t::unload() {
    api_ = nullptr;
    if (module_) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
  }
}  // namespace platf::dxgi::rtx_hdr
