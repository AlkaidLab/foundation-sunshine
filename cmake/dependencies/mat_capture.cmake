# The native audio ABI uses MSVC even when the host is built with MinGW.
# Ship first-party capture code only; Dolby components remain system prerequisites.
option(SUNSHINE_BUILD_MAT_CAPTURE "Build and bundle Windows MAT capture components (requires MSVC)" ON)
if (NOT SUNSHINE_BUILD_MAT_CAPTURE OR NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif ()
find_program(MAT_POWERSHELL powershell REQUIRED)
set(MAT_CAPTURE_BUILD "${CMAKE_BINARY_DIR}/mat-capture")
file(GLOB_RECURSE _mat_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/*.h"
    "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/mat-live/*.h"
    "${CMAKE_SOURCE_DIR}/tools/mat-capture/*.h"
    "${CMAKE_SOURCE_DIR}/tools/mat-capture/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/spatial_audio/*.h"
    "${CMAKE_SOURCE_DIR}/src/spatial_audio/*.cpp")
add_custom_command(OUTPUT "${MAT_CAPTURE_BUILD}/package.stamp"
    COMMAND "${MAT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/package.ps1"
        -Output "${MAT_CAPTURE_BUILD}" -CMake "${CMAKE_COMMAND}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${MAT_CAPTURE_BUILD}/package.stamp"
    DEPENDS ${_mat_inputs}
        "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/CMakeLists.txt"
        "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/runtime_hashes.cmake"
        "${CMAKE_SOURCE_DIR}/tools/sonic-mat-live/package.ps1"
    BYPRODUCTS "${MAT_CAPTURE_BUILD}/sunshine-plugin-mat-capture.exe"
        "${MAT_CAPTURE_BUILD}/sunshine-plugin-object-tap.dll"
        "${MAT_CAPTURE_BUILD}/sunshine-sonic-mat-worker.dll"
    VERBATIM)
add_custom_target(sunshine_mat_capture ALL DEPENDS "${MAT_CAPTURE_BUILD}/package.stamp")
