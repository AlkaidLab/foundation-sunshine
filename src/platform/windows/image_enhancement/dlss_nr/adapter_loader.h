/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/adapter_loader.h
 * @brief Verified loader for the optional MSVC NGX adapter DLL (DLSS NR).
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <windows.h>

#include "adapter_abi.h"

namespace platf::dxgi::image_enhancement::dlss_nr {
  inline constexpr wchar_t ADAPTER_FILENAME[] = L"foundation_dlssnr_adapter.dll";
  inline constexpr wchar_t RUNTIME_FILENAME[] = L"nvngx_dlssnr.dll";

  class dlssnr_adapter_loader_t {
  public:
    dlssnr_adapter_loader_t() = default;
    dlssnr_adapter_loader_t(const dlssnr_adapter_loader_t &) = delete;
    dlssnr_adapter_loader_t &
    operator=(const dlssnr_adapter_loader_t &) = delete;
    dlssnr_adapter_loader_t(dlssnr_adapter_loader_t &&other) noexcept;
    dlssnr_adapter_loader_t &
    operator=(dlssnr_adapter_loader_t &&other) noexcept;
    ~dlssnr_adapter_loader_t();

    /**
     * Load and verify the adapter and its NGX runtime DLL.
     *
     * The adapter digest is pinned at build time when the adapter component
     * is built alongside Sunshine. The runtime DLL comes from outside the
     * distribution, so its digest is pinned by the persisted component
     * settings (optional: an unpinned runtime is accepted but its digest is
     * logged for the record).
     */
    bool
    load(const std::filesystem::path &adapter_path, const std::filesystem::path &runtime_path,
      std::optional<std::string_view> expected_runtime_digest = std::nullopt);

    void
    unload();

    const foundation_dlssnr_adapter_api_t *
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
    const foundation_dlssnr_adapter_api_t *api_ = nullptr;
    std::string error_;
  };
}  // namespace platf::dxgi::image_enhancement::dlss_nr
