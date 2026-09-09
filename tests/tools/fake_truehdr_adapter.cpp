#include "src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_abi.h"

namespace {
  foundation_truehdr_status_e FOUNDATION_RTX_VIDEO_CALL
  create_truehdr(void *, const foundation_truehdr_config_t *, void **instance) {
    if (!instance) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    *instance = reinterpret_cast<void *>(1);
    return FOUNDATION_TRUEHDR_STATUS_OK;
  }

  foundation_truehdr_status_e FOUNDATION_RTX_VIDEO_CALL
  process_frame(void *, void *, void *, void *) {
#ifdef FAKE_TRUEHDR_PROCESS_FAILS
    return FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR;
#else
    return FOUNDATION_TRUEHDR_STATUS_OK;
#endif
  }

  void FOUNDATION_RTX_VIDEO_CALL
  flush_truehdr(void *) {}

  void FOUNDATION_RTX_VIDEO_CALL
  destroy_truehdr(void *) {}
}

extern "C" __declspec(dllexport) const foundation_truehdr_adapter_api_t *FOUNDATION_RTX_VIDEO_CALL
foundation_truehdr_adapter_get_api(uint32_t) {
  static const foundation_truehdr_adapter_api_t api {
#ifdef FAKE_TRUEHDR_BAD_ABI
    FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION + 1,
#else
    FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION,
#endif
    sizeof(foundation_truehdr_adapter_api_t),
    create_truehdr,
    process_frame,
    flush_truehdr,
    destroy_truehdr,
  };
  return &api;
}
