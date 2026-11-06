cmake_minimum_required(VERSION 3.25)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sunshine)

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

if (WIN32)
    # Standalone MAT sender tests use a mock consumer, never an audio device.
    set(MAT_RECEIVER_TEST_SOURCES
            ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/MatLivePlayback.c
            ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/MatReceiver.c
            ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/MatDepacketizer.c)
    add_executable(mat_live_host_tests mat-live/host_test.cpp ${MAT_RECEIVER_TEST_SOURCES})
    target_link_libraries(mat_live_host_tests PRIVATE OpenSSL::Crypto bcrypt advapi32 shell32 ole32 uuid)
    set_target_properties(mat_live_host_tests PROPERTIES CXX_STANDARD 23 LINK_SEARCH_START_STATIC 1)
    add_test(NAME mat_live_host_tests COMMAND mat_live_host_tests)
    set_tests_properties(mat_live_host_tests PROPERTIES TIMEOUT 15)
    # Reuse the dependency's authentication/tamper cases without duplicating them.
    add_executable(mat_live_crypto_tests
            ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/tests/MatLiveTest.c
            ${MAT_RECEIVER_TEST_SOURCES})
    target_include_directories(mat_live_crypto_tests PRIVATE ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src)
    target_link_libraries(mat_live_crypto_tests PRIVATE OpenSSL::Crypto)
    add_test(NAME mat_live_crypto_tests COMMAND mat_live_crypto_tests)
    set_tests_properties(mat_live_crypto_tests PROPERTIES TIMEOUT 15)
    add_executable(mat_mock_output_tests mat-live/mock_output_test.c ${MAT_RECEIVER_TEST_SOURCES})
    target_include_directories(mat_mock_output_tests PRIVATE ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src)
    target_link_libraries(mat_mock_output_tests PRIVATE OpenSSL::Crypto)
    add_test(NAME mat_mock_output_tests COMMAND mat_mock_output_tests)
    set_tests_properties(mat_mock_output_tests PROPERTIES TIMEOUT 15)
endif ()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

