/**
 * @file examples/capture_plugin_nvfbc/sunshine_nvfbc_plugin.cpp
 * @brief Example NvFBC capture plugin for Sunshine (Windows).
 *
 * This is a SKELETON implementation showing how to build an NvFBC capture
 * plugin DLL for Sunshine. You need to fill in the actual NvFBC API calls
 * based on the Windows NvFBC API definitions (from NVIDIA Grid SDK or
 * reverse-engineered from keylase/nvidia-patch's nvfbcwrp).
 *
 * Build: Compile as a DLL named "sunshine_nvfbc.dll" and place in
 *        Sunshine's "plugins/" directory.
 *
 * Usage: Set capture = nvfbc in sunshine.conf
 *
 * Prerequisites:
 *   - NVIDIA GPU with driver patched for NvFBC (keylase/nvidia-patch)
 *   - NvFBC64.dll present in system (installed with NVIDIA driver)
 */

#include <cstring>
#include <vector>
#include <Windows.h>

// Include the Sunshine capture plugin API
#include "src/platform/windows/capture_plugin/capture_plugin_api.h"

// ============================================================================
// Windows NvFBC API definitions (reverse-engineered / from Grid SDK)
// These must match the actual driver's NvFBC interface.
// Refer to: https://github.com/keylase/nvidia-patch/blob/master/win/nvfbcwrp/
// ============================================================================

// NvFBC function pointer type
typedef void *(*NvFBCCreateInstance_t)(unsigned int magic);

// TODO: Define the actual Windows NvFBC structures here:
// - NVFBC_SESSION_HANDLE
// - NVFBC_CREATE_PARAMS
// - NVFBC_TOSYS_SETUP_PARAMS
// - NVFBC_TOSYS_GRAB_FRAME_PARAMS
// etc.

// Magic private data to bypass consumer GPU check
static const unsigned int MAGIC_PRIVATE_DATA[4] = {
  0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA
};

// ============================================================================
// Plugin session state
// ============================================================================

struct nvfbc_session {
  HMODULE nvfbc_dll;                  // NvFBC64.dll handle
  NvFBCCreateInstance_t create_fn;    // NvFBCCreateInstance function
  void *fbc_handle;                   // NvFBC session handle

  int width;
  int height;
  int framerate;

  // Frame buffer for ToSys capture
  std::vector<uint8_t> frame_buffer;
  bool frame_ready;
  bool interrupted;
};

// ============================================================================
// Plugin API implementation
// ============================================================================

