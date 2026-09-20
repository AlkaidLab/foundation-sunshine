/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 *
 * @file src/platform/windows/image_enhancement/dlss_nr/adapter_abi.h
 * @brief C ABI between the MinGW host and the optional MSVC NGX adapter DLL
 *        for the same-resolution DLSS neural-rendering (SDR) filter.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
  #define FOUNDATION_DLSSNR_CALL __cdecl
  #if defined(FOUNDATION_DLSSNR_ADAPTER_EXPORTS)
    #define FOUNDATION_DLSSNR_EXPORT __declspec(dllexport)
  #else
    #define FOUNDATION_DLSSNR_EXPORT
  #endif
#else
  #define FOUNDATION_DLSSNR_CALL
  #define FOUNDATION_DLSSNR_EXPORT
#endif

#define FOUNDATION_DLSSNR_ADAPTER_ABI_VERSION 1u
#define FOUNDATION_DLSSNR_ADAPTER_GET_API_EXPORT "foundation_dlssnr_adapter_get_api"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum foundation_dlssnr_status_e {
  FOUNDATION_DLSSNR_STATUS_OK = 0,
  FOUNDATION_DLSSNR_STATUS_INVALID_ARGUMENT = 1,
  FOUNDATION_DLSSNR_STATUS_UNSUPPORTED = 2,
  FOUNDATION_DLSSNR_STATUS_RUNTIME_UNAVAILABLE = 3,
  FOUNDATION_DLSSNR_STATUS_DEVICE_LOST = 4,
  FOUNDATION_DLSSNR_STATUS_INTERNAL_ERROR = 5,
} foundation_dlssnr_status_e;

typedef enum foundation_dlssnr_motion_mode_e {
  FOUNDATION_DLSSNR_MOTION_ZERO = 0,
  FOUNDATION_DLSSNR_MOTION_OPTICAL_FLOW = 1,
} foundation_dlssnr_motion_mode_e;

typedef struct foundation_dlssnr_config_t {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  float intensity;
  float local_tone_strength;
  float local_structure_strength;
  float skin_structure_strength;
  int32_t style;
  int32_t motion_mode;
  int32_t motion_quality;
  uint8_t auto_mask;
  uint8_t ui_correction;
  const wchar_t *runtime_directory;
} foundation_dlssnr_config_t;

typedef struct foundation_dlssnr_adapter_api_t {
  uint32_t abi_version;
  uint32_t struct_size;

  foundation_dlssnr_status_e(FOUNDATION_DLSSNR_CALL *create)(
    void *d3d11_device,
    const foundation_dlssnr_config_t *config,
    void **instance);

  foundation_dlssnr_status_e(FOUNDATION_DLSSNR_CALL *process)(
    void *instance,
    void *d3d11_device_context,
    void *sdr_input_texture,
    void *sdr_output_texture);

  void(FOUNDATION_DLSSNR_CALL *flush)(void *instance);
  void(FOUNDATION_DLSSNR_CALL *destroy)(void *instance);
} foundation_dlssnr_adapter_api_t;

typedef const foundation_dlssnr_adapter_api_t *(FOUNDATION_DLSSNR_CALL *foundation_dlssnr_adapter_get_api_fn)(
  uint32_t requested_abi_version);

FOUNDATION_DLSSNR_EXPORT const foundation_dlssnr_adapter_api_t *FOUNDATION_DLSSNR_CALL
foundation_dlssnr_adapter_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif
