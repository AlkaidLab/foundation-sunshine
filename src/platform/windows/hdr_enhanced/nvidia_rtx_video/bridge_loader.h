/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/bridge_loader.h
 * @brief Safe loader for the optional RTX Video bridge DLL.
 */
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

#include "bridge_abi.h"

namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video {
  class bridge_loader_t {
  public:
    bridge_loader_t() = default;
    bridge_loader_t(const bridge_loader_t &) = delete;
    bridge_loader_t &
    operator=(const bridge_loader_t &) = delete;
    bridge_loader_t(bridge_loader_t &&other) noexcept;
    bridge_loader_t &
    operator=(bridge_loader_t &&other) noexcept;
    ~bridge_loader_t();

    bool
    load(const std::filesystem::path &absolute_path);

    void
    unload();

    const foundation_truehdr_bridge_api_t *
    api() const {
      return api_;
    }

    const std::string &
    error() const {
      return error_;
    }

    explicit operator bool() const {
      return module_ != nullptr && api_ != nullptr;
    }

  private:
    HMODULE module_ = nullptr;
    const foundation_truehdr_bridge_api_t *api_ = nullptr;
    std::string error_;
  };
}  // namespace platf::dxgi::hdr_enhanced::nvidia_rtx_video
