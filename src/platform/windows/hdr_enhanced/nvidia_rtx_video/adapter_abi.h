/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_abi.h
 * @brief C ABI between the MinGW host and its statically linked MSVC NGX adapter.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
  #define FOUNDATION_RTX_VIDEO_CALL __cdecl
#else
  #define FOUNDATION_RTX_VIDEO_CALL
#endif

#define FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION 1u
#define FOUNDATION_TRUEHDR_ADAPTER_GET_API_EXPORT "foundation_truehdr_adapter_get_api"

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
  FOUNDATION_TRUEHDR_STATUS_DEVELOPMENT_BUILD_EXPIRED = 6,
} foundation_truehdr_status_e;

typedef struct foundation_truehdr_config_t {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  float contrast;
  float saturation;
  float middle_gray_nits;
  float peak_nits;
  const wchar_t *runtime_directory;
} foundation_truehdr_config_t;

typedef struct foundation_truehdr_adapter_api_t {
  uint32_t abi_version;
  uint32_t struct_size;

  foundation_truehdr_status_e(FOUNDATION_RTX_VIDEO_CALL *create)(
    void *d3d11_device,
    const foundation_truehdr_config_t *config,
    void **instance);

  foundation_truehdr_status_e(FOUNDATION_RTX_VIDEO_CALL *process)(
    void *instance,
    void *d3d11_device_context,
    void *sdr_input_texture,
    void *scrgb_output_texture);

  void(FOUNDATION_RTX_VIDEO_CALL *flush)(void *instance);
  void(FOUNDATION_RTX_VIDEO_CALL *destroy)(void *instance);
} foundation_truehdr_adapter_api_t;

typedef const foundation_truehdr_adapter_api_t *(FOUNDATION_RTX_VIDEO_CALL *foundation_truehdr_adapter_get_api_fn)(
  uint32_t requested_abi_version);

const foundation_truehdr_adapter_api_t *FOUNDATION_RTX_VIDEO_CALL
foundation_truehdr_adapter_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif
