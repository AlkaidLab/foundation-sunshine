/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_loader.h
 * @brief Verified loader for the optional MSVC NGX adapter DLL.
 */
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

#include "adapter_abi.h"

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video {
  inline constexpr wchar_t ADAPTER_FILENAME[] = L"foundation_rtx_video_adapter.dll";
  inline constexpr wchar_t RUNTIME_FILENAME[] = L"nvngx_truehdr.dll";

  class adapter_loader_t {
  public:
    adapter_loader_t() = default;
    adapter_loader_t(const adapter_loader_t &) = delete;
    adapter_loader_t &
    operator=(const adapter_loader_t &) = delete;
    adapter_loader_t(adapter_loader_t &&other) noexcept;
    adapter_loader_t &
    operator=(adapter_loader_t &&other) noexcept;
    ~adapter_loader_t();

    bool
    load(const std::filesystem::path &adapter_path, const std::filesystem::path &runtime_path);

    void
    unload();

    const foundation_truehdr_adapter_api_t *
    api() const {
      return api_;
    }

    const std::string &
    error() const {
      return error_;
    }

    const wchar_t *
    runtime_directory() const {
      return runtime_directory_.c_str();
    }

    explicit operator bool() const {
      return module_ != nullptr && api_ != nullptr;
    }

  private:
    HMODULE module_ = nullptr;
    HANDLE adapter_file_ = INVALID_HANDLE_VALUE;
    HANDLE runtime_file_ = INVALID_HANDLE_VALUE;
    std::wstring runtime_directory_;
    const foundation_truehdr_adapter_api_t *api_ = nullptr;
    std::string error_;
  };
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video
