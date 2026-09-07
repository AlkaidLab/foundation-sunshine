/**
 * @file src/platform/windows/rtx_hdr/backend_loader.cpp
 * @brief Safe loader for the optional TrueHDR backend DLL.
 */

#include "backend_loader.h"

#include <utility>

#include "src/platform/windows/postprocess/stage_abi.h"

namespace platf::dxgi::rtx_hdr {
  backend_loader_t::backend_loader_t(backend_loader_t &&other) noexcept:
      module_ { std::exchange(other.module_, nullptr) },
      api_ { std::exchange(other.api_, nullptr) },
      stage_api_ { std::exchange(other.stage_api_, nullptr) },
      error_ { std::move(other.error_) } {}

  backend_loader_t &
  backend_loader_t::operator=(backend_loader_t &&other) noexcept {
    if (this != &other) {
      unload();
      module_ = std::exchange(other.module_, nullptr);
      api_ = std::exchange(other.api_, nullptr);
      stage_api_ = std::exchange(other.stage_api_, nullptr);
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

    // Stage ABI v2 is probed first: a v2 DLL never exports the v1 symbol, and
    // a v1 DLL never exports the v2 one, so probe order decides cleanly.
    if (const auto get_stage_api = reinterpret_cast<foundation_stage_get_api_fn>(
          GetProcAddress(module_, FOUNDATION_STAGE_GET_API_EXPORT))) {
      stage_api_ = get_stage_api(FOUNDATION_STAGE_ABI_VERSION);
      if (!stage_api_ || stage_api_->abi_version != FOUNDATION_STAGE_ABI_VERSION ||
          stage_api_->struct_size < sizeof(foundation_stage_api_t)) {
        stage_api_ = nullptr;
        error_ = "stage_abi_mismatch";
        unload();
        return false;
      }
      if (!stage_api_->caps || !stage_api_->create || !stage_api_->process ||
          !stage_api_->flush || !stage_api_->destroy) {
        stage_api_ = nullptr;
        error_ = "stage_api_incomplete";
        unload();
        return false;
      }
      const auto *caps = stage_api_->caps();
      if (!caps || caps->struct_size < sizeof(foundation_stage_caps_t) ||
          !caps->name || caps->max_frames_out == 0) {
        stage_api_ = nullptr;
        error_ = "stage_caps_invalid";
        unload();
        return false;
      }
      return true;
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
    stage_api_ = nullptr;
    if (module_) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
  }
}  // namespace platf::dxgi::rtx_hdr
