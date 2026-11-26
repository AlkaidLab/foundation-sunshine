/**
 * @file tools/alkaidlab_pyrowave_probe.cpp
 * @brief The out-of-process Vulkan probe of the PyroWave runtime.
 *
 * The host runs this tool once, when a client actually selects the PyroWave format, and reads the
 * exit code: 0 available, 2 API version, 4 default device, 5 interop, 6 unexpected failure. A
 * third-party Vulkan implicit layer that crashes or hangs ends only this process, and the host
 * stops advertising PyroWave for the rest of its lifetime.
 *
 * The tool is a headless helper: no configuration, no log, no output and no dialog, and every
 * path ends in an exit code. The codes skip 3: it is the exit code of the C runtime's abort(),
 * so an abnormal end of this process is never mistaken for one of its own answers.
 */
#define NOMINMAX
#include <windows.h>

#include <cstdint>

extern "C" {
#include <pyrowave.h>
}

int
main() {
  // Loader hard errors and fault boxes are suppressed: this helper answers with its exit code
  // only, never with a window.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

  // DLL resolution keeps to the system directories and this tool's own directory: a module that
  // is not either of those (a hijacked working directory, a user-writable path) is never loaded.
  // The Vulkan stack is unaffected: vulkan-1.dll comes from System32, and the loader opens ICDs
  // and layers by the absolute paths of their manifests.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

  try {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != PYROWAVE_API_VERSION_MAJOR ||
        minor != PYROWAVE_API_VERSION_MINOR ||
        patch != PYROWAVE_API_VERSION_PATCH) {
      return 2;
    }

    pyrowave_device device = nullptr;
    const auto create_result = pyrowave_create_default_device(&device);
    if (create_result != PYROWAVE_SUCCESS || device == nullptr) {
      if (device != nullptr) {
        pyrowave_device_destroy(device);
      }
      return 4;
    }

    const auto interop_supported = pyrowave_device_confirm_interop_support(device);
    pyrowave_device_destroy(device);
    return interop_supported ? 0 : 5;
  }
  catch (...) {
    // The C API is implemented in C++: an exception that escapes it answers "unavailable" with a
    // code of its own, so it is told apart from an abnormal end of the process.
    return 6;
  }
}
