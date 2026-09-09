/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_loader.cpp
 * @brief Verified loader for the optional MSVC NGX adapter DLL.
 */

#include "adapter_loader.h"

#include <array>
#include <string_view>
#include <utility>

#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
  #include <bcrypt.h>
  #include "rtx_video_trust.h"
#endif

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video {
  namespace {
#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
    bool
    lock_and_verify(
      const std::filesystem::path &path,
      std::string_view expected,
      std::string_view label,
      HANDLE &file,
      std::string &error) {
      file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        error = std::string(label) + "_open_failed";
        return false;
      }

      LARGE_INTEGER size {};
      if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024 * 1024) {
        error = std::string(label) + "_size_invalid";
        return false;
      }

      BCRYPT_ALG_HANDLE algorithm = nullptr;
      BCRYPT_HASH_HANDLE hash = nullptr;
      const auto cleanup = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
      };
      if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
          BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        cleanup();
        error = std::string(label) + "_digest_failed";
        return false;
      }

      std::array<unsigned char, 64 * 1024> buffer;
      LONGLONG remaining = size.QuadPart;
      while (remaining > 0) {
        DWORD received = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr) || received == 0 ||
            BCryptHashData(hash, buffer.data(), received, 0) < 0) {
          cleanup();
          error = std::string(label) + "_digest_failed";
          return false;
        }
        remaining -= received;
      }

      std::array<unsigned char, 32> bytes {};
      if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) {
        cleanup();
        error = std::string(label) + "_digest_failed";
        return false;
      }
      cleanup();

      std::array<char, 65> actual {};
      constexpr char hex[] = "0123456789abcdef";
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        actual[index * 2] = hex[bytes[index] >> 4];
        actual[index * 2 + 1] = hex[bytes[index] & 15];
      }
      if (std::string_view(actual.data(), 64) != expected) {
        error = std::string(label) + "_untrusted";
        return false;
      }
      return true;
    }
#endif
  }  // namespace

  adapter_loader_t::adapter_loader_t(adapter_loader_t &&other) noexcept:
      module_ { std::exchange(other.module_, nullptr) },
      adapter_file_ { std::exchange(other.adapter_file_, INVALID_HANDLE_VALUE) },
      runtime_file_ { std::exchange(other.runtime_file_, INVALID_HANDLE_VALUE) },
      runtime_directory_ { std::move(other.runtime_directory_) },
      api_ { std::exchange(other.api_, nullptr) },
      error_ { std::move(other.error_) } {}

  adapter_loader_t &
  adapter_loader_t::operator=(adapter_loader_t &&other) noexcept {
    if (this != &other) {
      unload();
      module_ = std::exchange(other.module_, nullptr);
      adapter_file_ = std::exchange(other.adapter_file_, INVALID_HANDLE_VALUE);
      runtime_file_ = std::exchange(other.runtime_file_, INVALID_HANDLE_VALUE);
      runtime_directory_ = std::move(other.runtime_directory_);
      api_ = std::exchange(other.api_, nullptr);
      error_ = std::move(other.error_);
    }
    return *this;
  }

  adapter_loader_t::~adapter_loader_t() {
    unload();
  }

  bool
  adapter_loader_t::load(
    const std::filesystem::path &adapter_path,
    const std::filesystem::path &runtime_path) {
    unload();
    error_.clear();
    if (!adapter_path.is_absolute()) {
      error_ = "adapter_path_not_absolute";
      return false;
    }
    if (!runtime_path.is_absolute()) {
      error_ = "runtime_path_not_absolute";
      return false;
    }

#ifdef SUNSHINE_RTX_VIDEO_ADAPTER
    std::error_code filesystem_error;
    const auto adapter_directory = std::filesystem::canonical(adapter_path.parent_path(), filesystem_error);
    if (filesystem_error) {
      error_ = "adapter_directory_invalid";
      return false;
    }
    const auto runtime_directory = std::filesystem::canonical(runtime_path.parent_path(), filesystem_error);
    if (filesystem_error || runtime_directory != adapter_directory) {
      error_ = "component_layout_invalid";
      return false;
    }
    if (!lock_and_verify(adapter_path, SUNSHINE_RTX_VIDEO_ADAPTER_SHA256, "adapter", adapter_file_, error_) ||
        !lock_and_verify(runtime_path, SUNSHINE_RTX_VIDEO_RUNTIME_SHA256, "runtime", runtime_file_, error_)) {
      unload();
      return false;
    }
    runtime_directory_ = runtime_directory.wstring();
#elif !defined(FAKE_TRUEHDR_ADAPTER_PATH)
    error_ = "adapter_not_built";
    return false;
#else
    runtime_file_ = CreateFileW(runtime_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (runtime_file_ == INVALID_HANDLE_VALUE) {
      error_ = "runtime_open_failed";
      return false;
    }
    runtime_directory_ = runtime_path.parent_path().wstring();
#endif

    module_ = LoadLibraryExW(
      adapter_path.c_str(),
      nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
      const auto load_error = GetLastError();
      error_ = "adapter_load_failed:" + std::to_string(load_error);
      unload();
      return false;
    }

    const auto get_api = reinterpret_cast<foundation_truehdr_adapter_get_api_fn>(
      GetProcAddress(module_, FOUNDATION_TRUEHDR_ADAPTER_GET_API_EXPORT));
    if (!get_api) {
      error_ = "adapter_export_missing";
      unload();
      return false;
    }

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
    error_.clear();
    return true;
  }

  void
  adapter_loader_t::unload() {
    api_ = nullptr;
    if (module_) {
      FreeLibrary(module_);
      module_ = nullptr;
    }
    if (runtime_file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(runtime_file_);
      runtime_file_ = INVALID_HANDLE_VALUE;
    }
    if (adapter_file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(adapter_file_);
      adapter_file_ = INVALID_HANDLE_VALUE;
    }
    runtime_directory_.clear();
  }
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video
