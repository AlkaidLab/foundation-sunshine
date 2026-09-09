/**
 * @file src/platform/windows/postprocess/chain_validator.h
 * @brief Pure chain validation: reachability, bounds, and plan generation.
 *
 * Implements rules R1–R6 of docs/postprocess_chain.md §5.2 as pure functions
 * over stage declarations — no GPU, no DLL, no session state. The executor
 * and the WebUI consume the resulting plan; identical inputs always produce
 * an identical plan (deterministic ordering, no timestamps).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/platform/windows/postprocess/stage_abi.h"

namespace platf::dxgi::postprocess {

  /// One chain link as declared to the validator (from stage caps or
  /// synthesized for a legacy v1 backend via synthesize_v1_declaration()).
  struct stage_declaration_t {
    std::string name;
    std::uint32_t abi_version = FOUNDATION_STAGE_ABI_VERSION;

    std::uint32_t input_domain = FOUNDATION_STAGE_DOMAIN_UNKNOWN;
    std::uint32_t input_encoding = FOUNDATION_STAGE_ENCODING_AUTOMATIC;
    std::uint32_t output_domain = FOUNDATION_STAGE_DOMAIN_UNKNOWN;
    std::uint32_t output_encoding = FOUNDATION_STAGE_ENCODING_AUTOMATIC;

    std::uint32_t resolution_behavior = FOUNDATION_STAGE_RESOLUTION_SAME;
    float min_scale = 1.0f;
    float max_scale = 1.0f;

    bool temporal = false;
    std::uint32_t max_frames_out = 1;
  };

  /// Caps a legacy v1 TrueHDR backend would have declared: SDR-in, scRGB-out,
  /// same resolution, not temporal. Used when migrating `rtx_hdr` apps to the
  /// chain model (docs §2.5).
  stage_declaration_t
  synthesize_v1_declaration(std::string name);

  /// Session-side bounds the plan must respect (encoder surface limits).
  struct chain_limits_t {
    float max_resolution_scale = 2.0f;
    std::uint32_t max_frame_multiplier = 2;
  };

  /// One entry of the generated plan: either a user DLL stage or an
  /// auto-inserted builtin conversion between two neighboring stages.
  struct plan_entry_t {
    std::string name;          // dll caps name, or builtin conversion id
    bool builtin_conversion = false;
  };

  struct chain_plan_t {
    bool ok = false;
    std::vector<std::string> errors;    // rejection reasons (rule ids prefix)
    std::vector<std::string> warnings;  // accepted-with-cost reasons

    std::vector<plan_entry_t> entries;  // execution order, conversions included
    float resolution_scale = 1.0f;      // product of stage scale bounds
    std::uint32_t frame_multiplier = 1; // product of temporal max_frames_out
    bool temporal = false;              // at least one temporal stage
  };

  /**
   * Validate capture_leg → stages… → encoder_leg.
   *
   * The leg contracts model what display_vram's convert() actually consumes:
   * the HDR wire path takes FP16 scRGB (it performs PQ/P010 itself), the SDR
   * wire path takes unorm8 Rec.709. Pass those as the encoder-leg domain pair.
   *
   * Deterministic: same inputs → same plan. Empty `stages` with matching legs
   * yields a trivially ok plan (no-op chain).
   */
  chain_plan_t
  validate_chain(
    const std::vector<stage_declaration_t> &stages,
    std::uint32_t capture_domain,
    std::uint32_t capture_encoding,
    std::uint32_t encoder_domain,
    std::uint32_t encoder_encoding,
    const chain_limits_t &limits = {});
}  // namespace platf::dxgi::postprocess
