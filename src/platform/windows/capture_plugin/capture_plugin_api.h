/**
 * @file src/platform/windows/capture_plugin/capture_plugin_api.h
 * @brief Capture plugin C ABI interface for Sunshine.
 *
 * This header defines the C ABI interface that capture plugin DLLs must implement.
 * Using pure C interface ensures cross-compiler compatibility (MSVC, MinGW, Clang).
 *
 * Plugin DLLs are loaded from the "plugins/" directory next to sunshine.exe.
 * Each plugin must export the functions prefixed with "sunshine_capture_".
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Version and ABI constants
// ============================================================================

/** Current plugin ABI version. Bump when the interface changes incompatibly. */
#define SUNSHINE_CAPTURE_PLUGIN_ABI_VERSION 1

// ============================================================================
// Opaque handle types
// ============================================================================

/** Opaque handle to a capture session created by the plugin. */
typedef struct sunshine_capture_session *sunshine_capture_session_t;

// ============================================================================
// Enumerations (matching Sunshine internal types)
// ============================================================================

typedef enum {
  SUNSHINE_MEM_SYSTEM = 0,  /**< System memory (software encoding) */
  SUNSHINE_MEM_DXGI = 1,    /**< DXGI / D3D11 (hardware encoding) */
  SUNSHINE_MEM_CUDA = 2,    /**< CUDA (NVENC direct) */
} sunshine_mem_type_e;

typedef enum {
  SUNSHINE_PIX_FMT_NV12 = 0,  /**< NV12 */
  SUNSHINE_PIX_FMT_P010 = 1,  /**< P010 (10-bit) */
  SUNSHINE_PIX_FMT_AYUV = 2,  /**< AYUV (4:4:4) */
  SUNSHINE_PIX_FMT_Y410 = 3,  /**< Y410 (4:4:4 10-bit) */
} sunshine_pix_fmt_e;

typedef enum {
  SUNSHINE_CAPTURE_OK = 0,          /**< Success */
  SUNSHINE_CAPTURE_REINIT = 1,      /**< Need reinit (display mode change, etc.) */
  SUNSHINE_CAPTURE_TIMEOUT = 2,     /**< Timeout waiting for frame */
  SUNSHINE_CAPTURE_INTERRUPTED = 3, /**< Capture interrupted (stop signal) */
  SUNSHINE_CAPTURE_ERROR = -1,      /**< Fatal error */
} sunshine_capture_result_e;

// ============================================================================
// Data structures
// ============================================================================

/** Plugin information returned by sunshine_capture_get_info(). */
typedef struct {
  uint32_t abi_version;     /**< Must be SUNSHINE_CAPTURE_PLUGIN_ABI_VERSION */
  const char *name;         /**< Human-readable plugin name (e.g., "NvFBC") */
  const char *version;      /**< Plugin version string (e.g., "1.0.0") */
  const char *author;       /**< Plugin author */

  /**
   * Bitmask of supported memory types.
   * Set bit 0 for SUNSHINE_MEM_SYSTEM, bit 1 for SUNSHINE_MEM_DXGI, bit 2 for SUNSHINE_MEM_CUDA.
   */
  uint32_t supported_mem_types;
} sunshine_capture_plugin_info_t;

/** Video configuration passed to the plugin when creating a session. */
typedef struct {
  int width;           /**< Target capture width */
  int height;          /**< Target capture height */
  int framerate;       /**< Target framerate */
  int dynamic_range;   /**< 0 = SDR, 1 = HDR10 PQ, 2 = HDR HLG */
} sunshine_video_config_t;

/** Frame data returned by sunshine_capture_next_frame(). */
typedef struct {
  uint8_t *data;        /**< Pointer to frame data (system memory) or NULL for GPU frames */
  int width;            /**< Frame width */
  int height;           /**< Frame height */
  int pixel_pitch;      /**< Bytes per pixel */
  int row_pitch;        /**< Bytes per row */
  int64_t timestamp_ns; /**< Frame timestamp in nanoseconds (steady clock) */

  /**
   * For DXGI memory type: ID3D11Texture2D* handle.
   * For CUDA memory type: CUdeviceptr handle.
   * Cast to appropriate type based on memory type.
   */
  void *gpu_handle;

  /** For DXGI: subresource index within the texture. */
  uint32_t gpu_subresource;
} sunshine_frame_t;

/** Display information for enumeration. */
typedef struct {
  char name[256];       /**< Display identifier string */
  int width;            /**< Display width */
  int height;           /**< Display height */
  int is_primary;       /**< Whether this is the primary display */
} sunshine_display_info_t;

