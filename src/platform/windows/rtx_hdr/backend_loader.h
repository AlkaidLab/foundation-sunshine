/**
 * @file src/platform/windows/rtx_hdr/backend_loader.h
 * @brief Safe loader for the optional TrueHDR backend DLL.
 */
#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

#include "backend_abi.h"

namespace platf::dxgi::rtx_hdr {
  class backend_loader_t {
  public:
    backend_loader_t() = default;
    backend_loader_t(const backend_loader_t &) = delete;
    backend_loader_t &
    operator=(const backend_loader_t &) = delete;
    backend_loader_t(backend_loader_t &&other) noexcept;
    backend_loader_t &
    operator=(backend_loader_t &&other) noexcept;
    ~backend_loader_t();

    bool
    load(const std::filesystem::path &absolute_path);

    void
    unload();

    const foundation_truehdr_api_t *
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
    const foundation_truehdr_api_t *api_ = nullptr;
    std::string error_;
  };
}  // namespace platf::dxgi::rtx_hdr
