/**
 * @file src/platform/windows/rtx_hdr/backend_abi.h
 * @brief Stable C ABI between Sunshine and an optional MSVC TrueHDR backend.
 */
#pragma once

#include <stdint.h>

#if defined(_WIN32)
  #define FOUNDATION_TRUEHDR_CALL __cdecl
#else
  #define FOUNDATION_TRUEHDR_CALL
#endif

#define FOUNDATION_TRUEHDR_ABI_VERSION 1u
#define FOUNDATION_TRUEHDR_GET_API_EXPORT "foundation_truehdr_get_api"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundation_truehdr_status_e {
  FOUNDATION_TRUEHDR_STATUS_OK = 0,
  FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT = 1,
  FOUNDATION_TRUEHDR_STATUS_UNSUPPORTED = 2,
  FOUNDATION_TRUEHDR_STATUS_RUNTIME_UNAVAILABLE = 3,
  FOUNDATION_TRUEHDR_STATUS_DEVICE_LOST = 4,
  FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR = 5,
} foundation_truehdr_status_e;

typedef struct foundation_truehdr_config_t {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  float contrast;
  float saturation;
  float middle_gray_nits;
  float peak_nits;
} foundation_truehdr_config_t;

typedef struct foundation_truehdr_api_t {
  uint32_t abi_version;
  uint32_t struct_size;

  foundation_truehdr_status_e(FOUNDATION_TRUEHDR_CALL *create)(
    void *d3d11_device,
    const foundation_truehdr_config_t *config,
    void **instance);

  foundation_truehdr_status_e(FOUNDATION_TRUEHDR_CALL *process)(
    void *instance,
    void *d3d11_device_context,
    void *sdr_input_texture,
    void *scrgb_output_texture);

  void(FOUNDATION_TRUEHDR_CALL *flush)(void *instance);
  void(FOUNDATION_TRUEHDR_CALL *destroy)(void *instance);
} foundation_truehdr_api_t;

typedef const foundation_truehdr_api_t *(FOUNDATION_TRUEHDR_CALL *foundation_truehdr_get_api_fn)(
  uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif
