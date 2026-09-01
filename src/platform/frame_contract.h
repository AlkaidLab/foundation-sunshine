/**
 * @file src/platform/frame_contract.h
 * @brief Vendor-neutral contracts between display preparation, capture, and video output.
 */
#pragma once

#include <cstdint>

namespace platf {
  enum class frame_domain_e : std::uint8_t {
    unknown,
    sdr_rec709,
    linear_scrgb,
    pq_bt2020,
    hlg_bt2020,
  };

  enum class pixel_encoding_class_e : std::uint8_t {
    automatic,
    unorm8,
    float16,
  };

  enum class source_display_intent_e : std::uint8_t {
    unchanged,
    require_sdr,
    require_hdr,
  };

  enum class wire_transfer_e : std::uint8_t {
    sdr,
    pq,
    hlg,
  };

  enum class pre_encode_filter_e : std::uint8_t {
    none,
    mock_sdr_to_scrgb,
    external_sdr_to_hdr,
  };

  struct pre_encode_filter_config_t {
    float contrast = 0.0f;
    float saturation = 0.0f;
    float middle_gray_nits = 50.0f;
    float peak_nits = 1000.0f;
  };

  struct capture_contract_t {
    frame_domain_e required_domain = frame_domain_e::unknown;
    pixel_encoding_class_e preferred_encoding = pixel_encoding_class_e::automatic;
    bool require_private_handoff = false;
  };

  struct captured_frame_desc_t {
    frame_domain_e domain = frame_domain_e::unknown;
    pixel_encoding_class_e encoding = pixel_encoding_class_e::automatic;
    float reference_white_nits = 0.0f;
    std::uint64_t adapter_luid = 0;
    bool borrowed = false;
    std::uint64_t source_generation = 0;
  };

  struct stream_output_contract_t {
    wire_transfer_e transfer = wire_transfer_e::sdr;
    bool require_10bit = false;
    bool require_bt2020 = false;
  };

  struct frame_pipeline_policy_t {
    stream_output_contract_t output;
    source_display_intent_e source_display = source_display_intent_e::unchanged;
    capture_contract_t capture;
  };

  /**
   * Resolve the three independent frame-pipeline contracts for a session.
   *
   * dynamic_range follows the GameStream wire values: 0 SDR, 1 PQ, 2 HLG.
   * post_process_hdr_active means an already-validated SDR-to-HDR filter will
   * generate the HDR source after capture. It must never be interpreted by a
   * capture backend as a vendor or filter selection.
   */
  frame_pipeline_policy_t
  resolve_frame_pipeline_policy(int dynamic_range, bool post_process_hdr_active);

  /**
   * Return true when the pre-encode stage, rather than the captured display,
   * is the authoritative HDR source for the wire output.
   */
  bool
  postprocess_produces_hdr_output(
    const frame_pipeline_policy_t &policy,
    pre_encode_filter_e filter);

  /** Return true when an actual captured frame can satisfy the requested contract. */
  bool
  frame_satisfies_capture_contract(
    const capture_contract_t &required,
    const captured_frame_desc_t &actual);
}  // namespace platf
