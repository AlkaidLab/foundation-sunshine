/**
 * @file src/pyrowave/capabilities.h
 * @brief Capability results used before a PyroWave session is negotiated.
 */
#pragma once

#include "config.h"
#include "types.h"

#include <cstdint>

namespace pyrowave {

  struct device_capabilities_t {
    api_version_t api_version {};
    bool vulkan_13 = false;
    bool subgroup = false;
    bool subgroup_size_control = false;
    bool shader_int16 = false;
    bool storage_buffer_8bit = false;
    bool external_d3d11_texture = false;
    bool external_timeline_fence = false;
    bool gpu_format_conversion = false;
  };

  struct client_capabilities_t {
    api_version_t api_version {};
    bool pyrowave = false;
    bool reassembly = false;
    bool partial_frame_decode = false;
    bool frame_deadline = false;
    bool block_aware_fec = false;
    chroma_e chroma = chroma_e::yuv420;
    transfer_e transfer = transfer_e::bt709;
  };

  struct capability_result_t {
    availability_e availability = availability_e::unavailable;
    failure_e failure = failure_e::none;
    device_capabilities_t device {};
    client_capabilities_t client {};
  };

  [[nodiscard]] capability_result_t
  evaluate_capabilities(
    const config_t &config,
    const device_capabilities_t &device,
    const client_capabilities_t &client) noexcept;

}  // namespace pyrowave