add_executable(endpoint_refresh_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_endpoint_refresh.cpp
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/EndpointRefreshProtocol.c)
target_include_directories(endpoint_refresh_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/third-party")
target_link_libraries(endpoint_refresh_unit_tests PRIVATE gtest_main Boost::asio sunshine::tls sunshine_project_options sunshine_coverage)
target_compile_definitions(endpoint_refresh_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
set_target_properties(endpoint_refresh_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_link_libraries(endpoint_refresh_unit_tests PRIVATE ws2_32)
endif ()
add_test(NAME endpoint_refresh_unit_tests COMMAND endpoint_refresh_unit_tests)

# Adapter contract only: DSP fidelity belongs to the SDK, wire validation to common-c.
add_executable(authored_ir_extended_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_authored_ir_extended.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir_extended.cpp
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Ds5HapticsIrStream.c)
target_include_directories(authored_ir_extended_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(authored_ir_extended_unit_tests PRIVATE moonlight::haptics gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(authored_ir_extended_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME authored_ir_extended_unit_tests COMMAND authored_ir_extended_unit_tests)

add_executable(analyze_authored_ir_extended
        ${CMAKE_SOURCE_DIR}/tests/tools/analyze_authored_ir_extended.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir.cpp
        ${CMAKE_SOURCE_DIR}/src/haptics/authored_ir_extended.cpp)
target_include_directories(analyze_authored_ir_extended PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(analyze_authored_ir_extended PRIVATE moonlight::haptics sunshine_project_options)
set_target_properties(analyze_authored_ir_extended PROPERTIES CXX_STANDARD 23)

add_executable(perf_frame_rate_unit_tests
        ${CMAKE_SOURCE_DIR}/src/streaming/perf_recorder.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_perf_frame_rates.cpp)
target_include_directories(perf_frame_rate_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(perf_frame_rate_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
set_target_properties(perf_frame_rate_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(perf_frame_rate_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME perf_frame_rate_unit_tests COMMAND perf_frame_rate_unit_tests)
# The audio endpoint list: shaping and recognition only, no audio API.
add_executable(audio_devices_unit_tests
        ${CMAKE_SOURCE_DIR}/src/audio_devices.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_audio_devices.cpp)
target_include_directories(audio_devices_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(audio_devices_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json sunshine_project_options sunshine_coverage)
set_target_properties(audio_devices_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(audio_devices_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME audio_devices_unit_tests COMMAND audio_devices_unit_tests)

# The VB-CABLE install and enable steps against a fake system: nothing is downloaded, installed or enabled.
add_executable(audio_vb_cable_unit_tests
        ${CMAKE_SOURCE_DIR}/src/audio_vb_cable.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_audio_vb_cable.cpp)
target_include_directories(audio_vb_cable_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(audio_vb_cable_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json sunshine_project_options sunshine_coverage)
set_target_properties(audio_vb_cable_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(audio_vb_cable_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME audio_vb_cable_unit_tests COMMAND audio_vb_cable_unit_tests)

if (WIN32)
    # Prints what GET /api/audio/devices answers on this machine; reads endpoints only. Not a unit test.
    add_executable(audio_devices_dump
            ${CMAKE_SOURCE_DIR}/tests/tools/audio_devices_dump.cpp
            ${CMAKE_SOURCE_DIR}/src/audio_devices.cpp
            ${CMAKE_SOURCE_DIR}/src/audio_vb_cable.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/audio_devices.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/audio_vb_cable_win.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/text_encoding.cpp)
    target_include_directories(audio_devices_dump PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(audio_devices_dump PRIVATE nlohmann_json::nlohmann_json sunshine_project_options ole32 uuid oleaut32 setupapi cfgmgr32 wintrust crypt32)
    set_target_properties(audio_devices_dump PROPERTIES CXX_STANDARD 23 LINK_SEARCH_START_STATIC 1)
endif ()

add_executable(video_idle_policy_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_video_idle_policy.cpp)
target_include_directories(video_idle_policy_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(video_idle_policy_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(video_idle_policy_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME video_idle_policy_unit_tests COMMAND video_idle_policy_unit_tests)

add_executable(measured_bitrate_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_measured_bitrate.cpp)
target_include_directories(measured_bitrate_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(measured_bitrate_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(measured_bitrate_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME measured_bitrate_unit_tests COMMAND measured_bitrate_unit_tests)

add_executable(host_bitrate_cap_unit_tests
        ${CMAKE_SOURCE_DIR}/src/streaming/bitrate.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_host_bitrate_cap.cpp)
target_include_directories(host_bitrate_cap_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(host_bitrate_cap_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(host_bitrate_cap_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME host_bitrate_cap_unit_tests COMMAND host_bitrate_cap_unit_tests)

add_executable(dynamic_param_protocol_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_dynamic_param_protocol.cpp)
target_include_directories(dynamic_param_protocol_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(dynamic_param_protocol_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(dynamic_param_protocol_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME dynamic_param_protocol_unit_tests COMMAND dynamic_param_protocol_unit_tests)

add_executable(pair_link_unit_tests
        ${CMAKE_SOURCE_DIR}/src/confighttp/pair_link.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_pair_link.cpp)
target_include_directories(pair_link_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(pair_link_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage Boost::asio
        $<$<PLATFORM_ID:Windows>:ws2_32>)
set_target_properties(pair_link_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME pair_link_unit_tests COMMAND pair_link_unit_tests)

add_executable(uac1_microphone_unit_tests
        ${CMAKE_SOURCE_DIR}/src/virtual_usb/uac1_microphone.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_uac1_microphone.cpp)
target_include_directories(uac1_microphone_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(uac1_microphone_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(uac1_microphone_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME uac1_microphone_unit_tests COMMAND uac1_microphone_unit_tests)

add_executable(config_overrides_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_config_overrides.cpp)
target_include_directories(config_overrides_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(config_overrides_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(config_overrides_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME config_overrides_unit_tests COMMAND config_overrides_unit_tests)
# The published encoder snapshot and the probe reuse decision: header only, no driver or display.
add_executable(encoder_snapshot_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_encoder_snapshot.cpp)
target_include_directories(encoder_snapshot_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(encoder_snapshot_unit_tests PRIVATE gtest_main Threads::Threads sunshine_project_options sunshine_coverage)
set_target_properties(encoder_snapshot_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(encoder_snapshot_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME encoder_snapshot_unit_tests COMMAND encoder_snapshot_unit_tests)

# Keeping the previous run's log across a restart: std::filesystem only.
add_executable(log_retention_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_log_retention.cpp)
target_include_directories(log_retention_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(log_retention_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(log_retention_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(log_retention_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME log_retention_unit_tests COMMAND log_retention_unit_tests)

add_executable(client_interface_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_client_interface.cpp)
target_include_directories(client_interface_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(client_interface_unit_tests PRIVATE gtest_main Boost::boost Boost::property_tree sunshine_project_options sunshine_coverage)
set_target_properties(client_interface_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME client_interface_unit_tests COMMAND client_interface_unit_tests)

add_executable(display_scale_steps_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_display_scale_steps.cpp)
target_include_directories(display_scale_steps_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(display_scale_steps_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(display_scale_steps_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME display_scale_steps_unit_tests COMMAND display_scale_steps_unit_tests)

add_executable(vdd_mode_list_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_vdd_mode_list.cpp)
target_include_directories(vdd_mode_list_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(vdd_mode_list_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        Boost::optional
        sunshine_project_options sunshine_coverage)
set_target_properties(vdd_mode_list_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME vdd_mode_list_unit_tests COMMAND vdd_mode_list_unit_tests)

add_executable(current_capture_target_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_current_capture_target.cpp)
target_include_directories(current_capture_target_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(current_capture_target_unit_tests PRIVATE gtest_main sunshine_project_options sunshine_coverage)
set_target_properties(current_capture_target_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME current_capture_target_unit_tests COMMAND current_capture_target_unit_tests)

add_executable(pen_barrel_roll_unit_tests ${CMAKE_SOURCE_DIR}/tests/unit/test_pen_barrel_roll.cpp)
target_include_directories(pen_barrel_roll_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/third-party")
target_link_libraries(pen_barrel_roll_unit_tests PRIVATE gtest_main
        sunshine_project_options sunshine_coverage)
set_target_properties(pen_barrel_roll_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME pen_barrel_roll_unit_tests COMMAND pen_barrel_roll_unit_tests)


# Keep compatibility conversion tests independent of the platform host backends.
add_executable(ai_provider_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_ai_provider.cpp)
target_include_directories(ai_provider_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ai_provider_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage sunshine_ai_provider)
set_target_properties(ai_provider_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME ai_provider_unit_tests COMMAND ai_provider_unit_tests)

add_executable(beast_http_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/tools/test_beast_http.cpp)
target_include_directories(beast_http_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(beast_http_unit_tests PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES} gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_definitions(beast_http_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
set_target_properties(beast_http_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(beast_http_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME beast_http_unit_tests COMMAND beast_http_unit_tests)
set_tests_properties(beast_http_unit_tests PROPERTIES TIMEOUT 60)

add_executable(webhook_transport_tests
        ${CMAKE_SOURCE_DIR}/tests/tools/test_webhook_transport.cpp
        ${CMAKE_SOURCE_DIR}/src/webhook/webhook.cpp
        ${CMAKE_SOURCE_DIR}/src/webhook/webhook_format.cpp)
target_include_directories(webhook_transport_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(webhook_transport_tests PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES} gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_definitions(webhook_transport_tests PRIVATE ${SUNSHINE_DEFINITIONS})
set_target_properties(webhook_transport_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(webhook_transport_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME webhook_transport_tests COMMAND webhook_transport_tests)
set_tests_properties(webhook_transport_tests PROPERTIES TIMEOUT 60)

# Exercise real curl transfers and a single-threaded HTTPS listener in isolation.
add_executable(http_safety_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/tools/test_http_safety.cpp
        ${CMAKE_SOURCE_DIR}/src/ai/proxy.cpp)
target_include_directories(http_safety_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(http_safety_unit_tests PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES} gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto sunshine_ai_provider sunshine_ai_stream)
target_compile_definitions(http_safety_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
set_target_properties(http_safety_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(http_safety_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME http_safety_unit_tests COMMAND http_safety_unit_tests)
set_tests_properties(http_safety_unit_tests PROPERTIES TIMEOUT 45)


if (WIN32)
    # Link fake NVAPI entrypoints from the fixture, never the real driver wrapper.
    add_executable(nvprefs_unit_tests
            ${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/driver_settings.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/nvprefs_interface.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/undo_data.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/undo_file.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_nvprefs.cpp)
    target_include_directories(nvprefs_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/third-party/nvapi")
    target_link_libraries(nvprefs_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
            sunshine_project_options sunshine_coverage dxgi advapi32)
    target_compile_definitions(nvprefs_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
    set_target_properties(nvprefs_unit_tests PROPERTIES CXX_STANDARD 23 LINK_SEARCH_START_STATIC 1)
    add_test(NAME nvprefs_unit_tests COMMAND nvprefs_unit_tests)
endif ()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

add_executable(hdr_calibration_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_hdr_calibration.cpp
        ${CMAKE_SOURCE_DIR}/src/hdr/calibration.cpp
        ${CMAKE_SOURCE_DIR}/src/hdr/client_display_capabilities.cpp
        ${CMAKE_SOURCE_DIR}/src/display_device/color_profile.cpp)
target_include_directories(hdr_calibration_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(hdr_calibration_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
set_target_properties(hdr_calibration_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(hdr_calibration_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    target_sources(hdr_calibration_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/hdr/session_target.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/test_hdr_session_target.cpp)
    target_link_libraries(hdr_calibration_unit_tests PRIVATE sunshine::host_headers sunshine::tls
          Boost::log Boost::format Boost::process)
    target_compile_definitions(hdr_calibration_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
endif ()
add_test(NAME hdr_calibration_unit_tests COMMAND hdr_calibration_unit_tests)

# Compiles the real config.cpp update paths. These tests mutate global config
# state and run config::parse, so they cannot join the aggregate suite.
set(CONFIG_RUNTIME_SUPPORT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/config.cpp
        ${CMAKE_SOURCE_DIR}/src/streaming/bitrate.cpp
        ${CMAKE_SOURCE_DIR}/src/file_handler.cpp
        ${CMAKE_SOURCE_DIR}/src/display_device/parsed_config.cpp
        ${CMAKE_SOURCE_DIR}/src/display_device/to_string.cpp
        ${CMAKE_SOURCE_DIR}/src/hdr/calibration.cpp
        ${CMAKE_SOURCE_DIR}/src/hdr/client_display_capabilities.cpp
        ${CMAKE_SOURCE_DIR}/src/display_device/color_profile.cpp)
target_include_directories(config_runtime_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}")  # generated version.h
target_link_libraries(config_runtime_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage
        alkaidlab_stun_codec
        sunshine::host_headers sunshine::tls
        Boost::boost Boost::log Boost::filesystem Boost::property_tree Boost::asio Boost::beast Boost::regex
        Boost::format Boost::process)
set_target_properties(config_runtime_unit_tests PROPERTIES CXX_STANDARD 23)
target_compile_definitions(config_runtime_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
if (WIN32)
    set_target_properties(config_runtime_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    # windows.cmake stages AMF headers here for the host build; config.cpp's
    # encoder-enums branch includes them on Windows.
    target_include_directories(config_runtime_unit_tests SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/amf_include")
    target_link_libraries(config_runtime_unit_tests PRIVATE ws2_32)
endif ()
add_test(NAME config_runtime_unit_tests COMMAND config_runtime_unit_tests)

add_executable(tray_state_unit_tests
        ${CMAKE_SOURCE_DIR}/src/tray/tray_state.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_http_util.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_tray_state.cpp)
target_include_directories(tray_state_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(tray_state_unit_tests
        PRIVATE nlohmann_json::nlohmann_json
        gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(tray_state_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(tray_state_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(tray_state_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME tray_state_unit_tests COMMAND tray_state_unit_tests)
# Session and login-throttle policy are independent of a listening server and
# of the platform display stack, so their suite stays a focused standalone
# target on every host.
add_executable(session_store_unit_tests
        ${CMAKE_SOURCE_DIR}/src/confighttp/session_store.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/login_throttle.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_session_store.cpp)
target_include_directories(session_store_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(session_store_unit_tests PRIVATE gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_options(session_store_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(session_store_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(session_store_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME session_store_unit_tests COMMAND session_store_unit_tests)

# Exercise the auth handlers over a real TLS listener without linking the
# platform display and streaming stacks. The test file supplies the small set
# of globals those handlers read.
add_executable(session_auth_routes_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_session_auth_routes.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/auth.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/local_auth_token.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/common.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/session_store.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/login_throttle.cpp
        ${CMAKE_SOURCE_DIR}/src/http/server.cpp
        ${CMAKE_SOURCE_DIR}/src/http/client.cpp
        ${CMAKE_SOURCE_DIR}/src/http/types.cpp
        ${CMAKE_SOURCE_DIR}/src/http/tls_trust.cpp)
target_include_directories(session_auth_routes_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(session_auth_routes_unit_tests PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES} gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto ${PLATFORM_LIBRARIES})
target_compile_definitions(session_auth_routes_unit_tests PRIVATE ${SUNSHINE_DEFINITIONS})
target_compile_options(session_auth_routes_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(session_auth_routes_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(session_auth_routes_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME session_auth_routes_unit_tests COMMAND session_auth_routes_unit_tests)
set_tests_properties(session_auth_routes_unit_tests PROPERTIES TIMEOUT 60)

add_executable(sunshinesvc_state_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_sunshinesvc_state.cpp)
target_include_directories(sunshinesvc_state_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(sunshinesvc_state_unit_tests PRIVATE gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(sunshinesvc_state_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(sunshinesvc_state_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(sunshinesvc_state_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME sunshinesvc_state_unit_tests COMMAND sunshinesvc_state_unit_tests)

# The log sink tests exercise Windows file-sharing behavior from the service wrapper.
if (WIN32)
    add_executable(sunshinesvc_log_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/test_sunshinesvc_log.cpp)
    target_include_directories(sunshinesvc_log_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(sunshinesvc_log_unit_tests PRIVATE gtest_main
            sunshine_project_options sunshine_coverage)
    target_compile_options(sunshinesvc_log_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(sunshinesvc_log_unit_tests PROPERTIES CXX_STANDARD 23)
    set_target_properties(sunshinesvc_log_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    add_test(NAME sunshinesvc_log_unit_tests COMMAND sunshinesvc_log_unit_tests)
endif ()

add_executable(video_probe_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_video_probe.cpp)
target_include_directories(video_probe_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(video_probe_unit_tests PRIVATE gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(video_probe_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(video_probe_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(video_probe_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME video_probe_unit_tests COMMAND video_probe_unit_tests)

add_executable(frame_contract_unit_tests
        ${CMAKE_SOURCE_DIR}/src/platform/frame_contract.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_frame_contract.cpp)
if (WIN32)
    target_sources(frame_contract_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/platform/windows/frame_contract.cpp)
endif ()
target_include_directories(frame_contract_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(frame_contract_unit_tests PRIVATE gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(frame_contract_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(frame_contract_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(frame_contract_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME frame_contract_unit_tests COMMAND frame_contract_unit_tests)
add_executable(session_info_unit_tests
        ${CMAKE_SOURCE_DIR}/src/session_info.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_session_info.cpp)
target_include_directories(session_info_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(session_info_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
target_compile_options(session_info_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(session_info_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(session_info_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME session_info_unit_tests COMMAND session_info_unit_tests)

add_executable(network_reachability_unit_tests
        ${CMAKE_SOURCE_DIR}/src/network_reachability.cpp
        ${CMAKE_SOURCE_DIR}/src/public_address.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_network_reachability.cpp)
target_include_directories(network_reachability_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(network_reachability_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage ${Boost_LIBRARIES})
target_compile_options(network_reachability_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(network_reachability_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(network_reachability_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME network_reachability_unit_tests COMMAND network_reachability_unit_tests)

# 版本比较: the date part of a Foundation Sunshine version, against the table shared with the panel and the Web UI
add_executable(version_compare_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_version_compare.cpp)
target_include_directories(version_compare_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(version_compare_unit_tests PRIVATE
        SUNSHINE_VERSION_VECTORS="${CMAKE_SOURCE_DIR}/tests/fixtures/version_vectors.json")
target_link_libraries(version_compare_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
target_compile_options(version_compare_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(version_compare_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(version_compare_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME version_compare_unit_tests COMMAND version_compare_unit_tests)

add_executable(system_info_unit_tests
        ${CMAKE_SOURCE_DIR}/src/system_info.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_system_info.cpp)
target_include_directories(system_info_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(system_info_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
target_compile_options(system_info_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(system_info_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(system_info_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
else ()
    target_sources(system_info_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/system_info_probe.cpp
            ${CMAKE_SOURCE_DIR}/src/network_reachability.cpp
            ${CMAKE_SOURCE_DIR}/src/public_address.cpp)
    target_link_libraries(system_info_unit_tests PRIVATE ${Boost_LIBRARIES})
endif ()
add_test(NAME system_info_unit_tests COMMAND system_info_unit_tests)

# Build identity: the compiled record (generated build_info_defs.h and version.h in the build tree) and its JSON contract.
add_executable(build_info_unit_tests
        ${CMAKE_SOURCE_DIR}/src/build_info.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_build_info.cpp)
# The binary FILEVERSION encoding: HHMMSS above 65535 is split, every field fits 16 bits.
add_test(NAME rc_version_encoding COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tests/cmake/test_rc_version.cmake)
# A git archive tree (no .git) records its commit from cmake/prep/build_identity_archive.txt (export-subst).
add_test(NAME build_info_archive COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tests/cmake/test_build_info_archive.cmake WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(network_reachability_sample
        ${CMAKE_SOURCE_DIR}/tests/tools/network_reachability_sample.cpp
        ${CMAKE_SOURCE_DIR}/src/network_reachability.cpp
        ${CMAKE_SOURCE_DIR}/src/public_address.cpp
        ${CMAKE_SOURCE_DIR}/src/network_reachability_probe.cpp)
target_include_directories(network_reachability_sample PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/third-party")
target_link_libraries(network_reachability_sample PRIVATE nlohmann_json::nlohmann_json
        sunshine_project_options Boost::boost ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES} ${MINIUPNP_LIBRARIES})
target_compile_options(network_reachability_sample PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(network_reachability_sample PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_compile_definitions(network_reachability_sample PRIVATE MINIUPNPC_STATICLIB MINIUPNP_STATICLIB)
    target_link_libraries(network_reachability_sample PRIVATE oleaut32 iphlpapi)
    set_target_properties(network_reachability_sample PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()

add_executable(network_overview_sample
        ${CMAKE_SOURCE_DIR}/tests/tools/network_overview_sample.cpp
        ${CMAKE_SOURCE_DIR}/src/nat_discovery.cpp
        ${CMAKE_SOURCE_DIR}/src/nat_discovery_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/network_reachability.cpp
        ${CMAKE_SOURCE_DIR}/src/public_address.cpp
        ${CMAKE_SOURCE_DIR}/src/network_reachability_probe.cpp)
target_include_directories(network_overview_sample PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/third-party")
target_link_libraries(network_overview_sample PRIVATE nlohmann_json::nlohmann_json alkaidlab_stun_codec
        sunshine_project_options Boost::boost ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES} ${MINIUPNP_LIBRARIES})
target_compile_options(network_overview_sample PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(network_overview_sample PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_compile_definitions(network_overview_sample PRIVATE MINIUPNPC_STATICLIB MINIUPNP_STATICLIB)
    target_link_libraries(network_overview_sample PRIVATE oleaut32 iphlpapi)
    set_target_properties(network_overview_sample PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()

add_executable(system_load_unit_tests
        ${CMAKE_SOURCE_DIR}/src/system_load.cpp
        ${CMAKE_SOURCE_DIR}/src/system_load_stub.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_system_load.cpp)
target_include_directories(system_load_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(system_load_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
target_compile_options(system_load_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(system_load_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(system_load_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME system_load_unit_tests COMMAND system_load_unit_tests)

add_executable(pyrowave_unit_tests
        ${CMAKE_SOURCE_DIR}/src/pyrowave/types.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/config.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/capabilities.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/gpu_interop.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/packet.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/packetizer.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/session.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/runtime.cpp
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/PyrowaveProtocol.c
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/PyrowaveReassembly.c
        ${CMAKE_SOURCE_DIR}/tests/tools/pyrowave_test_stubs.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_pyrowave.cpp)
target_include_directories(pyrowave_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(pyrowave_unit_tests PRIVATE Boost::log Boost::format gtest_main
        sunshine_project_options sunshine_coverage)
target_link_libraries(pyrowave_unit_tests PRIVATE sunshine::host_headers)
target_compile_options(pyrowave_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(pyrowave_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    target_sources(pyrowave_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/pyrowave/dynamic_hdr.cpp
            ${CMAKE_SOURCE_DIR}/src/video_hdr_metadata.cpp
            ${CMAKE_SOURCE_DIR}/src/video_dolby_vision.cpp
            ${CMAKE_SOURCE_DIR}/src/video_hdr_bitstream.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/pyrowave/transport.cpp)
    target_link_libraries(pyrowave_unit_tests PRIVATE Pyrowave::Runtime sunshine::ffmpeg)
    set_target_properties(pyrowave_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME pyrowave_unit_tests COMMAND pyrowave_unit_tests)

add_executable(pyrowave_packetizer_probe
        ${CMAKE_SOURCE_DIR}/src/pyrowave/types.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/packet.cpp
        ${CMAKE_SOURCE_DIR}/src/pyrowave/packetizer.cpp
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/PyrowaveProtocol.c
        ${CMAKE_SOURCE_DIR}/tests/tools/pyrowave_test_stubs.cpp
        ${CMAKE_SOURCE_DIR}/tests/tools/pyrowave_packetizer_probe.cpp)
target_include_directories(pyrowave_packetizer_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(pyrowave_packetizer_probe PRIVATE Boost::log Boost::format
        sunshine_project_options sunshine_coverage)
target_compile_options(pyrowave_packetizer_probe PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(pyrowave_packetizer_probe PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
if (WIN32)
    set_target_properties(pyrowave_packetizer_probe PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME pyrowave_packetizer_probe COMMAND pyrowave_packetizer_probe)

if (WIN32)
    set(HDR_ENCODING_TEST_SOURCE
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_hdr_encoding.cpp)
    add_executable(hdr_encoding_unit_tests
            ${HDR_ENCODING_TEST_SOURCE})
    target_link_libraries(hdr_encoding_unit_tests PRIVATE d3d11 d3dcompiler dxgi gtest_main
            sunshine_project_options sunshine_coverage)
    target_compile_definitions(hdr_encoding_unit_tests PRIVATE
            SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
    set_target_properties(hdr_encoding_unit_tests PROPERTIES CXX_STANDARD 23 LINK_SEARCH_START_STATIC 1)
    add_test(NAME hdr_encoding_unit_tests COMMAND hdr_encoding_unit_tests)

    set(PRE_ENCODE_FILTER_TEST_SOURCE
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_pre_encode_filter.cpp)
    set(TRUEHDR_ADAPTER_LOADER_TEST_SOURCE
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_truehdr_adapter_loader.cpp)

    add_executable(pre_encode_filter_unit_tests
            ${CMAKE_SOURCE_DIR}/src/platform/windows/pre_encode_filter.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/backend_factory.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/rtx_hdr/truehdr_filter.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/rtx_hdr/adapter_loader.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/dlssnr_filter.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/hdr_filter.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/dlss_nr/adapter_loader.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/pre_encode_filter_test_stubs.cpp
            ${PRE_ENCODE_FILTER_TEST_SOURCE})
    target_include_directories(pre_encode_filter_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(pre_encode_filter_unit_tests PRIVATE sunshine::host_headers
            sunshine_project_options sunshine_coverage)
    target_link_libraries(pre_encode_filter_unit_tests
            PRIVATE Boost::log
            Boost::thread
            d3d11
            d3dcompiler
            dxgi
            gtest_main)
    target_compile_definitions(pre_encode_filter_unit_tests PRIVATE
            SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
    target_compile_options(pre_encode_filter_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(pre_encode_filter_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_test(NAME pre_encode_filter_unit_tests COMMAND pre_encode_filter_unit_tests)

    if (SUNSHINE_DLSSNR_AVAILABLE)
        # Hardware/runtime are supplied explicitly; do not add to ordinary CTest.
        get_target_property(_nr_filter_sources pre_encode_filter_unit_tests SOURCES)
        list(REMOVE_ITEM _nr_filter_sources "${PRE_ENCODE_FILTER_TEST_SOURCE}")
        add_executable(dlssnr_pipeline_smoke ${_nr_filter_sources}
                ${CMAKE_SOURCE_DIR}/tests/tools/dlssnr_pipeline_smoke.cpp)
        target_include_directories(dlssnr_pipeline_smoke PRIVATE "${CMAKE_SOURCE_DIR}" "${DLSSNR_TRUST_INCLUDE}")
        target_compile_definitions(dlssnr_pipeline_smoke PRIVATE SUNSHINE_DLSSNR_ADAPTER
                SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
        target_link_libraries(dlssnr_pipeline_smoke PRIVATE Boost::log Boost::thread d3d11 d3dcompiler dxgi bcrypt
                sunshine_project_options sunshine_coverage)
        target_link_libraries(dlssnr_pipeline_smoke PRIVATE sunshine::host_headers)
        set_target_properties(dlssnr_pipeline_smoke PROPERTIES CXX_STANDARD 23 LINK_SEARCH_START_STATIC 1)
        add_dependencies(dlssnr_pipeline_smoke sunshine_dlssnr_adapter)
    endif ()

    add_library(fake_truehdr_adapter SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_adapter.cpp)
    target_link_libraries(fake_truehdr_adapter PRIVATE
            sunshine_project_options sunshine_coverage)
    target_include_directories(fake_truehdr_adapter PRIVATE "${CMAKE_SOURCE_DIR}")
    set_target_properties(fake_truehdr_adapter PROPERTIES
            CXX_STANDARD 23
            PREFIX "")
    add_custom_command(TARGET fake_truehdr_adapter POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E touch "$<TARGET_FILE_DIR:fake_truehdr_adapter>/nvngx_truehdr.dll")

    add_library(fake_truehdr_bad_adapter SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_adapter.cpp)
    target_link_libraries(fake_truehdr_bad_adapter PRIVATE
            sunshine_project_options sunshine_coverage)
    target_include_directories(fake_truehdr_bad_adapter PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(fake_truehdr_bad_adapter PRIVATE FAKE_TRUEHDR_BAD_ABI)
    set_target_properties(fake_truehdr_bad_adapter PROPERTIES
            CXX_STANDARD 23
            PREFIX "")

    add_library(fake_truehdr_failing_adapter SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_adapter.cpp)
    target_link_libraries(fake_truehdr_failing_adapter PRIVATE
            sunshine_project_options sunshine_coverage)
    target_include_directories(fake_truehdr_failing_adapter PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(fake_truehdr_failing_adapter PRIVATE FAKE_TRUEHDR_PROCESS_FAILS)
    set_target_properties(fake_truehdr_failing_adapter PROPERTIES
            CXX_STANDARD 23
            PREFIX "")

    add_executable(truehdr_adapter_loader_unit_tests
            ${CMAKE_SOURCE_DIR}/src/platform/windows/image_enhancement/rtx_hdr/adapter_loader.cpp
            ${TRUEHDR_ADAPTER_LOADER_TEST_SOURCE})
    target_include_directories(truehdr_adapter_loader_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(truehdr_adapter_loader_unit_tests PRIVATE gtest_main
            sunshine_project_options sunshine_coverage)
    target_compile_definitions(truehdr_adapter_loader_unit_tests PRIVATE
            FAKE_TRUEHDR_ADAPTER_PATH="$<TARGET_FILE:fake_truehdr_adapter>"
            FAKE_TRUEHDR_BAD_ADAPTER_PATH="$<TARGET_FILE:fake_truehdr_bad_adapter>"
            FAKE_TRUEHDR_RUNTIME_PATH="$<TARGET_FILE_DIR:fake_truehdr_adapter>/nvngx_truehdr.dll")
    target_compile_options(truehdr_adapter_loader_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(truehdr_adapter_loader_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_dependencies(truehdr_adapter_loader_unit_tests
            fake_truehdr_adapter
            fake_truehdr_bad_adapter)
    add_dependencies(pre_encode_filter_unit_tests
            fake_truehdr_adapter
            fake_truehdr_failing_adapter)
    target_compile_definitions(pre_encode_filter_unit_tests PRIVATE
            FAKE_TRUEHDR_ADAPTER_PATH="$<TARGET_FILE:fake_truehdr_adapter>"
            FAKE_TRUEHDR_FAILING_ADAPTER_PATH="$<TARGET_FILE:fake_truehdr_failing_adapter>"
            FAKE_TRUEHDR_RUNTIME_PATH="$<TARGET_FILE_DIR:fake_truehdr_adapter>/nvngx_truehdr.dll")
    add_test(NAME truehdr_adapter_loader_unit_tests COMMAND truehdr_adapter_loader_unit_tests)

    add_executable(vdd_prerequisite_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/test_vdd_prerequisite.cpp)
    target_include_directories(vdd_prerequisite_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vdd_prerequisite_unit_tests
            PRIVATE Boost::optional
            gtest_main
            sunshine_project_options sunshine_coverage)
    target_compile_options(vdd_prerequisite_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(vdd_prerequisite_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_test(NAME vdd_prerequisite_unit_tests COMMAND vdd_prerequisite_unit_tests)

    add_executable(credential_store_unit_tests
            ${CMAKE_SOURCE_DIR}/src/ai/credential_store.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/test_credential_store.cpp)
    target_include_directories(credential_store_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(credential_store_unit_tests
            PRIVATE advapi32
            crypt32
            gtest_main
            sunshine_project_options sunshine_coverage)
    target_compile_options(credential_store_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(credential_store_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_test(NAME credential_store_unit_tests COMMAND credential_store_unit_tests)
endif ()

if (NOT BUILD_TESTS)
    return()
endif ()

set(REMOTE_USB_SOCKET_LIBRARIES)
if (WIN32)
    list(APPEND REMOTE_USB_SOCKET_LIBRARIES mswsock ws2_32)
endif ()

add_executable(loopback_usbip_bridge_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_loopback_usbip_bridge.cpp)
target_include_directories(loopback_usbip_bridge_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(loopback_usbip_bridge_unit_tests
        PRIVATE Boost::asio
        Boost::system
        OpenSSL::Crypto
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        gtest_main
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_options(loopback_usbip_bridge_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(loopback_usbip_bridge_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME loopback_usbip_bridge_unit_tests COMMAND loopback_usbip_bridge_unit_tests)

add_executable(virtual_touchscreen_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/virtual_touchscreen_device.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_virtual_touchscreen_device.cpp)
target_include_directories(virtual_touchscreen_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(virtual_touchscreen_unit_tests
        PRIVATE Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(virtual_touchscreen_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(virtual_touchscreen_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME virtual_touchscreen_unit_tests COMMAND virtual_touchscreen_unit_tests)

add_executable(remote_usb_process_tree_helper
        ${CMAKE_SOURCE_DIR}/tests/tools/remote_usb_process_tree_helper.cpp)
target_link_libraries(remote_usb_process_tree_helper PRIVATE
        Boost::process Boost::system ${CMAKE_THREAD_LIBS_INIT}
        sunshine_project_options sunshine_coverage)
set_target_properties(remote_usb_process_tree_helper PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

add_executable(remote_usb_host_controller_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/usbip_stale_port_sweep.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_usbip_stale_port_sweep.cpp)
if (WIN32)
    target_sources(remote_usb_host_controller_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/platform/windows/text_encoding.cpp)
endif ()
target_include_directories(remote_usb_host_controller_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_host_controller_unit_tests
        PRIVATE Boost::asio
        Boost::process
        Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_options(remote_usb_host_controller_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
target_compile_definitions(remote_usb_host_controller_unit_tests PRIVATE
        REMOTE_USB_PROCESS_TREE_HELPER="$<TARGET_FILE:remote_usb_process_tree_helper>")
add_dependencies(remote_usb_host_controller_unit_tests remote_usb_process_tree_helper)
set_target_properties(remote_usb_host_controller_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_host_controller_unit_tests COMMAND remote_usb_host_controller_unit_tests)

# Drives the production TLS service for tests/tools/test_reverse_tunnel.py and
# paired Moonlight probe runs, including real usbip-win2 device imports.
add_executable(reverse_tunnel_probe
        ${CMAKE_SOURCE_DIR}/tests/tools/reverse_tunnel_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/reverse_tunnel_service.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/usbip_stale_port_sweep.cpp)
if (WIN32)
    target_sources(reverse_tunnel_probe PRIVATE
            ${CMAKE_SOURCE_DIR}/src/platform/windows/text_encoding.cpp)
endif ()
target_include_directories(reverse_tunnel_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(reverse_tunnel_probe PRIVATE
        Boost::asio Boost::process Boost::system Boost::log
        OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json
        ${CMAKE_THREAD_LIBS_INIT} ${REMOTE_USB_SOCKET_LIBRARIES}
        sunshine_project_options sunshine_coverage)
set_target_properties(reverse_tunnel_probe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
        AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_test(NAME controller_handoff_probe
            COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/tools/controller_handoff_probe.py"
            --compiler "${CMAKE_CXX_COMPILER}")
    set_tests_properties(controller_handoff_probe PROPERTIES TIMEOUT 60)
endif ()
if(Python3_Interpreter_FOUND)
    add_test(NAME cmake_configuration_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/tools/test_cmake_configuration.py"
                --source "${CMAKE_SOURCE_DIR}" --cmake "${CMAKE_COMMAND}"
                --compiler "${CMAKE_CXX_COMPILER}")
    set_tests_properties(cmake_configuration_tests PROPERTIES TIMEOUT 180)
endif()
find_program(REVERSE_TUNNEL_OPENSSL_EXECUTABLE openssl)
if (Python3_Interpreter_FOUND AND REVERSE_TUNNEL_OPENSSL_EXECUTABLE)
    # The gate builds the ctest targets only; the probe is built here so the
    # transport test can run from a plain ctest invocation.
    add_test(NAME reverse_tunnel_probe_build
            COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                    --target reverse_tunnel_probe --parallel 4)
    set_tests_properties(reverse_tunnel_probe_build PROPERTIES
            FIXTURES_SETUP REVERSE_TUNNEL_PROBE)
    add_test(NAME reverse_tunnel_transport_tests
            COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/tools/test_reverse_tunnel.py"
            "$<TARGET_FILE:reverse_tunnel_probe>"
            --openssl "${REVERSE_TUNNEL_OPENSSL_EXECUTABLE}")
    set_tests_properties(reverse_tunnel_transport_tests PROPERTIES
            TIMEOUT 60
            FIXTURES_REQUIRED REVERSE_TUNNEL_PROBE)
endif ()

# modify SUNSHINE_DEFINITIONS
if (WIN32)
    list(APPEND
            SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed

# this indicates we're building tests in case sunshine needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS SUNSHINE_TESTS)

# Focused pending-pairing suite. It links the pairing registry, the Web UI
# route module and their direct dependencies instead of the full host graph.
add_executable(pairing_pending_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/tests_main.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_pairing_pending.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/pairing_pending_platform_stubs.cpp
        ${CMAKE_SOURCE_DIR}/src/client_family.cpp
        ${CMAKE_SOURCE_DIR}/src/nvhttp/pairing.cpp
        ${CMAKE_SOURCE_DIR}/src/nvhttp/pairing_pending.cpp
        ${CMAKE_SOURCE_DIR}/src/nvhttp/preset_pin.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/pairing.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/auth.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/local_auth_token.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/common.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/login_throttle.cpp
        ${CMAKE_SOURCE_DIR}/src/confighttp/session_store.cpp
        ${CMAKE_SOURCE_DIR}/src/file_handler.cpp
        ${CMAKE_SOURCE_DIR}/src/globals.cpp
        ${CMAKE_SOURCE_DIR}/src/httpcommon.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/network.cpp
        ${CMAKE_SOURCE_DIR}/src/tray/tray_state.cpp)
if (WIN32)
    target_sources(pairing_pending_unit_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src/platform/windows/text_encoding.cpp)
endif ()
target_include_directories(pairing_pending_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers/1301/include"
        "${FFMPEG_PREPARED_BINARIES}/include")
target_link_libraries(pairing_pending_unit_tests PRIVATE
        sunshine_http sunshine_crypto
        Boost::log Boost::format Boost::filesystem Boost::thread Boost::system
        Boost::property_tree Boost::program_options Boost::process
        Boost::locale Boost::atomic Boost::chrono Boost::date_time
        Boost::container Boost::context Boost::random Boost::charconv
        nlohmann_json::nlohmann_json
        enet
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        gtest_main
        sunshine_project_options sunshine_coverage)
if (WIN32)
    target_link_libraries(pairing_pending_unit_tests PRIVATE
            ${PLATFORM_LIBRARIES} bcrypt iphlpapi)
else ()
    target_link_libraries(pairing_pending_unit_tests PRIVATE m)
endif ()
target_compile_definitions(pairing_pending_unit_tests PRIVATE
        ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(pairing_pending_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(pairing_pending_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME pairing_pending_unit_tests COMMAND pairing_pending_unit_tests)
set_tests_properties(pairing_pending_unit_tests PROPERTIES TIMEOUT 120)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)

# Standalone diagnostic programs under tests/tools have their own targets and
# must not be linked into the aggregate GoogleTest executable.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/tools/")
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_hdr_calibration.cpp)
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_authored_ir_extended.cpp)
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_audio_devices.cpp)
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_audio_vb_cable.cpp)
list(REMOVE_ITEM TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/unit/test_host_bitrate_cap.cpp)
# MAT harnesses have independent entrypoints and explicit test targets.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/mat-live/")
# These files already run in registered standalone suites. Keep one copy of
# their assertions and isolate their fixtures from the aggregate host state.
list(REMOVE_ITEM TEST_SOURCES
        ${CMAKE_SOURCE_DIR}/tests/unit/test_ai_provider.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_build_info.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_version_compare.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_client_family.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_client_fingerprint.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_config_overrides.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_config_runtime.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/config_runtime_stubs.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_current_capture_target.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_encoder_snapshot.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_log_retention.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_display_scale_steps.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_endpoint_refresh.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_frame_contract.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_http_util.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_input_activity.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_pen_barrel_roll.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_perf_frame_rates.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_pyrowave.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_usbip_stale_port_sweep.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_rtsp.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_session_auth_routes.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_session_store.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_sunshinesvc_state.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_tray_state.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_uac1_microphone.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_video_probe.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_virtual_touchscreen_device.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/pairing_pending_platform_stubs.cpp)

set(ABR_TEST_STUB_SOURCE ${CMAKE_SOURCE_DIR}/tests/unit/abr_test_stubs.cpp)
list(REMOVE_ITEM TEST_SOURCES ${ABR_TEST_STUB_SOURCE})
if (WIN32)
    # Windows-only suites include fake APIs or backend paths of their own.
    list(REMOVE_ITEM TEST_SOURCES
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_ds5_component_manifest.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_ds5_sidecar_client.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_microphone_client.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_nvprefs.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/test_credential_store.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/test_sunshinesvc_log.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/test_vdd_prerequisite.cpp
            ${HDR_ENCODING_TEST_SOURCE}
            ${PRE_ENCODE_FILTER_TEST_SOURCE}
            ${TRUEHDR_ADAPTER_LOADER_TEST_SOURCE})
endif ()

set(SUNSHINE_SOURCES
        ${SUNSHINE_HOST_SOURCES}
        ${PLATFORM_TARGET_FILES}
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Ds5HapticsIrStream.c)

add_executable(${PROJECT_NAME} ${TEST_SOURCES})
target_sources(${PROJECT_NAME} PRIVATE
        ${SUNSHINE_SOURCES}
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/PyrowaveReassembly.c)
if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE Pyrowave::Runtime sunshine_windows_resources)
endif ()
# The network overview in the host sources uses the STUN codec on every platform.
target_link_libraries(${PROJECT_NAME} PRIVATE alkaidlab_stun_codec)
if (TARGET sunshine_rtx_video_adapter)
    add_dependencies(${PROJECT_NAME} sunshine_rtx_video_adapter)
    target_include_directories(${PROJECT_NAME} PRIVATE "${RTX_VIDEO_TRUST_INCLUDE}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE SUNSHINE_RTX_VIDEO_ADAPTER)
endif ()
if (TARGET sunshine_dlssnr_adapter)
    add_dependencies(${PROJECT_NAME} sunshine_dlssnr_adapter)
    target_include_directories(${PROJECT_NAME} PRIVATE "${DLSSNR_TRUST_INCLUDE}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE SUNSHINE_DLSSNR_ADAPTER)
endif ()

foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sunshine
endforeach()

set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
target_link_libraries(${PROJECT_NAME}
        PRIVATE
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest
        ${PLATFORM_LIBRARIES}
        sunshine_project_options sunshine_coverage sunshine_crypto sunshine_ai_provider sunshine_ai_stream)
target_compile_definitions(${PROJECT_NAME} PUBLIC
        SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        ${SUNSHINE_DEFINITIONS}
        ${TEST_DEFINITIONS})
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE)

# Bound hardware and network waits in the aggregate suite.
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
set_tests_properties(${PROJECT_NAME} PROPERTIES TIMEOUT 600)

add_executable(abr_unit_tests
        ${CMAKE_SOURCE_DIR}/src/streaming/abr.cpp
        ${CMAKE_SOURCE_DIR}/src/streaming/bitrate.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_abr_reported_loss.cpp
        ${ABR_TEST_STUB_SOURCE})
target_include_directories(abr_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(abr_unit_tests
        PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES}
        sunshine_project_options sunshine_coverage)
target_compile_definitions(abr_unit_tests PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS} SUNSHINE_ABR_TESTS)
target_compile_options(abr_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)
target_link_options(abr_unit_tests PRIVATE)
set_target_properties(abr_unit_tests PROPERTIES CXX_STANDARD 23)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(abr_unit_tests ${dep})  # compile these before abr_unit_tests
endforeach()
add_test(NAME abr_unit_tests COMMAND abr_unit_tests)

add_executable(input_activity_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_input_activity.cpp
        ${CMAKE_SOURCE_DIR}/src/input_activity.cpp
        ${CMAKE_SOURCE_DIR}/src/cursor_channel.cpp)
target_include_directories(input_activity_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/third-party")
target_link_libraries(input_activity_unit_tests
        PRIVATE gtest_main
        sunshine_project_options sunshine_coverage)
target_compile_definitions(input_activity_unit_tests PRIVATE INPUT_ACTIVITY_STANDALONE_TEST)
target_compile_options(input_activity_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(input_activity_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME input_activity_unit_tests COMMAND input_activity_unit_tests)

add_executable(launch_session_manager_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_rtsp.cpp
        ${CMAKE_SOURCE_DIR}/src/launch_session_manager.cpp)
target_include_directories(launch_session_manager_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(launch_session_manager_unit_tests
        PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES}
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_definitions(launch_session_manager_unit_tests PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(launch_session_manager_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(launch_session_manager_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME launch_session_manager_unit_tests COMMAND launch_session_manager_unit_tests)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(launch_session_manager_unit_tests ${dep})
endforeach()

add_executable(client_fingerprint_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_client_fingerprint.cpp
        ${CMAKE_SOURCE_DIR}/src/client_fingerprint.cpp)
target_include_directories(client_fingerprint_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(client_fingerprint_unit_tests
        PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES}
        sunshine_project_options sunshine_coverage sunshine_crypto)
target_compile_definitions(client_fingerprint_unit_tests PRIVATE
        ${SUNSHINE_DEFINITIONS}
        CLIENT_FINGERPRINT_STANDALONE_TEST)
target_compile_options(client_fingerprint_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(client_fingerprint_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME client_fingerprint_unit_tests COMMAND client_fingerprint_unit_tests)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(client_fingerprint_unit_tests ${dep})
endforeach()

add_executable(client_family_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_client_family.cpp
        ${CMAKE_SOURCE_DIR}/src/client_family.cpp)
target_include_directories(client_family_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(client_family_unit_tests
        PRIVATE gtest_main nlohmann_json::nlohmann_json
        sunshine_project_options sunshine_coverage)
target_compile_definitions(client_family_unit_tests PRIVATE
        CLIENT_FAMILY_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/client_families.json")
target_compile_options(client_family_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(client_family_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME client_family_unit_tests COMMAND client_family_unit_tests)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(client_family_unit_tests ${dep})
endforeach()

if (WIN32)
    add_executable(ds5_fake_sidecar
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_fake_sidecar.cpp)
    target_link_libraries(ds5_fake_sidecar PRIVATE
            sunshine_project_options sunshine_coverage)
    set_target_properties(ds5_fake_sidecar PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_executable(ds5_sidecar_client_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_ds5_sidecar_client.cpp
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_ds5_component_manifest.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_sidecar_test_stubs.cpp
            ${CMAKE_SOURCE_DIR}/src/ds5/config.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/ds5/ds5_sidecar_client.cpp)
    target_link_libraries(ds5_sidecar_client_unit_tests PRIVATE sunshine::host_headers
            sunshine_project_options sunshine_coverage)
    target_include_directories(ds5_sidecar_client_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(ds5_sidecar_client_unit_tests
            PRIVATE Boost::log
            Boost::format
            Boost::property_tree
            nlohmann_json::nlohmann_json
            gtest_main)
    target_compile_definitions(ds5_sidecar_client_unit_tests PRIVATE
            SUNSHINE_DS5_FAKE_SIDECAR_PATH="$<TARGET_FILE:ds5_fake_sidecar>"
            SUNSHINE_DS5_SIDECAR_TEST_HOOK)
    target_compile_options(ds5_sidecar_client_unit_tests PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(ds5_sidecar_client_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_dependencies(ds5_sidecar_client_unit_tests ds5_fake_sidecar)
    add_test(NAME ds5_sidecar_client_unit_tests COMMAND ds5_sidecar_client_unit_tests)

    add_executable(microphone_client_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_microphone_client.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_sidecar_test_stubs.cpp
            ${CMAKE_SOURCE_DIR}/src/ds5/config.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/ds5/ds5_sidecar_client.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_device_host/microphone_client.cpp)
    target_link_libraries(microphone_client_unit_tests PRIVATE sunshine::host_headers
            sunshine_project_options sunshine_coverage)
    target_include_directories(microphone_client_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(microphone_client_unit_tests
            PRIVATE Boost::log
            Boost::format
            Boost::property_tree
            nlohmann_json::nlohmann_json
            gtest_main)
    target_compile_definitions(microphone_client_unit_tests PRIVATE
            SUNSHINE_DS5_FAKE_SIDECAR_PATH="$<TARGET_FILE:ds5_fake_sidecar>"
            SUNSHINE_DS5_SIDECAR_TEST_HOOK)
    target_compile_options(microphone_client_unit_tests PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(microphone_client_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_dependencies(microphone_client_unit_tests ds5_fake_sidecar)
    add_test(NAME microphone_client_unit_tests COMMAND microphone_client_unit_tests)

    # prefer static libraries since we're linking statically
    # this fixes libcurl linking errors when using non MSYS2 version of CMake
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)
    set_target_properties(abr_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    set_target_properties(client_family_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
    set_target_properties(client_fingerprint_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()

if (WIN32)
    add_executable(vmouse_probe
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_probe.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_platform_support.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_mouse.cpp)
    target_include_directories(vmouse_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vmouse_probe
            PRIVATE ${SUNSHINE_EXTERNAL_LIBRARIES}
            ${PLATFORM_LIBRARIES}
            sunshine_project_options sunshine_coverage)
    target_compile_definitions(vmouse_probe PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS} SUNSHINE_VIRTUAL_MOUSE_STANDALONE_TEST)
    target_compile_options(vmouse_probe PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>
            -fno-profile-arcs
            -fno-test-coverage)
    target_link_options(vmouse_probe PRIVATE
            -fno-profile-arcs
            -fno-test-coverage)
    set_target_properties(vmouse_probe PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)

    add_executable(vmouse_send_diag
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_send_diag.cpp)
    target_include_directories(vmouse_send_diag PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vmouse_send_diag
            PRIVATE ${PLATFORM_LIBRARIES}
            sunshine_project_options sunshine_coverage)
    target_compile_definitions(vmouse_send_diag PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
    set_target_properties(vmouse_send_diag PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
endif ()
