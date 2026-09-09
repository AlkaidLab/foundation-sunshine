# linux specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="linux")

# AppImage
if(${SUNSHINE_BUILD_APPIMAGE})
    # use relative assets path for AppImage
    string(REPLACE "${CMAKE_INSTALL_PREFIX}" ".${CMAKE_INSTALL_PREFIX}" SUNSHINE_ASSETS_DIR_DEF ${SUNSHINE_ASSETS_DIR})
endif()

# cuda
set(CUDA_FOUND OFF)
if(${SUNSHINE_ENABLE_CUDA})
    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        set(CUDA_FOUND ON)
        enable_language(CUDA)

        message(STATUS "CUDA Compiler Version: ${CMAKE_CUDA_COMPILER_VERSION}")
        set(CMAKE_CUDA_ARCHITECTURES "")

        # https://tech.amikelive.com/node-930/cuda-compatibility-of-nvidia-display-gpu-drivers/
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 6.5)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 10)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 6.5)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 50 52)
        endif()

        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 7.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 11)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER 7.6)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 60 61 62)
        endif()

        # https://docs.nvidia.com/cuda/archive/9.2/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 9.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 20)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 9.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 70)
        endif()

        # https://docs.nvidia.com/cuda/archive/10.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 10.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 72 75)
        endif()

        # https://docs.nvidia.com/cuda/archive/11.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 11.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 30)
        elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 11.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 80)
        endif()

        # https://docs.nvidia.com/cuda/archive/11.8.0/cuda-compiler-driver-nvcc/index.html
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 11.8)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 86 87 89 90)
        endif()

        if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.0)
            list(APPEND CMAKE_CUDA_ARCHITECTURES 35)
        endif()

        # sort the architectures
        list(SORT CMAKE_CUDA_ARCHITECTURES COMPARE NATURAL)

        # message(STATUS "CUDA NVCC Flags: ${CUDA_NVCC_FLAGS}")
        message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    endif()
endif()
if(CUDA_FOUND)
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvfbc")
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cu"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/cuda.cpp"
            "${CMAKE_SOURCE_DIR}/third-party/nvfbc/NvFBC.h")

    add_compile_definitions(SUNSHINE_BUILD_CUDA)
endif()

# NVENC SDK headers: the encoder option tables in video.cpp consume their
# enums on every platform, though only Windows links the SDK itself. Pick the
# newest pinned API line that is present.
foreach(_nvenc_sdk IN ITEMS 1301 1300 1200 1100)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers/${_nvenc_sdk}/include/ffnvcodec/nvEncodeAPI.h")
        include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers/${_nvenc_sdk}/include")
        break()
    endif()
endforeach()

# drm
if(${SUNSHINE_ENABLE_DRM})
    find_package(LIBDRM)
    find_package(LIBCAP)
else()
    set(LIBDRM_FOUND OFF)
    set(LIBCAP_FOUND OFF)
