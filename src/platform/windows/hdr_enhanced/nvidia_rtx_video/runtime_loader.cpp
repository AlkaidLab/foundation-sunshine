/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/runtime_loader.cpp
 * @brief Verified NVIDIA runtime ownership and access to the built-in NGX adapter.
 */

#include "runtime_loader.h"

#include <utility>
#include <array>

#ifdef SUNSHINE_RTX_VIDEO_STATIC
  #include <bcrypt.h>
  #include <boost/scope/scope_exit.hpp>
  #include "rtx_video_runtime.h"
#endif

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video {
  runtime_loader_t::runtime_loader_t(runtime_loader_t &&other) noexcept:
      module_ { std::exchange(other.module_, nullptr) },
      file_ { std::exchange(other.file_, INVALID_HANDLE_VALUE) },
      directory_ { std::move(other.directory_) },
      api_ { std::exchange(other.api_, nullptr) },
      error_ { std::move(other.error_) } {}

  runtime_loader_t &
  runtime_loader_t::operator=(runtime_loader_t &&other) noexcept {
    if (this != &other) {
      unload();
      module_ = std::exchange(other.module_, nullptr);
      file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
      directory_ = std::move(other.directory_);
      api_ = std::exchange(other.api_, nullptr);
      error_ = std::move(other.error_);
    }
    return *this;
  }

  runtime_loader_t::~runtime_loader_t() {
    unload();
  }

  bool
  runtime_loader_t::load(const std::filesystem::path &absolute_path) {
    unload();
    error_.clear();
    if (!absolute_path.is_absolute()) {
      error_ = "runtime_path_not_absolute";
      return false;
    }

#ifdef SUNSHINE_RTX_VIDEO_STATIC
    // 校验与加载共用只读句柄；实例存活期间禁止写入或替换运行库。
    file_ = CreateFileW(absolute_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      error_ = "runtime_open_failed";
      return false;
    }
    auto rollback = boost::scope::scope_exit([&] { unload(); });
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024 * 1024) {
      error_ = "runtime_size_invalid";
      return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto cleanup = boost::scope::scope_exit([&] {
      if (hash) BCryptDestroyHash(hash);
      if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    });
    error_ = "runtime_digest_failed";
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::array<unsigned char, 64 * 1024> buffer;
    LONGLONG remaining = size.QuadPart;
    while (remaining > 0) {
      DWORD received = 0;
      if (!ReadFile(file_, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr) || received == 0) return false;
      if (BCryptHashData(hash, buffer.data(), received, 0) < 0) return false;
      remaining -= received;
    }
    std::array<unsigned char, 32> bytes {};
    if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) return false;
    std::string actual;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
      actual += hex[byte >> 4];
      actual += hex[byte & 15];
    }
    if (actual != SUNSHINE_RTX_VIDEO_RUNTIME_SHA256) {
      error_ = "runtime_untrusted";
      return false;
    }
    directory_ = absolute_path.parent_path().wstring();
#elif !defined(FAKE_TRUEHDR_RUNTIME_PATH)
    error_ = "backend_not_built";
    return false;
#endif

    module_ = LoadLibraryExW(
      absolute_path.c_str(),
      nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
      const auto load_error = GetLastError();
      error_ = "runtime_load_failed:" + std::to_string(load_error);
      return false;
    }

#ifdef SUNSHINE_RTX_VIDEO_STATIC
    const auto get_api = &foundation_truehdr_adapter_get_api;
#else
    const auto get_api = reinterpret_cast<foundation_truehdr_adapter_get_api_fn>(
      GetProcAddress(module_, FOUNDATION_TRUEHDR_ADAPTER_GET_API_EXPORT));
    if (!get_api) {
      error_ = "adapter_export_missing";
      unload();
      return false;
    }
#endif

    api_ = get_api(FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION);
    if (!api_ || api_->abi_version != FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION ||
        api_->struct_size < sizeof(foundation_truehdr_adapter_api_t)) {
      error_ = "adapter_abi_mismatch";
      unload();
      return false;
    }
    if (!api_->create || !api_->process || !api_->flush || !api_->destroy) {
      error_ = "adapter_api_incomplete";
      unload();
      return false;
    }
#ifdef SUNSHINE_RTX_VIDEO_STATIC
    rollback.set_active(false);
#endif
    error_.clear();
    return true;
  }

  void
  runtime_loader_t::unload() {
    api_ = nullptr;
    if (module_) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
      file_ = INVALID_HANDLE_VALUE;
    }
    directory_.clear();
  }
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video
