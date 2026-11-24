/**
 * @file src/amf/amf_runtime_pin.h
 * @brief Keeps the AMD AMF runtime loaded for the life of the process.
 */
#pragma once

#ifdef _WIN32
  #include <mutex>

  #include <windows.h>

  #include <AMF/core/Factory.h>

namespace amf {

  /**
   * @brief Takes one reference on the AMF runtime (amfrt64.dll) that is never released.
   *
   * The encoder, DirectCapture and the video converter each load the runtime and used to unload it with their own
   * FreeLibrary(), so probing the encoders (several create-and-destroy rounds in a few seconds) loaded and unloaded the
   * runtime again and again. The runtime installs state in the graphics driver's user-mode modules that outlives one
   * component, and two of our crash reports end inside those modules (amdxx64.dll at startup, amfrtdrv64.dll while a
   * texture was released) with the runtime's own teardown just before. AMF's own helper (AMFFactoryHelper) counts the
   * users of one loaded library and frees it with the last one; the process-wide reference here makes that last
   * one the process itself. A later FreeLibrary() only drops the caller's own reference.
   * Only a successful load is kept: a failed one (the runtime is missing, or the driver is not ready yet) is tried again at the next call.
   * Source: amf-sdk-v1.5.3-factory-helper-20261116.
   */
  inline void
  pin_runtime() {
    static std::mutex mutex;
    static HMODULE pinned = nullptr;
    std::lock_guard lock { mutex };
    if (!pinned) {
      pinned = LoadLibraryW(AMF_DLL_NAME);
    }
  }

}  // namespace amf
#endif
