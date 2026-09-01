#include "src/platform/windows/rtx_hdr/backend_abi.h"

namespace {
  foundation_truehdr_status_e FOUNDATION_TRUEHDR_CALL
  create_backend(void *, const foundation_truehdr_config_t *, void **instance) {
    if (!instance) {
      return FOUNDATION_TRUEHDR_STATUS_INVALID_ARGUMENT;
    }
    *instance = reinterpret_cast<void *>(1);
    return FOUNDATION_TRUEHDR_STATUS_OK;
  }

  foundation_truehdr_status_e FOUNDATION_TRUEHDR_CALL
  process_frame(void *, void *, void *, void *) {
#ifdef FAKE_TRUEHDR_PROCESS_FAILS
    return FOUNDATION_TRUEHDR_STATUS_INTERNAL_ERROR;
#else
    return FOUNDATION_TRUEHDR_STATUS_OK;
#endif
  }

  void FOUNDATION_TRUEHDR_CALL
  flush_backend(void *) {}

  void FOUNDATION_TRUEHDR_CALL
  destroy_backend(void *) {}
}

extern "C" __declspec(dllexport) const foundation_truehdr_api_t *FOUNDATION_TRUEHDR_CALL
foundation_truehdr_get_api(uint32_t) {
  static const foundation_truehdr_api_t api {
#ifdef FAKE_TRUEHDR_BAD_ABI
    FOUNDATION_TRUEHDR_ABI_VERSION + 1,
#else
    FOUNDATION_TRUEHDR_ABI_VERSION,
#endif
    sizeof(foundation_truehdr_api_t),
    create_backend,
    process_frame,
    flush_backend,
    destroy_backend,
  };
  return &api;
}
