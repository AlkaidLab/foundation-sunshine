# common target definitions
# this file will also load platform specific macros

add_executable(sunshine "${CMAKE_SOURCE_DIR}/src/main.cpp")
target_sources(sunshine PRIVATE ${SUNSHINE_HOST_SOURCES} ${PLATFORM_TARGET_FILES})
if(WIN32)
    target_link_libraries(sunshine PRIVATE Pyrowave::Runtime sunshine_windows_resources)
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
        sunshine_project_options sunshine_crypto sunshine_ai_provider sunshine_ai_stream)
if (TARGET sunshine_rtx_video_adapter)
    add_dependencies(sunshine sunshine_rtx_video_adapter)
    target_include_directories(sunshine PRIVATE "${RTX_VIDEO_TRUST_INCLUDE}")
    target_compile_definitions(sunshine PRIVATE SUNSHINE_RTX_VIDEO_ADAPTER)
endif ()
if (TARGET sunshine_dlssnr_adapter)
    add_dependencies(sunshine sunshine_dlssnr_adapter)
    target_include_directories(sunshine PRIVATE "${DLSSNR_TRUST_INCLUDE}")
    target_compile_definitions(sunshine PRIVATE SUNSHINE_DLSSNR_ADAPTER)
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
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

sunshine_configure_debug_sources("${TEST_DIR}")
