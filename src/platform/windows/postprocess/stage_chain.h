/**
 * @file src/platform/windows/postprocess/stage_chain.h
 * @brief Chain executor and v2 stage driver for the post-process pipeline.
 *
 * chain_filter_t composes existing pre_encode_filter_t implementations into
 * the user-configurable chain: each child's output feeds the next child's
 * input, and a failed child is bypassed (excluded for the session, reported
 * per slot). Domain continuity is enforced by the children themselves — every
 * filter validates its own input contract — so bypass needs no runtime
 * re-validation: a removal that breaks the domain chain cascades into child
 * input-contract failures until the chain empties, which surfaces as a chain
 * failure to the outer failover (docs/postprocess_chain.md §2.2, R8).
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>

#include "src/platform/windows/pre_encode_filter.h"
#include "src/platform/windows/postprocess/stage_abi.h"
#include "src/platform/windows/rtx_hdr/backend_loader.h"

namespace platf::dxgi::postprocess {

  /// Runtime state of one chain slot, for status reporting
  /// (/api/runtime/postprocess) and tests.
  struct stage_state_t {
    std::string name;
    std::string state { "active" };  // active | bypassed
    std::string failure_reason;
  };

  /**
   * Composite pre-encode filter running child filters in order. Construction
   * requires at least one child; use make_stage_chain().
   */
  class chain_filter_t final: public pre_encode_filter_t {
  public:
    // Constructed via make_stage_chain; public for std::make_unique (the
    // class is final and only this header builds it).
    explicit chain_filter_t(std::vector<std::unique_ptr<pre_encode_filter_t>> stages);

    bool
    requires_detached_input() const override;

    filter_result_t
    process(const gpu_frame_view_t &input) override;

    void
    flush() override;

    std::string_view
    backend_name() const override;

    bool
    degraded() const override;

    std::string_view
    failure_reason() const override;

    /// Per-slot snapshot in chain order; bypassed slots keep their name and
    /// last failure reason for the session.
    const std::vector<stage_state_t>
    stage_states() const;

    const std::vector<pre_encode_filter_t *>  // test/inspection access
    active_stages() const;

    std::uint32_t
    bypassed_count() const {
      return bypassed_;
    }

  private:
    struct slot_t {
      std::unique_ptr<pre_encode_filter_t> filter;
      stage_state_t state;
    };
    std::vector<slot_t> slots_;
    std::uint32_t bypassed_ = 0;
    std::string failure_reason_;
  };

  /// Build a chain from non-empty `stages`; returns null when empty. Child
  /// order is execution order.
  std::unique_ptr<pre_encode_filter_t>
  make_stage_chain(std::vector<std::unique_ptr<pre_encode_filter_t>> stages);

  /**
   * Drive a stage-ABI v2 DLL as a pre-encode filter. The loader must have
   * loaded a v2 stage (stage_v2()); temporal stages are refused until the
   * capture loop drives multi-frame batches (docs Phase 3) — this function
   * logs and returns null for them. `scale_hint` sizes the host-allocated
   * output textures (1.0 for SAME stages).
   */
  std::unique_ptr<pre_encode_filter_t>
  make_dll_stage_filter(
    rtx_hdr::backend_loader_t loader,
    ID3D11Device *device,
    ID3D11DeviceContext *device_context,
    float scale_hint,
    const std::string &params_json);
}  // namespace platf::dxgi::postprocess
