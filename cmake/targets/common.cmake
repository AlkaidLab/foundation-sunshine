# common target definitions
# this file will also load platform specific macros

add_executable(sunshine "${CMAKE_SOURCE_DIR}/src/main.cpp")
# cmake-add-custom-command-20261120, DEPENDS: regenerate when a log statement moves.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
file(GLOB_RECURSE _log_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h" "${CMAKE_SOURCE_DIR}/src/*.mm" "${CMAKE_SOURCE_DIR}/src/*.hpp")
set(_log_catalogue "${CMAKE_SOURCE_DIR}/scripts/telemetry/error_code_catalogue.py")
set(_log_templates "${CMAKE_CURRENT_BINARY_DIR}/error_templates.generated.inc")
add_custom_command(OUTPUT "${_log_templates}"
    COMMAND "${Python3_EXECUTABLE}" "${_log_catalogue}" "${CMAKE_SOURCE_DIR}" --templates "${_log_templates}"
    DEPENDS "${_log_catalogue}" "${CMAKE_CURRENT_LIST_FILE}" ${_log_sources}
    VERBATIM)
add_library(sunshine_log_templates STATIC src/telemetry/log_template.cpp "${_log_templates}")
target_include_directories(sunshine_log_templates PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(sunshine_log_templates PRIVATE cxx_std_20)
target_link_libraries(sunshine PRIVATE sunshine_log_templates)

include(${CMAKE_MODULE_PATH}/dependencies/mat_capture.cmake)
if (TARGET sunshine_mat_capture)
    add_dependencies(sunshine sunshine_mat_capture)
endif ()
# Host sources are compiled once and shared by `sunshine` and `test_sunshine` (Windows builds with tests). Before,
# the aggregate test executable compiled the same ~250 files a second time, with a different command line.
# Only files whose code depends on definitions that the test executable adds are compiled in both targets:
#   SUNSHINE_TESTS       test seams (extra functions behind #ifdef SUNSHINE_TESTS)
#   SUNSHINE_SHADERS_DIR the shader directory of the source tree, which tests read without an install
# The other uses of SUNSHINE_TESTS are declarations in headers (input.h, logging_severity.h, nvhttp.h,
# nvhttp/pairing_pending.h, webhook/webhook.h): they add names, never members or layouts, so a translation unit that
# compiles without them is ODR-safe next to one that compiles with them.
# A coverage build keeps the separate test compilation: the shared objects are not built with the coverage flags of
# sunshine_coverage, and linking the test target against it cannot instrument objects that were already compiled.
option(SUNSHINE_SHARE_HOST_OBJECTS "Compile the host sources once for sunshine and test_sunshine" ON)
set(SUNSHINE_SHARED_HOST_OBJECTS_ENABLED OFF)
if(WIN32 AND SUNSHINE_SHARE_HOST_OBJECTS AND (BUILD_TESTS OR BUILD_TRAY_TESTS) AND NOT SUNSHINE_TESTS_ENABLE_COVERAGE)
    set(SUNSHINE_SHARED_HOST_OBJECTS_ENABLED ON)
    set(_sunshine_seam_regex
        ".*/src/(input|logging|nvhttp/pairing|nvhttp/pairing_pending|webhook/webhook|nvenc/win/nvenc_dynamic_factory|nvenc/win/impl/nvenc_dynamic_factory_1301|platform/windows/display_vram|platform/windows/pre_encode_filter)\\.cpp$")
    set(_sunshine_host_all ${SUNSHINE_HOST_SOURCES} ${PLATFORM_TARGET_FILES})
    set(SUNSHINE_SEAM_SOURCES ${_sunshine_host_all})
    list(FILTER SUNSHINE_SEAM_SOURCES INCLUDE REGEX "${_sunshine_seam_regex}")
    set(SUNSHINE_SHARED_SOURCES ${_sunshine_host_all})
    list(FILTER SUNSHINE_SHARED_SOURCES EXCLUDE REGEX "${_sunshine_seam_regex}")
    target_sources(sunshine PRIVATE ${SUNSHINE_SEAM_SOURCES})
else()
    target_sources(sunshine PRIVATE ${SUNSHINE_HOST_SOURCES} ${PLATFORM_TARGET_FILES})
endif()
if(WIN32)
    target_link_libraries(sunshine PRIVATE Pyrowave::Runtime sunshine_windows_resources winhttp windowscodecs ole32 shell32)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/LICENSE"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.pyrowave" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/NOTICE.md"
            DESTINATION "tools/pyrowave" RENAME "NOTICE.pyrowave.md" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/LICENSES/GPL-3.0-only.txt"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.pyrowave-GPL-3.0-only.txt" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/third_party/Granite/LICENSE"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.granite" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/third_party/Granite/third_party/volk/LICENSE.md"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.volk" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/third_party/Granite/third_party/khronos/vulkan-headers/LICENSE.md"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.vulkan-headers" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/third_party/Granite/third_party/khronos/vulkan-headers/LICENSES/MIT.txt"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.vulkan-headers-MIT.txt" COMPONENT application)
    install(FILES "${CMAKE_SOURCE_DIR}/third-party/pyrowave/third_party/Granite/third_party/khronos/vulkan-headers/LICENSES/Apache-2.0.txt"
            DESTINATION "tools/pyrowave" RENAME "LICENSE.vulkan-headers-Apache-2.0.txt" COMPONENT application)
