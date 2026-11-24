/**
 * @file src/pyrowave/capabilities.cpp
 * @brief Capability intersection for the experimental PyroWave path.
 */
#include "capabilities.h"

namespace pyrowave {

  capability_result_t
  evaluate_capabilities(
    const config_t &config,
    const device_capabilities_t &device,
    const client_capabilities_t &client) noexcept {
    capability_result_t result {
      .availability = availability_e::unavailable,
      .failure = failure_e::none,
      .device = device,
      .client = client,
    };

    if (!config.enabled || !config.allow_experimental_client) {
      result.availability = availability_e::disabled;
      return result;
    }

    if (!validate(config)) {
      result.availability = availability_e::failed;
      result.failure = failure_e::configuration_invalid;
      return result;
    }

    if (!device.api_version.matches_exactly(config.expected_api_version) ||
        !client.api_version.matches_exactly(config.expected_api_version)) {
      result.availability = availability_e::failed;
      result.failure = failure_e::api_mismatch;
      return result;
    }

    if (!device.vulkan_13 || !device.subgroup || !device.subgroup_size_control ||
        !device.shader_int16 || !device.storage_buffer_8bit) {
      result.availability = availability_e::failed;
      result.failure = failure_e::vulkan_unsupported;
      return result;
    }

    if (!client.pyrowave || !client.reassembly || !client.frame_deadline ||
        !client.block_aware_fec ||
        client.chroma != config.chroma ||
        client.transfer != config.transfer) {
      result.availability = availability_e::failed;
      result.failure = failure_e::client_unsupported;
      return result;
    }

    if (config.require_external_interop && !device.external_d3d11_texture) {
      result.availability = availability_e::failed;
      result.failure = failure_e::image_import_failed;
      return result;
    }

    if (config.require_format_conversion && !device.gpu_format_conversion) {
      result.availability = availability_e::failed;
      result.failure = failure_e::format_conversion_failed;
      return result;
    }

    if (config.require_timeline_sync && !device.external_timeline_fence) {
      result.availability = availability_e::failed;
      result.failure = failure_e::synchronization_unsupported;
      return result;
    }

    result.availability = availability_e::available;
    return result;
  }

}  // namespace pyrowave
