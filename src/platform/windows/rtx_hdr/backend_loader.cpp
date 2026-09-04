/**
 * @file src/platform/windows/rtx_hdr/backend_loader.cpp
 * @brief Safe loader for the optional TrueHDR backend DLL.
 */

#include "backend_loader.h"

#include <cstdlib>
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

  namespace {
    constexpr auto kTrueHdrRuntimeName = L"nvngx_truehdr.dll";
    constexpr std::size_t kMaxScanEntries = 20000;

    bool
    is_truehdr_runtime(const std::filesystem::directory_entry &entry) {
      std::error_code ec;
      return entry.is_regular_file(ec) && entry.path().filename() == std::filesystem::path { kTrueHdrRuntimeName };
    }

    std::string
    search_directory(const std::filesystem::path &root, int max_depth) {
      std::error_code ec;
      if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return {};
      }
      std::filesystem::recursive_directory_iterator it {
        root, std::filesystem::directory_options::skip_permission_denied, ec
      };
      for (std::size_t scanned = 0; it != std::filesystem::end(it); it.increment(ec)) {
        if (ec || ++scanned > kMaxScanEntries) {
          return {};
        }
        if (it.depth() >= max_depth) {
          it.disable_recursion_pending();
        }
        if (is_truehdr_runtime(*it)) {
          return it->path().string();
        }
      }
      return {};
    }
  }  // namespace

  std::string
  locate_system_truehdr_runtime(const std::filesystem::path &backend_path) {
    if (!backend_path.empty()) {
      if (auto hint = search_directory(backend_path.parent_path(), 0); !hint.empty()) {
        return hint;
      }
    }
    const std::filesystem::path program_files {
      std::getenv("ProgramFiles") ? std::getenv("ProgramFiles") : "C:/Program Files"
    };
    if (auto hint = search_directory(program_files / L"NVIDIA Corporation" / L"NVIDIA app", 3); !hint.empty()) {
      return hint;
    }
    return search_directory(program_files / L"NVIDIA Corporation", 2);
  }
}  // namespace platf::dxgi::rtx_hdr
