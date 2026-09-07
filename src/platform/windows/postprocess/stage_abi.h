/**
 * @file src/platform/windows/postprocess/stage_abi.h
 * @brief Stable C ABI between Sunshine and third-party post-process stage DLLs (ABI v2).
 *
 * A stage is one link of the user-configurable post-process chain
 * (docs/postprocess_chain.md). The host loads a DLL, probes
 * FOUNDATION_STAGE_GET_API_EXPORT, validates caps against the chain
 * (chain_validator), and drives per-frame processing on the capture thread.
 *
 * Versioning: hosts request FOUNDATION_STAGE_ABI_VERSION. A DLL may return
 * nullptr when it cannot serve the requested version; the host then refuses
 * the stage (rule R1). Legacy v1 TrueHDR backends exporting only
 * foundation_truehdr_get_api are admitted by host-side caps synthesis and
 * never see this header.
 *
 * Threading: all entry points are called from a single capture/encode thread,
 * serialized per stage. reset() may be called between process() batches.
 */
#pragma once

#include <stdint.h>

#if defined(_WIN32)
  #define FOUNDATION_STAGE_CALL __cdecl
#else
  #define FOUNDATION_STAGE_CALL
#endif

#define FOUNDATION_STAGE_ABI_VERSION 2u
#define FOUNDATION_STAGE_GET_API_EXPORT "foundation_stage_get_api"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundation_stage_status_e {
  FOUNDATION_STAGE_STATUS_OK = 0,
  FOUNDATION_STAGE_STATUS_INVALID_ARGUMENT = 1,
  FOUNDATION_STAGE_STATUS_UNSUPPORTED = 2,
  FOUNDATION_STAGE_STATUS_RUNTIME_UNAVAILABLE = 3,
  FOUNDATION_STAGE_STATUS_DEVICE_LOST = 4,
  FOUNDATION_STAGE_STATUS_INTERNAL_ERROR = 5,
} foundation_stage_status_e;

/* Frame domain values. These mirror platf::frame_domain_e on the host side
 * (enforced by static_assert); do not renumber. */
#define FOUNDATION_STAGE_DOMAIN_UNKNOWN 0u
#define FOUNDATION_STAGE_DOMAIN_SDR_REC709 1u
#define FOUNDATION_STAGE_DOMAIN_LINEAR_SCRGB 2u
#define FOUNDATION_STAGE_DOMAIN_PQ_BT2020 3u
#define FOUNDATION_STAGE_DOMAIN_HLG_BT2020 4u

/* Pixel encoding values, mirroring platf::pixel_encoding_class_e. */
#define FOUNDATION_STAGE_ENCODING_AUTOMATIC 0u
#define FOUNDATION_STAGE_ENCODING_UNORM8 1u
#define FOUNDATION_STAGE_ENCODING_FLOAT16 2u

/* Frame provenance inside a temporal stage's output batch. */
#define FOUNDATION_STAGE_FRAME_REAL 0u
#define FOUNDATION_STAGE_FRAME_INTERPOLATED 1u

/* Resolution behavior declared by a stage. */
#define FOUNDATION_STAGE_RESOLUTION_SAME 0u
#define FOUNDATION_STAGE_RESOLUTION_SCALE 1u
#define FOUNDATION_STAGE_RESOLUTION_ARBITRARY 2u

typedef struct foundation_stage_frame_t {
  /* Host-allocated D3D11 objects. Inputs are read-only for the stage;
   * outputs are written by the stage between process() entry and return. */
  void *texture;  /* ID3D11Texture2D* */
  void *srv;      /* ID3D11ShaderResourceView*, may be NULL on inputs */
  uint32_t dxgi_format;
  uint32_t width;
  uint32_t height;
  uint32_t domain;               /* FOUNDATION_STAGE_DOMAIN_* */
  uint32_t encoding;             /* FOUNDATION_STAGE_ENCODING_* */
  float reference_white_nits;    /* scRGB reference white when applicable */
  uint32_t frame_type;           /* FOUNDATION_STAGE_FRAME_* (outputs only) */
  uint64_t source_generation;    /* host capture generation, pass-through */
} foundation_stage_frame_t;

typedef struct foundation_stage_caps_t {
  uint32_t struct_size;
  uint32_t abi_version;          /* FOUNDATION_STAGE_ABI_VERSION */
  const char *name;              /* stable dotted id, e.g. "nvidia.vsr" */

  /* Domain contract: what the stage consumes and produces. */
  uint32_t input_domain;
  uint32_t input_encoding;
  uint32_t output_domain;
  uint32_t output_encoding;

  /* Resolution contract. SAME: out == in. SCALE: out within
   * [min_scale, max_scale] of the create-time scale_hint. ARBITRARY:
   * the stage must be last before the encoder adapter. */
  uint32_t resolution_behavior;  /* FOUNDATION_STAGE_RESOLUTION_* */
  float min_scale;
  float max_scale;

  /* Temporal contract: temporal stages keep history (reset() clears it)
   * and may emit up to max_frames_out frames per input. Non-temporal
   * stages set max_frames_out = 1 and emit exactly one frame. */
  uint32_t temporal;
  uint32_t max_frames_out;
} foundation_stage_caps_t;

typedef struct foundation_stage_params_t {
  uint32_t struct_size;
  void *d3d11_device;            /* ID3D11Device* */
  void *d3d11_device_context;    /* ID3D11DeviceContext* */
  uint32_t input_width;          /* session capture resolution */
  uint32_t input_height;
  float scale_hint;              /* 1.0 for SAME stages */
  const char *params_json;       /* per-app parameters, may be NULL */
} foundation_stage_params_t;

typedef struct foundation_stage_api_t {
  uint32_t abi_version;
  uint32_t struct_size;

  /* Static capabilities; valid for the DLL's lifetime. Never NULL. */
  const foundation_stage_caps_t *(FOUNDATION_STAGE_CALL *caps)(void);

  foundation_stage_status_e(FOUNDATION_STAGE_CALL *create)(
    const foundation_stage_params_t *params,
    void **instance);

  /* Process one input frame. `out` points to a host-allocated array of
   * caps->max_frames_out frames; the stage fills [0, *out_count) and tags
   * each with frame_type. Textures arrive sized per the create-time
   * scale_hint; width/height in each filled frame must match the texture. */
  foundation_stage_status_e(FOUNDATION_STAGE_CALL *process)(
    void *instance,
    const foundation_stage_frame_t *input,
    foundation_stage_frame_t *out,
    uint32_t *out_count);

  /* Drop temporal history (IDR / session rebuild). May be NULL when the
   * stage is not temporal. */
  void(FOUNDATION_STAGE_CALL *reset)(void *instance);

  void(FOUNDATION_STAGE_CALL *flush)(void *instance);
  void(FOUNDATION_STAGE_CALL *destroy)(void *instance);
} foundation_stage_api_t;

typedef const foundation_stage_api_t *(FOUNDATION_STAGE_CALL *foundation_stage_get_api_fn)(
  uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif
