# common compile definitions
# this file will also load platform specific definitions

list(APPEND SUNSHINE_COMPILE_OPTIONS -Wall -Wno-sign-compare)
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options

    # GCC 12 and higher will complain about maybe-uninitialized
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-maybe-uninitialized)

        # Disable the bogus warning that may prevent compilation (only for GCC 12).
        # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-restrict)
        endif()
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
endif()
if(BUILD_WERROR)
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Werror)
endif()

# setup assets directory
if(NOT SUNSHINE_ASSETS_DIR)
    set(SUNSHINE_ASSETS_DIR "assets")
endif()

# platform specific compile definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/compile_definitions/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/compile_definitions/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/compile_definitions/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/compile_definitions/linux.cmake)
    endif()
endif()

configure_file("${CMAKE_SOURCE_DIR}/src/version.h.in" version.h @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}")  # required for importing version.h

set(SUNSHINE_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Input.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Rtsp.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Session.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Video.h"
        "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/display_device.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/parsed_config.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/parsed_config.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/session.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/session.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/settings.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/settings.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/to_string.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/to_string.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/vdd_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/vdd_utils.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/vdd_ioctl.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device/vdd_ioctl.h"
        "${CMAKE_SOURCE_DIR}/src/display_device/vdd_control_ioctl.h"
        "${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        "${CMAKE_SOURCE_DIR}/src/upnp.h"
        "${CMAKE_SOURCE_DIR}/src/cbs.cpp"
        "${CMAKE_SOURCE_DIR}/src/utility.h"
        "${CMAKE_SOURCE_DIR}/src/uuid.h"
        "${CMAKE_SOURCE_DIR}/src/config.h"
        "${CMAKE_SOURCE_DIR}/src/config.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.h"
        "${CMAKE_SOURCE_DIR}/src/file_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/file_handler.h"
        "${CMAKE_SOURCE_DIR}/src/globals.cpp"
        "${CMAKE_SOURCE_DIR}/src/globals.h"
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging.h"
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/main.h"
        "${CMAKE_SOURCE_DIR}/src/crypto.cpp"
        "${CMAKE_SOURCE_DIR}/src/crypto.h"
        "${CMAKE_SOURCE_DIR}/src/webhook_format.cpp"
        "${CMAKE_SOURCE_DIR}/src/webhook_format.h"
        "${CMAKE_SOURCE_DIR}/src/webhook_httpsclient.cpp"
        "${CMAKE_SOURCE_DIR}/src/webhook_httpsclient.h"
        "${CMAKE_SOURCE_DIR}/src/webhook.cpp"
        "${CMAKE_SOURCE_DIR}/src/webhook.h"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.h"
        "${CMAKE_SOURCE_DIR}/src/abr.cpp"
        "${CMAKE_SOURCE_DIR}/src/abr.h"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.cpp"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.h"
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/confighttp.h"
        "${CMAKE_SOURCE_DIR}/src/rtsp.cpp"
        "${CMAKE_SOURCE_DIR}/src/rtsp.h"
        "${CMAKE_SOURCE_DIR}/src/stream.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream.h"
        "${CMAKE_SOURCE_DIR}/src/clipboard_bridge.cpp"
        "${CMAKE_SOURCE_DIR}/src/clipboard_bridge.h"
        "${CMAKE_SOURCE_DIR}/src/clipboard_http.cpp"
        "${CMAKE_SOURCE_DIR}/src/clipboard_http.h"
        "${CMAKE_SOURCE_DIR}/src/frame_interest.cpp"
        "${CMAKE_SOURCE_DIR}/src/frame_interest.h"
        "${CMAKE_SOURCE_DIR}/src/stream_quality.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream_quality.h"
        "${CMAKE_SOURCE_DIR}/src/stream_quality_controller.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream_quality_controller.h"
        "${CMAKE_SOURCE_DIR}/src/video.cpp"
        "${CMAKE_SOURCE_DIR}/src/video.h"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.cpp"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.h"
        "${CMAKE_SOURCE_DIR}/src/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/input.h"
        "${CMAKE_SOURCE_DIR}/src/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio.h"
        "${CMAKE_SOURCE_DIR}/src/platform/common.h"
        "${CMAKE_SOURCE_DIR}/src/process.cpp"
        "${CMAKE_SOURCE_DIR}/src/process.h"
        "${CMAKE_SOURCE_DIR}/src/network.cpp"
        "${CMAKE_SOURCE_DIR}/src/network.h"
        "${CMAKE_SOURCE_DIR}/src/move_by_copy.h"
        "${CMAKE_SOURCE_DIR}/src/system_tray.cpp"
        "${CMAKE_SOURCE_DIR}/src/system_tray.h"
        "${CMAKE_SOURCE_DIR}/src/system_tray_i18n.cpp"
        "${CMAKE_SOURCE_DIR}/src/system_tray_i18n.h"
        "${CMAKE_SOURCE_DIR}/src/task_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_safe.h"
        "${CMAKE_SOURCE_DIR}/src/sync.h"
        "${CMAKE_SOURCE_DIR}/src/round_robin.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.cpp"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.h"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.c"
        ${PLATFORM_TARGET_FILES})

