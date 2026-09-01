/**
 * @file src/platform/frame_contract.cpp
 * @brief Frame-pipeline contract policy and validation.
 */

#include "frame_contract.h"

namespace platf {
  frame_pipeline_policy_t
  resolve_frame_pipeline_policy(int dynamic_range, bool post_process_hdr_active) {
    frame_pipeline_policy_t policy;

    if (dynamic_range == 2) {
      policy.output = {
        .transfer = wire_transfer_e::hlg,
        .require_10bit = true,
        .require_bt2020 = true,
      };
    }
    else if (dynamic_range > 0) {
      // Preserve the legacy pipeline's boolean handling for future or unknown
      // positive wire values: only 2 is special-cased as HLG; all others use PQ.
      policy.output = {
        .transfer = wire_transfer_e::pq,
        .require_10bit = true,
        .require_bt2020 = true,
      };
    }
    else {
      policy.output = {
        .transfer = wire_transfer_e::sdr,
        .require_10bit = false,
        .require_bt2020 = false,
      };
      post_process_hdr_active = false;
    }

    if (post_process_hdr_active) {
      policy.source_display = source_display_intent_e::require_sdr;
      policy.capture = {
        .required_domain = frame_domain_e::sdr_rec709,
        .preferred_encoding = pixel_encoding_class_e::unorm8,
        .require_private_handoff = true,
      };
    }
    else if (policy.output.transfer != wire_transfer_e::sdr) {
      policy.source_display = source_display_intent_e::require_hdr;
      policy.capture = {
        .required_domain = frame_domain_e::linear_scrgb,
        .preferred_encoding = pixel_encoding_class_e::float16,
        .require_private_handoff = false,
      };
    }
    else {
      policy.source_display = source_display_intent_e::unchanged;
      policy.capture = {
        .required_domain = frame_domain_e::sdr_rec709,
        .preferred_encoding = pixel_encoding_class_e::unorm8,
        .require_private_handoff = false,
      };
    }

    return policy;
  }

  bool
  postprocess_produces_hdr_output(
    const frame_pipeline_policy_t &policy,
    pre_encode_filter_e filter) {
    return filter != pre_encode_filter_e::none &&
           policy.source_display == source_display_intent_e::require_sdr &&
           policy.output.transfer != wire_transfer_e::sdr;
  }

  bool
  frame_satisfies_capture_contract(
    const capture_contract_t &required,
    const captured_frame_desc_t &actual) {
    if (actual.domain == frame_domain_e::unknown) {
      return false;
    }
    if (required.required_domain != frame_domain_e::unknown &&
        required.required_domain != actual.domain) {
      return false;
    }
    if (required.preferred_encoding != pixel_encoding_class_e::automatic &&
        required.preferred_encoding != actual.encoding) {
      return false;
    }
    if (required.require_private_handoff && actual.borrowed) {
      return false;
    }
    return true;
  }
}  // namespace platf