// ============================================================================
// Plugin exported functions
// ============================================================================

/**
 * @brief Get plugin information.
 * @param[out] info Filled with plugin info. name/version/author pointers must
 *                  remain valid for the lifetime of the DLL.
 * @return 0 on success, non-zero on failure.
 *
 * Plugin DLL must export as: sunshine_capture_get_info
 */
typedef int (*sunshine_capture_get_info_fn)(sunshine_capture_plugin_info_t *info);

/**
 * @brief Enumerate available displays.
 * @param mem_type Requested memory type for capture.
 * @param[out] displays Array to fill with display info.
 * @param max_displays Maximum number of entries in the displays array.
 * @return Number of displays found (may exceed max_displays to indicate truncation).
 *
 * Plugin DLL must export as: sunshine_capture_enum_displays
 */
typedef int (*sunshine_capture_enum_displays_fn)(
  sunshine_mem_type_e mem_type,
  sunshine_display_info_t *displays,
  int max_displays);

/**
 * @brief Create a capture session.
 * @param mem_type Requested memory type.
 * @param display_name Display to capture (from enum_displays, or empty for default).
 * @param config Video configuration.
 * @param[out] session Handle to the created session.
 * @return 0 on success, non-zero on failure.
 *
 * Plugin DLL must export as: sunshine_capture_create_session
 */
typedef int (*sunshine_capture_create_session_fn)(
  sunshine_mem_type_e mem_type,
  const char *display_name,
  const sunshine_video_config_t *config,
  sunshine_capture_session_t *session);

/**
 * @brief Destroy a capture session and release all resources.
 * @param session The session to destroy.
 *
 * Plugin DLL must export as: sunshine_capture_destroy_session
 */
typedef void (*sunshine_capture_destroy_session_fn)(sunshine_capture_session_t session);

/**
 * @brief Capture the next frame.
 * @param session The capture session.
 * @param[out] frame Filled with frame data on success.
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = infinite).
 * @return SUNSHINE_CAPTURE_OK on success, or an error code.
 *
 * Plugin DLL must export as: sunshine_capture_next_frame
 */
typedef sunshine_capture_result_e (*sunshine_capture_next_frame_fn)(
  sunshine_capture_session_t session,
  sunshine_frame_t *frame,
  int timeout_ms);

/**
 * @brief Release a frame after processing.
 * Must be called after each successful sunshine_capture_next_frame().
 * @param session The capture session.
 * @param frame The frame to release.
 *
 * Plugin DLL must export as: sunshine_capture_release_frame
 */
typedef void (*sunshine_capture_release_frame_fn)(
  sunshine_capture_session_t session,
  sunshine_frame_t *frame);

/**
 * @brief Check if HDR is active on the captured display.
 * @param session The capture session.
 * @return 1 if HDR, 0 if SDR.
 *
 * Plugin DLL must export as: sunshine_capture_is_hdr
 */
typedef int (*sunshine_capture_is_hdr_fn)(sunshine_capture_session_t session);

/**
 * @brief Signal the capture session to stop.
 * This is called from a different thread to interrupt a blocking next_frame() call.
 * @param session The capture session.
 *
 * Plugin DLL must export as: sunshine_capture_interrupt
 */
typedef void (*sunshine_capture_interrupt_fn)(sunshine_capture_session_t session);

// ============================================================================
// Function table (populated by the plugin loader)
// ============================================================================

/** Complete function table loaded from a plugin DLL. */
typedef struct {
  sunshine_capture_get_info_fn get_info;
  sunshine_capture_enum_displays_fn enum_displays;
  sunshine_capture_create_session_fn create_session;
  sunshine_capture_destroy_session_fn destroy_session;
  sunshine_capture_next_frame_fn next_frame;
  sunshine_capture_release_frame_fn release_frame;
  sunshine_capture_is_hdr_fn is_hdr;
  sunshine_capture_interrupt_fn interrupt;
} sunshine_capture_plugin_vtable_t;

// ============================================================================
// Convenience macros for plugin implementation
// ============================================================================

/**
 * Use these macros in your plugin .cpp to export the required functions.
 * Example:
 *   SUNSHINE_CAPTURE_EXPORT int sunshine_capture_get_info(sunshine_capture_plugin_info_t *info) { ... }
 */
#ifdef _WIN32
  #define SUNSHINE_CAPTURE_EXPORT __declspec(dllexport)
#else
  #define SUNSHINE_CAPTURE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif
