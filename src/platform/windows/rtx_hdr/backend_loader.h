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

  /**
   * Best-effort search for an NVIDIA TrueHDR runtime (nvngx_truehdr.dll) that
   * already exists on this machine. Informational only: it feeds setup hints
   * in logs and the Web UI; nothing is loaded from the result automatically.
   * Searches the directory of backend_path (when given), then the NVIDIA
   * Program Files locations, with bounded recursion.
   */
  std::string
  locate_system_truehdr_runtime(const std::filesystem::path &backend_path = {});
}  // namespace platf::dxgi::rtx_hdr