endif()
if(LIBDRM_FOUND AND LIBCAP_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_DRM)
    include_directories(SYSTEM ${LIBDRM_INCLUDE_DIRS} ${LIBCAP_INCLUDE_DIRS})
    list(APPEND PLATFORM_LIBRARIES ${LIBDRM_LIBRARIES} ${LIBCAP_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/kmsgrab.cpp")
    list(APPEND SUNSHINE_DEFINITIONS EGL_NO_X11=1)
elseif(NOT LIBDRM_FOUND)
    message(WARNING "Missing libdrm")
elseif(NOT LIBDRM_FOUND)
    message(WARNING "Missing libcap")
endif()

# evdev
include(dependencies/libevdev_Sunshine)

# vaapi
if(${SUNSHINE_ENABLE_VAAPI})
    find_package(Libva)
else()
    set(LIBVA_FOUND OFF)
endif()
if(LIBVA_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_VAAPI)
    include_directories(SYSTEM ${LIBVA_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${LIBVA_LIBRARIES} ${LIBVA_DRM_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/vaapi.cpp")
endif()

# wayland
if(${SUNSHINE_ENABLE_WAYLAND})
    find_package(Wayland)
else()
    set(WAYLAND_FOUND OFF)
endif()
if(WAYLAND_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_WAYLAND)

    if(NOT SUNSHINE_SYSTEM_WAYLAND_PROTOCOLS)
        set(WAYLAND_PROTOCOLS_DIR "${CMAKE_SOURCE_DIR}/third-party/wayland-protocols")
    else()
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols REQUIRED)
    endif()

    GEN_WAYLAND("${WAYLAND_PROTOCOLS_DIR}" "unstable/xdg-output" xdg-output-unstable-v1)
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/wlr-protocols" "unstable" wlr-export-dmabuf-unstable-v1)

    include_directories(
            SYSTEM
            ${WAYLAND_INCLUDE_DIRS}
            ${CMAKE_BINARY_DIR}/generated-src
    )

    list(APPEND PLATFORM_LIBRARIES ${WAYLAND_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wlgrab.cpp"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/wayland.cpp")
endif()

# x11
if(${SUNSHINE_ENABLE_X11})
    find_package(X11)
else()
    set(X11_FOUND OFF)
endif()
if(X11_FOUND)
    add_compile_definitions(SUNSHINE_BUILD_X11)
    include_directories(SYSTEM ${X11_INCLUDE_DIR})
    list(APPEND PLATFORM_LIBRARIES ${X11_LIBRARIES})
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.h"
            "${CMAKE_SOURCE_DIR}/src/platform/linux/x11grab.cpp")
endif()

if(NOT ${CUDA_FOUND}
        AND NOT ${WAYLAND_FOUND}
        AND NOT ${X11_FOUND}
        AND NOT (${LIBDRM_FOUND} AND ${LIBCAP_FOUND})
        AND NOT ${LIBVA_FOUND})
    message(FATAL_ERROR "Couldn't find either cuda, wayland, x11, (libdrm and libcap), or libva")
endif()

# tray icon
# The pinned third-party/tray implements Linux via Qt + libnotify
# (src/tray_linux.cpp); link it as its tray::tray target like upstream does.
if(${SUNSHINE_ENABLE_TRAY})
    pkg_check_modules(LIBNOTIFY libnotify)
    if(NOT LIBNOTIFY_FOUND)
        set(SUNSHINE_TRAY 0)
        message(WARNING "Missing libnotify, disabling tray icon")
        message(STATUS "LIBNOTIFY_FOUND: ${LIBNOTIFY_FOUND}")
    else()
        include_directories(SYSTEM ${LIBNOTIFY_INCLUDE_DIRS})
        link_directories(${LIBNOTIFY_LIBRARY_DIRS})

        add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/tray")
        list(APPEND SUNSHINE_EXTERNAL_LIBRARIES tray::tray)
    endif()
else()
    set(SUNSHINE_TRAY 0)
    message(STATUS "Tray icon disabled")
endif()

if(${SUNSHINE_ENABLE_TRAY} AND ${SUNSHINE_TRAY} EQUAL 0 AND SUNSHINE_REQUIRE_TRAY)
    message(FATAL_ERROR "Tray icon is required")
endif()

if(${SUNSHINE_USE_LEGACY_INPUT})  # TODO: Remove this legacy option after the next stable release
    list(APPEND PLATFORM_TARGET_FILES "${CMAKE_SOURCE_DIR}/src/platform/linux/input/legacy_input.cpp")
else()
    # These need to be set before adding the inputtino subdirectory in order for them to be picked up
    set(LIBEVDEV_CUSTOM_INCLUDE_DIR "${EVDEV_INCLUDE_DIR}")
    set(LIBEVDEV_CUSTOM_LIBRARY "${EVDEV_LIBRARY}")

    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/inputtino")
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES inputtino::libinputtino)
    file(GLOB_RECURSE INPUTTINO_SOURCES
            ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.h
            ${CMAKE_SOURCE_DIR}/src/platform/linux/input/inputtino*.cpp)
    list(APPEND PLATFORM_TARGET_FILES ${INPUTTINO_SOURCES})

    # build libevdev before the libinputtino target
    if(EXTERNAL_PROJECT_LIBEVDEV_USED)
        add_dependencies(libinputtino libevdev)
    endif()
endif()

list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/linux/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/graphics.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/display_device.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/egl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/src/gl.c"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/EGL/eglplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/KHR/khrplatform.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/gl.h"
        "${CMAKE_SOURCE_DIR}/third-party/glad/include/glad/egl.h")

list(APPEND PLATFORM_LIBRARIES
        dl
        pulse
        pulse-simple)

include_directories(
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party/glad/include")