if(NOT SUNSHINE_ASSETS_DIR_DEF)
    set(SUNSHINE_ASSETS_DIR_DEF "${SUNSHINE_ASSETS_DIR}")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR_DEF}")

list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY=${SUNSHINE_TRAY})
if(SUNSHINE_ENABLE_NVENC_FRAME_INTEREST_BACKEND)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_NVENC_FRAME_INTEREST_BACKEND=1)
endif()

# Publisher metadata - escape spaces for proper compilation
string(REPLACE " " "_" SUNSHINE_PUBLISHER_NAME_SAFE "${SUNSHINE_PUBLISHER_NAME}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_NAME="${SUNSHINE_PUBLISHER_NAME_SAFE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_WEBSITE="${SUNSHINE_PUBLISHER_WEBSITE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_ISSUE_URL="${SUNSHINE_PUBLISHER_ISSUE_URL}")

include_directories("${CMAKE_SOURCE_DIR}")

set(ALKAIDLAB_PLATFORM_PATH "${CMAKE_SOURCE_DIR}/../alkaidlab-platform" CACHE PATH "Path to alkaidlab-platform")
set(ALKAIDLAB_ZAKO_INPUT_PATH "${CMAKE_SOURCE_DIR}/../zako-input" CACHE PATH "Path to zako-input")

if(EXISTS "${ALKAIDLAB_PLATFORM_PATH}/core/kernel/include/alkaidlab/session_core/session_core.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/sdk/c/include/alkaidlab/session_sdk/session_sdk.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/include/alkaidlab/modules/stream_quality/adaptive_controller.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/include/alkaidlab/control_path_health/control_path_health.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/include/alkaidlab/rescue_control/rescue_control.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/include/alkaidlab/sunshine_adapter/sunshine_session_adapter.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/include/alkaidlab/sunshine_adapter/gamestream_rtsp_handshake_adapter.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/include/alkaidlab/sunshine_adapter/gamestream_enet_control_transport_adapter.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/include/alkaidlab/sunshine_adapter/rescue_wire_codec.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/include/alkaidlab/clipboard_sync/clipboard_sync.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/backends/win32/include/alkaidlab/clipboard_sync/win32_clipboard_backend.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/include/alkaidlab/microphone_uplink/microphone_uplink.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/backends/windows-wasapi-sink/include/alkaidlab/microphone_uplink/windows_wasapi_sink_backend.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/network/transport/include/alkaidlab/transport/transport_module.h" AND
   EXISTS "${ALKAIDLAB_PLATFORM_PATH}/modules/network/transport/gamestream-enet/include/alkaidlab/transport/gamestream_enet_transport.h" AND
   EXISTS "${ALKAIDLAB_ZAKO_INPUT_PATH}/include/zako/input/zako_input.h")
    list(APPEND SUNSHINE_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/alkaidlab_session_bridge.h"
            "${CMAKE_SOURCE_DIR}/src/alkaidlab_session_bridge.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/core/kernel/src/session_core.c"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/src/stream_quality_control.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/src/stream_quality_control_internal.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/src/stream_quality_adaptive_module.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/src/control_path_health.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/src/rescue_control.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/src/clipboard_sync.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/sdk/c/src/session_runtime.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/sdk/c/src/module_runtime.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/sdk/c/src/transport_runtime.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/src/microphone_uplink.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/network/transport/gamestream-enet/src/gamestream_enet_transport.cpp"
            "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/src/sunshine_session_adapter.c"
            "${ALKAIDLAB_ZAKO_INPUT_PATH}/src/zako_input.c")
    if(WIN32)
        list(APPEND SUNSHINE_TARGET_FILES
                "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/backends/win32/src/win32_clipboard_backend.cpp"
                "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/backends/windows-wasapi-sink/src/windows_wasapi_sink_backend.cpp")
    endif()
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ALKAIDLAB_PLATFORM_CORE=1)
    include_directories(
            "${ALKAIDLAB_PLATFORM_PATH}/core/kernel/include"
            "${ALKAIDLAB_PLATFORM_PATH}/sdk/c/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/adaptive-controller/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/streaming/continuity/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/network/transport/include"
            "${ALKAIDLAB_PLATFORM_PATH}/modules/network/transport/gamestream-enet/include"
            "${ALKAIDLAB_PLATFORM_PATH}/adapters/gamestream/sunshine/include"
            "${ALKAIDLAB_ZAKO_INPUT_PATH}/include")
    if(WIN32)
        include_directories(
                "${ALKAIDLAB_PLATFORM_PATH}/modules/data/clipboard-sync/platform-clipboard-sync/backends/win32/include"
                "${ALKAIDLAB_PLATFORM_PATH}/modules/audio/microphone-uplink/opus-uplink/backends/windows-wasapi-sink/include")
    endif()
else()
    message(WARNING "AlkaidLab Platform was not found; building without platform Core/SDK/Adapter/Module integration")
endif()

include_directories(
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        ${FFMPEG_INCLUDE_DIRS}
        /opt/homebrew/include
        /opt/local/include
        /usr/local/include
        ${Boost_INCLUDE_DIRS}  # has to be the last, or we get runtime error on macOS ffmpeg encoder
)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${MINIUPNP_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        enet
        nlohmann_json::nlohmann_json
        opus
        ${FFMPEG_LIBRARIES}
        ${Boost_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})