endif()
include(${CMAKE_MODULE_PATH}/dependencies/rtx_video_adapter.cmake)
include(${CMAKE_MODULE_PATH}/dependencies/dlssnr_adapter.cmake)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

# todo - is this necessary? ... for anything except linux?
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

target_link_libraries(sunshine PRIVATE
        ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS}
        sunshine_project_options sunshine_crypto sunshine_ai_provider sunshine_ai_stream
        alkaidlab_ds5_core alkaidlab_usbip_core alkaidlab_stun_codec)
# The adapter definitions and trust-header directories are per source file (see the end of this file), not per target:
# only three files read them, and a target-wide definition would give every translation unit a different command line
# whenever the adapters are on or off, so a compiler cache filled by one kind of build could not serve the other.
if (TARGET sunshine_rtx_video_adapter)
    add_dependencies(sunshine sunshine_rtx_video_adapter)
endif ()
if (TARGET sunshine_dlssnr_adapter)
    add_dependencies(sunshine sunshine_dlssnr_adapter)
endif ()
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})
set_target_properties(sunshine PROPERTIES CXX_STANDARD 23
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301

if(SUNSHINE_SHARED_HOST_OBJECTS_ENABLED)
    # Same compile configuration as `sunshine`: definitions, options, standard and the usage requirements of every
    # library it links. The objects are linked into sunshine and test_sunshine through $<TARGET_OBJECTS:...>.
    add_library(sunshine_host_objects OBJECT ${SUNSHINE_SHARED_SOURCES})
    target_link_libraries(sunshine_host_objects PRIVATE
            Pyrowave::Runtime sunshine_windows_resources winhttp windowscodecs ole32 shell32
            ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS}
            sunshine_project_options sunshine_crypto sunshine_ai_provider sunshine_ai_stream
            alkaidlab_ds5_core alkaidlab_usbip_core alkaidlab_stun_codec)
    foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
        add_dependencies(sunshine_host_objects ${dep})
    endforeach()
    if (TARGET sunshine_rtx_video_adapter)
        add_dependencies(sunshine_host_objects sunshine_rtx_video_adapter)
    endif ()
    if (TARGET sunshine_dlssnr_adapter)
        add_dependencies(sunshine_host_objects sunshine_dlssnr_adapter)
    endif ()
    target_compile_definitions(sunshine_host_objects PUBLIC ${SUNSHINE_DEFINITIONS})
    set_target_properties(sunshine_host_objects PROPERTIES CXX_STANDARD 23)
    target_compile_options(sunshine_host_objects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    target_sources(sunshine PRIVATE $<TARGET_OBJECTS:sunshine_host_objects>)
endif()

# Homebrew build fails the vite build if we set these environment variables
if(${SUNSHINE_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${SUNSHINE_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

include(${CMAKE_MODULE_PATH}/targets/web_ui.cmake)

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

# tests
if(BUILD_TESTS OR BUILD_TRAY_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS AND NOT BUILD_TRAY_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    string(APPEND VIGEM_COMPILE_FLAGS "-Wno-dynamic-class-memaccess ")
else()
    string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
endif()
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# Adapter definitions: set only on the files that read them, and only when they are compiled for the targets that load the
# adapters (the host, its shared object library and the aggregate test executable). A source property applies to every
# target that compiles the file, and other test executables compile the adapter loaders against fake adapters
# (pre_encode_filter_unit_tests): with the real trust header there the fake adapter is untrusted and its tests fail,
# which only shows when the real adapter is built (builds with the private SDK).
# SUNSHINE_SIMULATE_ADAPTER_DEFINES builds the same definitions without the NVIDIA SDK, with placeholder digests, so a
# build without secrets (fork PRs) still compiles and tests the configuration of a master build. Test builds only.
option(SUNSHINE_SIMULATE_ADAPTER_DEFINES "Test only: define the adapter build with placeholder digests, without the SDK" OFF)
set(_alk_rtx_adapter_on OFF)
set(_alk_nr_adapter_on OFF)
if (TARGET sunshine_rtx_video_adapter)
    set(_alk_rtx_adapter_on ON)
endif ()
if (TARGET sunshine_dlssnr_adapter)
    set(_alk_nr_adapter_on ON)
endif ()
if (SUNSHINE_SIMULATE_ADAPTER_DEFINES)
    if (NOT WIN32 OR NOT (BUILD_TESTS OR BUILD_TRAY_TESTS))
        message(FATAL_ERROR "SUNSHINE_SIMULATE_ADAPTER_DEFINES is for Windows test builds")
    endif ()
    string(REPEAT "0" 64 _alk_zero_digest)
    # Only the adapters that are not really built are simulated; a real one keeps its own trust header.
    if (NOT _alk_rtx_adapter_on)
        set(RTX_VIDEO_TRUST_INCLUDE "${CMAKE_BINARY_DIR}/generated/simulated_rtx_video")
        file(WRITE "${RTX_VIDEO_TRUST_INCLUDE}/rtx_video_trust.h"
            "#pragma once\n#define SUNSHINE_RTX_VIDEO_ADAPTER_SHA256 \"${_alk_zero_digest}\"\n#define SUNSHINE_RTX_VIDEO_RUNTIME_SHA256 \"${_alk_zero_digest}\"\n")
        set(_alk_rtx_adapter_on ON)
    endif ()
    if (NOT _alk_nr_adapter_on)
        set(DLSSNR_TRUST_INCLUDE "${CMAKE_BINARY_DIR}/generated/simulated_dlssnr")
        file(WRITE "${DLSSNR_TRUST_INCLUDE}/dlssnr_trust.h"
            "#pragma once\n#define SUNSHINE_DLSSNR_ADAPTER_SHA256 \"${_alk_zero_digest}\"\n")
        set(_alk_nr_adapter_on ON)
    endif ()
endif ()

set(_alk_src "${CMAKE_SOURCE_DIR}/src")
set(_alk_adapter_targets "$<OR:$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunshine>,$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunshine_host_objects>,$<STREQUAL:$<TARGET_PROPERTY:NAME>,test_sunshine>>")
if (_alk_rtx_adapter_on)
    foreach(_f "${_alk_src}/image_enhancement/config.cpp" "${_alk_src}/platform/windows/image_enhancement/rtx_hdr/adapter_loader.cpp")
        set_property(SOURCE "${_f}" DIRECTORY "${CMAKE_SOURCE_DIR}" ${TEST_DIR} APPEND PROPERTY COMPILE_DEFINITIONS "$<${_alk_adapter_targets}:SUNSHINE_RTX_VIDEO_ADAPTER>")
        set_property(SOURCE "${_f}" DIRECTORY "${CMAKE_SOURCE_DIR}" ${TEST_DIR} APPEND PROPERTY INCLUDE_DIRECTORIES "$<${_alk_adapter_targets}:${RTX_VIDEO_TRUST_INCLUDE}>")
    endforeach()
endif ()
if (_alk_nr_adapter_on)
    foreach(_f "${_alk_src}/image_enhancement/config.cpp" "${_alk_src}/platform/windows/image_enhancement/dlss_nr/adapter_loader.cpp")
        set_property(SOURCE "${_f}" DIRECTORY "${CMAKE_SOURCE_DIR}" ${TEST_DIR} APPEND PROPERTY COMPILE_DEFINITIONS "$<${_alk_adapter_targets}:SUNSHINE_DLSSNR_ADAPTER>")
        set_property(SOURCE "${_f}" DIRECTORY "${CMAKE_SOURCE_DIR}" ${TEST_DIR} APPEND PROPERTY INCLUDE_DIRECTORIES "$<${_alk_adapter_targets}:${DLSSNR_TRUST_INCLUDE}>")
    endforeach()
endif ()

sunshine_configure_debug_sources("${TEST_DIR}")
sunshine_configure_fec_sources("${TEST_DIR}")
