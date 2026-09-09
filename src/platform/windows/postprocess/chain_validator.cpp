/**
 * @file src/platform/windows/postprocess/chain_validator.cpp
 * @brief Pure chain validation (see chain_validator.h).
 */
#include "chain_validator.h"

#include <algorithm>

namespace platf::dxgi::postprocess {
  namespace {
    struct io_t {
      std::uint32_t domain;
      std::uint32_t encoding;
    };

    bool
    operator==(const io_t &a, const io_t &b) {
      return a.domain == b.domain && a.encoding == b.encoding;
    }

    std::string
    io_label(const io_t &io) {
      // Stable, human-readable identifiers for error and warning messages.
      const char *domain = "unknown";
      switch (io.domain) {
        case FOUNDATION_STAGE_DOMAIN_SDR_REC709: domain = "sdr_rec709"; break;
        case FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB: domain = "linear_scrgb"; break;
        case FOUNDATION_STAGE_DOMAIN_PQ_BT2020: domain = "pq_bt2020"; break;
        case FOUNDATION_STAGE_DOMAIN_HLG_BT2020: domain = "hlg_bt2020"; break;
        default: break;
      }
      const char *encoding = "automatic";
      switch (io.encoding) {
        case FOUNDATION_STAGE_ENCODING_UNORM8: encoding = "unorm8"; break;
        case FOUNDATION_STAGE_ENCODING_FLOAT16: encoding = "float16"; break;
        default: break;
      }
      return std::string { domain } + "/" + encoding;
    }

    /// Builtin conversion edges the host can auto-insert at a boundary whose
    /// sides do not meet. v1 ships the linearize pass; extend here as more
    /// builtin passes land (docs/postprocess_chain.md §5.1).
    struct builtin_edge_t {
      io_t from;
      io_t to;
      const char *id;
    };
    const builtin_edge_t kBuiltinEdges[] = {
      { { FOUNDATION_STAGE_DOMAIN_SDR_REC709, FOUNDATION_STAGE_ENCODING_UNORM8 },
        { FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB, FOUNDATION_STAGE_ENCODING_FLOAT16 },
        "builtin_linearize" },
    };

    const builtin_edge_t *
    find_edge(const io_t &from, const io_t &to) {
      for (const auto &edge: kBuiltinEdges) {
        if (edge.from == from && edge.to == to) {
          return &edge;
        }
      }
      return nullptr;
    }

    /// Bridge cursor → next at one boundary: direct match passes through, a
    /// known builtin edge is appended to the plan, anything else is an R2
    /// rejection naming the missing conversion.
    void
    bridge(io_t &cursor, const io_t &next, chain_plan_t &plan) {
      if (cursor == next) {
        return;
      }
      if (const auto *edge = find_edge(cursor, next)) {
        plan.entries.push_back({ edge->id, true });
        cursor = next;
        return;
      }
      plan.errors.push_back(
        "R2_conversion_unavailable: " + io_label(cursor) + " -> " + io_label(next) +
        " has no builtin conversion; insert a compatible stage or reorder the chain");
    }
  }  // namespace

  stage_declaration_t
  synthesize_v1_declaration(std::string name) {
    stage_declaration_t declaration;
    declaration.name = std::move(name);
    declaration.abi_version = 1;
    declaration.input_domain = FOUNDATION_STAGE_DOMAIN_SDR_REC709;
    declaration.input_encoding = FOUNDATION_STAGE_ENCODING_UNORM8;
    declaration.output_domain = FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB;
    declaration.output_encoding = FOUNDATION_STAGE_ENCODING_FLOAT16;
    declaration.resolution_behavior = FOUNDATION_STAGE_RESOLUTION_SAME;
    declaration.temporal = false;
    declaration.max_frames_out = 1;
    return declaration;
  }