extern "C" {

SUNSHINE_CAPTURE_EXPORT int
sunshine_capture_get_info(sunshine_capture_plugin_info_t *info) {
  if (!info) return -1;

  info->abi_version = SUNSHINE_CAPTURE_PLUGIN_ABI_VERSION;
  info->name = "nvfbc";
  info->version = "0.1.0";
  info->author = "Community";

  // Support system memory (ToSys) and CUDA (ToCuda)
  info->supported_mem_types = (1 << SUNSHINE_MEM_SYSTEM) | (1 << SUNSHINE_MEM_CUDA);

  return 0;
}

SUNSHINE_CAPTURE_EXPORT int
sunshine_capture_enum_displays(
  sunshine_mem_type_e mem_type,
  sunshine_display_info_t *displays,
  int max_displays) {
  // NvFBC captures the entire desktop, so we expose one "display"
  if (displays && max_displays > 0) {
    strncpy(displays[0].name, "NvFBC Desktop", sizeof(displays[0].name) - 1);
    displays[0].name[sizeof(displays[0].name) - 1] = '\0';
    displays[0].width = GetSystemMetrics(SM_CXSCREEN);
    displays[0].height = GetSystemMetrics(SM_CYSCREEN);
    displays[0].is_primary = 1;
  }
  return 1;
}

SUNSHINE_CAPTURE_EXPORT int
sunshine_capture_create_session(
  sunshine_mem_type_e mem_type,
  const char *display_name,
  const sunshine_video_config_t *config,
  sunshine_capture_session_t *session) {
  if (!config || !session) return -1;

  auto *s = new nvfbc_session {};
  s->width = config->width;
  s->height = config->height;
  s->framerate = config->framerate;
  s->frame_ready = false;
  s->interrupted = false;

  // Load NvFBC64.dll
  s->nvfbc_dll = LoadLibraryExA("NvFBC64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!s->nvfbc_dll) {
    delete s;
    return -1;
  }

  s->create_fn = reinterpret_cast<NvFBCCreateInstance_t>(
    GetProcAddress(s->nvfbc_dll, "NvFBCCreateInstance"));
  if (!s->create_fn) {
    FreeLibrary(s->nvfbc_dll);
    delete s;
    return -1;
  }

  // TODO: Initialize NvFBC session with MAGIC_PRIVATE_DATA
  // This requires the actual Windows NvFBC API structures.
  //
  // Pseudocode:
  //   NVFBC_CREATE_PARAMS create_params = {};
  //   create_params.privateData = MAGIC_PRIVATE_DATA;
  //   create_params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);
  //   auto status = nvFBCCreate(&create_params, &s->fbc_handle);
  //
  //   NVFBC_TOSYS_SETUP_PARAMS setup = {};
  //   setup.bufferFormat = NVFBC_BUFFER_FORMAT_BGRA;
  //   setup.ppBuffer = &s->frame_buffer_ptr;
  //   status = nvFBCToSysSetUp(s->fbc_handle, &setup);

  // Allocate frame buffer for ToSys mode
  s->frame_buffer.resize(s->width * s->height * 4);  // BGRA

  *session = reinterpret_cast<sunshine_capture_session_t>(s);
  return 0;
}

SUNSHINE_CAPTURE_EXPORT void
sunshine_capture_destroy_session(sunshine_capture_session_t session) {
  if (!session) return;

  auto *s = reinterpret_cast<nvfbc_session *>(session);

  // TODO: Destroy NvFBC session
  // nvFBCRelease(s->fbc_handle);

  if (s->nvfbc_dll) {
    FreeLibrary(s->nvfbc_dll);
  }

  delete s;
}

SUNSHINE_CAPTURE_EXPORT sunshine_capture_result_e
sunshine_capture_next_frame(
  sunshine_capture_session_t session,
  sunshine_frame_t *frame,
  int timeout_ms) {
  if (!session || !frame) return SUNSHINE_CAPTURE_ERROR;

  auto *s = reinterpret_cast<nvfbc_session *>(session);

  if (s->interrupted) {
    return SUNSHINE_CAPTURE_INTERRUPTED;
  }

  // TODO: Actual NvFBC grab frame call
  //
  // Pseudocode:
  //   NVFBC_TOSYS_GRAB_FRAME_PARAMS grab = {};
  //   grab.dwFlags = NVFBC_TOSYS_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY;
  //   grab.dwTimeoutMs = timeout_ms;
  //   auto status = nvFBCToSysGrabFrame(s->fbc_handle, &grab);
  //
  //   if (status == NVFBC_SUCCESS) {
  //     frame->data = s->frame_buffer_ptr;  // Pointer set by NvFBC
  //     frame->width = s->width;
  //     ...
  //     return SUNSHINE_CAPTURE_OK;
  //   }

  // Placeholder: return timeout
  return SUNSHINE_CAPTURE_TIMEOUT;
}

SUNSHINE_CAPTURE_EXPORT void
sunshine_capture_release_frame(
  sunshine_capture_session_t session,
  sunshine_frame_t *frame) {
  // NvFBC ToSys: frames are managed by NvFBC internally, no release needed
  // NvFBC ToCuda: may need to unlock CUDA resource
  (void) session;
  (void) frame;
}

SUNSHINE_CAPTURE_EXPORT int
sunshine_capture_is_hdr(sunshine_capture_session_t session) {
  // NvFBC typically does not support HDR
  (void) session;
  return 0;
}

SUNSHINE_CAPTURE_EXPORT void
sunshine_capture_interrupt(sunshine_capture_session_t session) {
  if (!session) return;

  auto *s = reinterpret_cast<nvfbc_session *>(session);
  s->interrupted = true;
}

}  // extern "C"
