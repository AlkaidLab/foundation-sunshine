/**
 * @file src/platform/windows/d3d12/d3d12_hdr_analysis.h
 * @brief Experimental D3D12 two-pass HDR luminance analysis backend.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <d3d11_4.h>

#include "d3d12_device.h"
#include "d3d12_hdr_statistics.h"

namespace platf::dxgi::d3d12 {
  struct writable_snapshot_t {
    std::size_t slot = 0;
    std::uint64_t generation = 0;

    // D3D11 views onto the shared D3D12 snapshot texture. `uav` lets the
    // D3D11 conversion shader write cell statistics straight into the shared
    // resource, so no staging copy is needed before the D3D12 compute pass.
    ID3D11Texture2D *texture = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    ID3D11UnorderedAccessView *pq_uav = nullptr;
  };

  struct completed_hdr_result_t {
    hdr_final_result_t result;
    std::uint64_t source_frame = 0;
    std::uint64_t generation = 0;
    // Queue timestamp spans include preemption. Clock calibration only estimates
    // submission-to-start latency; polling also includes the caller's cadence.
    struct timing_t {
      double pass1_ms = 0;
      double pass2_ms = 0;
      double readback_ms = 0;
      double gpu_total_ms = 0;
      std::optional<double> submit_to_start_ms;
      double poll_latency_ms = 0;
    };
    std::optional<timing_t> timing;
  };

  struct hdr_analysis_init_result_t {
    bool success = false;
    HRESULT hresult = S_OK;
    std::string_view stage = "none";
  };

  class hdr_analysis_t {
  public:
    hdr_analysis_t();
    hdr_analysis_t(const hdr_analysis_t &) = delete;
    hdr_analysis_t &
    operator=(const hdr_analysis_t &) = delete;
    ~hdr_analysis_t();

    [[nodiscard]] static bool
    built();

    /** Number of timed-out analyzers retaining resources until GPU completion. */
    [[nodiscard]] static std::uint32_t
    pending_resource_retirements();

    [[nodiscard]] hdr_analysis_init_result_t
    initialize(
      device_t &foundation,
      ID3D11Device *d3d11_device,
      ID3D11DeviceContext *d3d11_context,
      std::uint32_t analysis_width,
      std::uint32_t analysis_height,
      std::uint32_t source_width,
      std::uint32_t source_height,
      float max_analysis_nits,
      std::uint64_t generation,
      bool timing_enabled = false);

    [[nodiscard]] bool
    available() const;

    [[nodiscard]] std::optional<writable_snapshot_t>
    try_acquire_snapshot();

    [[nodiscard]] bool
    cancel_snapshot(const writable_snapshot_t &snapshot);

    [[nodiscard]] bool
    submit(
      const writable_snapshot_t &snapshot,
      std::uint64_t source_frame,
      bool measure_timing = false);

    [[nodiscard]] std::optional<completed_hdr_result_t>
    poll();

    [[nodiscard]] HRESULT
    failure_hresult() const;

    [[nodiscard]] std::string_view
    failure_stage() const;

    void
    disable();

  private:
    void
    release_resources();

    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace platf::dxgi::d3d12