  chain_plan_t
  validate_chain(
    const std::vector<stage_declaration_t> &stages,
    std::uint32_t capture_domain,
    std::uint32_t capture_encoding,
    std::uint32_t encoder_domain,
    std::uint32_t encoder_encoding,
    const chain_limits_t &limits) {
    chain_plan_t plan;
    io_t cursor { capture_domain, capture_encoding };
    const io_t encoder { encoder_domain, encoder_encoding };
    std::uint32_t temporal_count = 0;
    std::size_t accepted = 0;
    bool any_hdr_output = false;

    for (std::size_t i = 0; i < stages.size(); ++i) {
      const auto &stage = stages[i];

      // R1: ABI version must be one this host drives (v2 native or v1
      // legacy via caps synthesis). Rejected stages are excluded from the
      // plan; the rest of the chain still validates.
      if (stage.abi_version != FOUNDATION_STAGE_ABI_VERSION && stage.abi_version != 1) {
        plan.errors.push_back("R1_abi_unsupported: " + stage.name + " declares ABI " + std::to_string(stage.abi_version));
        continue;
      }
      if (stage.max_frames_out == 0) {
        plan.errors.push_back("R1_invalid_caps: " + stage.name + " declares max_frames_out = 0");
        continue;
      }

      bridge(cursor, { stage.input_domain, stage.input_encoding }, plan);
      plan.entries.push_back({ stage.name, false });
      cursor = { stage.output_domain, stage.output_encoding };
      ++accepted;
      any_hdr_output |= stage.output_domain != FOUNDATION_STAGE_DOMAIN_SDR_REC709;

      // R3: resolution bounds. ARBITRARY must be negotiable with the encoder
      // adapter, which today means it must be the final stage.
      if (stage.resolution_behavior == FOUNDATION_STAGE_RESOLUTION_ARBITRARY &&
          i + 1 != stages.size()) {
        plan.errors.push_back("R3_arbitrary_not_last: " + stage.name + " must be the final stage before the encoder adapter");
      }
      if (stage.resolution_behavior == FOUNDATION_STAGE_RESOLUTION_SCALE &&
          stage.max_scale < stage.min_scale) {
        plan.errors.push_back("R3_invalid_scale_range: " + stage.name + " declares max_scale < min_scale");
      }
      plan.resolution_scale *= std::max(1.0f, stage.max_scale);
      if (plan.resolution_scale > limits.max_resolution_scale + 1e-3f) {
        plan.errors.push_back(
          "R3_scale_exceeds_encoder: chain scale x" + std::to_string(plan.resolution_scale) +
          " exceeds the encoder bound x" + std::to_string(limits.max_resolution_scale));
      }

      // R4: temporal bounds.
      if (stage.temporal) {
        ++temporal_count;
        plan.temporal = true;
        plan.frame_multiplier *= stage.max_frames_out;
      }
    }

    bridge(cursor, encoder, plan);

    if (temporal_count > 1) {
      plan.warnings.push_back("R4_multiple_temporal: " + std::to_string(temporal_count) + " temporal stages multiply frame count and latency; quality is not guaranteed");
    }
    if (plan.frame_multiplier > limits.max_frame_multiplier) {
      plan.errors.push_back(
        "R4_frame_multiplier_exceeds_encoder: x" + std::to_string(plan.frame_multiplier) +
        " exceeds the bound x" + std::to_string(limits.max_frame_multiplier));
    }

    // R5: everything after the first temporal stage runs at multiplied rate.
    if (temporal_count >= 1) {
      plan.warnings.push_back("R5_temporal_cost: stages and the encoder after the first temporal stage run at x" + std::to_string(plan.frame_multiplier) + " frame rate");
    }

    // R6: an HDR wire whose every stage output stays SDR is SDR-in-HDR —
    // the auto-inserted linearize pass feeds the HDR container, but nothing
    // synthesizes HDR. Allowed, said out loud. (The HDR encoder leg consumes
    // FP16 scRGB; PQ/HLG-domain legs cover stages doing their own tonemap.)
    const bool hdr_wire = encoder.domain == FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB ||
                          encoder.domain == FOUNDATION_STAGE_DOMAIN_PQ_BT2020 ||
                          encoder.domain == FOUNDATION_STAGE_DOMAIN_HLG_BT2020;
    if (accepted > 0 && hdr_wire && !any_hdr_output) {
      plan.warnings.push_back("R6_sdr_in_hdr_container: chain output stays SDR inside an HDR wire contract");
    }

    plan.ok = plan.errors.empty();
    return plan;
  }
}  // namespace platf::dxgi::postprocess
