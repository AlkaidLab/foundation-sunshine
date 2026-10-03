cmake_minimum_required(VERSION 3.16)
project(sunshine_streaming_validation LANGUAGES C)

get_filename_component(TRANSPORT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TRANSPORT_COMMON_C_ROOT "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c" CACHE PATH "Client core under test")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${TRANSPORT_COMMON_C_ROOT}" common-c)
add_executable(transport_stream_client stream_client.c)
add_executable(transport_capture_output_tests capture_output_test.c)
foreach(target transport_stream_client transport_capture_output_tests)
target_include_directories(${target} PRIVATE
        "${TRANSPORT_COMMON_C_ROOT}/src"
        "${TRANSPORT_COMMON_C_ROOT}/enet/include")
target_link_libraries(${target} PRIVATE moonlight-common-c)
set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endif()
endforeach()
enable_testing()
add_test(NAME transport_capture_output_tests COMMAND transport_capture_output_tests)

# Optional offline validation using the client's existing Windows FFmpeg.
# Sunshine's encoder-only FFmpeg is insufficient; never treat a missing
# decoder as a successful decode. This target does not render video.
set(TRANSPORT_FFMPEG_ROOT "" CACHE PATH "Existing Moonlight Windows FFmpeg bundle")
if(TRANSPORT_FFMPEG_ROOT)
    if(NOT WIN32)
        message(FATAL_ERROR "TRANSPORT_FFMPEG_ROOT currently describes the Windows client bundle")
    endif()
    add_executable(transport_decode_capture decode_capture.c)
    target_include_directories(transport_decode_capture SYSTEM PRIVATE "${TRANSPORT_FFMPEG_ROOT}/include/x64")
    target_link_libraries(transport_decode_capture PRIVATE
            "${TRANSPORT_FFMPEG_ROOT}/lib/x64/avcodec.lib"
            "${TRANSPORT_FFMPEG_ROOT}/lib/x64/avutil.lib")
    set_target_properties(transport_decode_capture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(transport_decode_capture PRIVATE /W4 /WX)
    else()
        target_compile_options(transport_decode_capture PRIVATE -Wall -Wextra -Werror)
    endif()
    file(GLOB decoder_runtime_dlls
            "${TRANSPORT_FFMPEG_ROOT}/lib/x64/avcodec-*.dll"
            "${TRANSPORT_FFMPEG_ROOT}/lib/x64/avutil-*.dll"
            "${TRANSPORT_FFMPEG_ROOT}/lib/x64/dav1d.dll")
    foreach(runtime IN LISTS decoder_runtime_dlls)
        add_custom_command(TARGET transport_decode_capture POST_BUILD COMMAND "${CMAKE_COMMAND}" -E
                copy_if_different "${runtime}" "$<TARGET_FILE_DIR:transport_decode_capture>")
    endforeach()
endif()

# Explicit runtime invocation only: launching a stream needs an isolated host,
# paired test certificate, launch ticket and a bounded capture destination.
