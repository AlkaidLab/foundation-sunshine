/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/runtime_loader.h
 * @brief Verified NVIDIA runtime ownership and access to the built-in NGX adapter.
 */
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

#include "adapter_abi.h"

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video {
  class runtime_loader_t {
  public:
    runtime_loader_t() = default;
    runtime_loader_t(const runtime_loader_t &) = delete;
    runtime_loader_t &
    operator=(const runtime_loader_t &) = delete;
    runtime_loader_t(runtime_loader_t &&other) noexcept;
    runtime_loader_t &
    operator=(runtime_loader_t &&other) noexcept;
    ~runtime_loader_t();

    bool
    load(const std::filesystem::path &absolute_path);

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
    runtime_directory() const { return directory_.c_str(); }

    explicit operator bool() const {
      return module_ != nullptr && api_ != nullptr;
    }

  private:
    HMODULE module_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring directory_;
    const foundation_truehdr_adapter_api_t *api_ = nullptr;
    std::string error_;
  };
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video
