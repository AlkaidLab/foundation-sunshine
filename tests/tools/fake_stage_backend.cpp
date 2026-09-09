#include "src/platform/windows/postprocess/stage_abi.h"

// Fake stage-ABI v2 backend for unit tests. Variants via compile defines:
//   FAKE_STAGE_BAD_ABI       - wrong abi_version (loader must reject)
//   FAKE_STAGE_PROCESS_FAILS - process returns INTERNAL_ERROR
//   FAKE_STAGE_TEMPORAL      - temporal caps with max_frames_out 2 (driver must refuse)
//   FAKE_STAGE_SCALE         - SCALE resolution 1.0..2.0
namespace {
  const foundation_stage_caps_t *
  caps_backend(void) {
    static const foundation_stage_caps_t caps {
      .struct_size = sizeof(foundation_stage_caps_t),
      .abi_version = FOUNDATION_STAGE_ABI_VERSION,
      .name = "fake.v2stage",
      .input_domain = FOUNDATION_STAGE_DOMAIN_SDR_REC709,
      .input_encoding = FOUNDATION_STAGE_ENCODING_UNORM8,
      .output_domain = FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB,
      .output_encoding = FOUNDATION_STAGE_ENCODING_FLOAT16,
      .resolution_behavior =
#ifdef FAKE_STAGE_SCALE
        FOUNDATION_STAGE_RESOLUTION_SCALE,
#else
        FOUNDATION_STAGE_RESOLUTION_SAME,
#endif
      .min_scale = 1.0f,
#ifdef FAKE_STAGE_SCALE
      .max_scale = 2.0f,
#else
      .max_scale = 1.0f,
#endif
      .temporal =
#ifdef FAKE_STAGE_TEMPORAL
        1u,
#else
        0u,
#endif
      .max_frames_out =
#ifdef FAKE_STAGE_TEMPORAL
        2u,
#else
        1u,
#endif
    };
    return &caps;
  }

  foundation_stage_status_e FOUNDATION_STAGE_CALL
  create_backend(const foundation_stage_params_t *params, void **instance) {
    if (!params || !instance || params->input_width == 0 || params->input_height == 0) {
      return FOUNDATION_STAGE_STATUS_INVALID_ARGUMENT;
    }
    *instance = reinterpret_cast<void *>(1);
    return FOUNDATION_STAGE_STATUS_OK;
  }

  foundation_stage_status_e FOUNDATION_STAGE_CALL
  process_backend(void *, const foundation_stage_frame_t *input, foundation_stage_frame_t *out, uint32_t *out_count) {
#ifdef FAKE_STAGE_PROCESS_FAILS
    (void) input;
    (void) out;
    (void) out_count;
    return FOUNDATION_STAGE_STATUS_INTERNAL_ERROR;
#else
    if (!input || !out || !out_count) {
      return FOUNDATION_STAGE_STATUS_INVALID_ARGUMENT;
    }
    // No GPU work: the host prefilled the output frame views; endorsing them
    // (and tagging the real frame) is all the driver requires.
    out[0].frame_type = FOUNDATION_STAGE_FRAME_REAL;
    out[0].source_generation = input->source_generation;
    *out_count = 1;
    return FOUNDATION_STAGE_STATUS_OK;
#endif
  }

  void FOUNDATION_STAGE_CALL
  reset_backend(void *) {}

  void FOUNDATION_STAGE_CALL
  flush_backend(void *) {}

  void FOUNDATION_STAGE_CALL
  destroy_backend(void *) {}
}

extern "C" __declspec(dllexport) const foundation_stage_api_t *FOUNDATION_STAGE_CALL
foundation_stage_get_api(uint32_t) {
  static const foundation_stage_api_t api {
#ifdef FAKE_STAGE_BAD_ABI
    FOUNDATION_STAGE_ABI_VERSION + 1,
#else
    FOUNDATION_STAGE_ABI_VERSION,
#endif
    sizeof(foundation_stage_api_t),
    caps_backend,
    create_backend,
    process_backend,
    reset_backend,
    flush_backend,
    destroy_backend,
  };
  return &api;
}
