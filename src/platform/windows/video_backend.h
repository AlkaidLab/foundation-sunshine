/**
 * @file src/platform/windows/video_backend.h
 * @brief Windows video pipeline backend selection state.
 */
#pragma once

#include <optional>
#include <string_view>

namespace platf::dxgi::video_backend {
  enum class windows_video_backend_e {
    automatic,
    d3d11,
    d3d12,
  };

  enum class effective_backend_e {
    unavailable,
    d3d11,
    hybrid,
    d3d12,
  };

  enum class fallback_reason_e {
    none,
    build_stage_unavailable,
    d3d12_device_failed,
    shader_model_unsupported,
    shared_resource_failed,
    shared_fence_failed,
    topology_benchmark_rejected,
    encoder_backend_unavailable,
    encoder_resource_registration_failed,
    device_removed,
    runtime_fence_failed,
  };

  struct selection_t {
    windows_video_backend_e requested = windows_video_backend_e::automatic;
    effective_backend_e effective = effective_backend_e::d3d11;
    fallback_reason_e fallback = fallback_reason_e::none;
    bool strict = false;
    bool environment_override = false;
    bool invalid_value = false;

    [[nodiscard]] bool
    pipeline_available() const {
      return effective != effective_backend_e::unavailable;
    }
  };

  [[nodiscard]] inline std::optional<windows_video_backend_e>
  parse(std::string_view value) {
    if (value.empty() || value == "auto") {
      return windows_video_backend_e::automatic;
    }
    if (value == "d3d11") {
      return windows_video_backend_e::d3d11;
    }
    if (value == "d3d12") {
      return windows_video_backend_e::d3d12;
    }
    return std::nullopt;
  }

  [[nodiscard]] inline std::string_view
  to_string(windows_video_backend_e backend) {
    switch (backend) {
      case windows_video_backend_e::automatic:
        return "auto";
      case windows_video_backend_e::d3d11:
        return "d3d11";
      case windows_video_backend_e::d3d12:
        return "d3d12";
    }
    return "auto";
  }

  [[nodiscard]] inline std::string_view
  to_string(effective_backend_e backend) {
    switch (backend) {
      case effective_backend_e::unavailable:
        return "unavailable";
      case effective_backend_e::d3d11:
        return "d3d11";
      case effective_backend_e::hybrid:
        return "hybrid";
      case effective_backend_e::d3d12:
        return "d3d12";
    }
    return "unavailable";
  }

  [[nodiscard]] inline std::string_view
  to_string(fallback_reason_e reason) {
    switch (reason) {
      case fallback_reason_e::none:
        return "none";
      case fallback_reason_e::build_stage_unavailable:
        return "build_stage_unavailable";
      case fallback_reason_e::d3d12_device_failed:
        return "d3d12_device_failed";
      case fallback_reason_e::shader_model_unsupported:
        return "shader_model_unsupported";
      case fallback_reason_e::shared_resource_failed:
        return "shared_resource_failed";
      case fallback_reason_e::shared_fence_failed:
        return "shared_fence_failed";
      case fallback_reason_e::topology_benchmark_rejected:
        return "topology_benchmark_rejected";
      case fallback_reason_e::encoder_backend_unavailable:
        return "encoder_backend_unavailable";
      case fallback_reason_e::encoder_resource_registration_failed:
        return "encoder_resource_registration_failed";
      case fallback_reason_e::device_removed:
        return "device_removed";
      case fallback_reason_e::runtime_fence_failed:
        return "runtime_fence_failed";
    }
    return "none";
  }

  /**
   * Resolve and lock the requested backend for one video pipeline.
   *
   * PR2 deliberately passes d3d12_stage_available=false. Later stages may set it
   * only after all capability, topology, and performance gates have passed.
   *
   * This never returns `unavailable`: refusing to encode is a decision only a
   * real, failed D3D12 initialization can justify. See
   * apply_d3d12_initialization().
   */
  [[nodiscard]] inline selection_t
  resolve(
    std::string_view configured,
    std::optional<std::string_view> environment,
    bool strict_requested,
    bool d3d12_stage_available) {
    selection_t result;
    const auto requested_value = environment.value_or(configured);
    result.environment_override = environment.has_value();

    const auto parsed = parse(requested_value);
    result.requested = parsed.value_or(windows_video_backend_e::automatic);
    result.invalid_value = !parsed.has_value();
    result.strict =
      strict_requested && result.requested == windows_video_backend_e::d3d12;

    if (result.requested == windows_video_backend_e::d3d11) {
      return result;
    }
    if (d3d12_stage_available) {
      result.effective = effective_backend_e::d3d12;
      return result;
    }

    result.fallback = fallback_reason_e::build_stage_unavailable;
    return result;
  }

  /**
   * Fold the outcome of the real D3D12 initialization attempt into a selection.
   *
   * Strict mode means "tell me loudly when the D3D12 backend I asked for did not
   * come up", not "refuse to stream". Only an actual failed attempt may take the
   * pipeline down, and only when d3d12 was requested explicitly -- otherwise a
   * strict flag left in the environment would kill every encoder on a build
   * where the D3D12 stage is simply not wired up yet.
   */
  inline void
  apply_d3d12_initialization(
    selection_t &selection,
    bool success,
    fallback_reason_e reason) {
    if (selection.requested != windows_video_backend_e::d3d12) {
      return;
    }
    if (success) {
      selection.fallback = fallback_reason_e::none;
      return;
    }

    selection.fallback = reason;
    if (selection.strict) {
      selection.effective = effective_backend_e::unavailable;
    }
  }
}  // namespace platf::dxgi::video_backend
