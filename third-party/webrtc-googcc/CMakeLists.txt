cmake_minimum_required(VERSION 3.20)

# Pinned Windows dependency shared by production opt-in and isolated validation.
# Adding this target does not select a live transport/control mode.
if(NOT WIN32)
    message(FATAL_ERROR "This GoogCC dependency recipe currently validates Windows only")
endif()
set(SUNSHINE_GOOGCC_DEPS_DIR "" CACHE PATH "Pinned GoogCC checkouts produced by bootstrap.py")
if(NOT SUNSHINE_GOOGCC_DEPS_DIR)
    message(FATAL_ERROR "Set SUNSHINE_GOOGCC_DEPS_DIR after running third-party/webrtc-googcc/bootstrap.py")
endif()
find_package(Git REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
file(READ "${CMAKE_CURRENT_LIST_DIR}/versions.json" GOOGCC_VERSIONS)
foreach(DEPENDENCY webrtc chromium-third-party googletest)
    string(JSON EXPECTED_REVISION GET "${GOOGCC_VERSIONS}" "${DEPENDENCY}" revision)
    string(JSON DEPENDENCY_DIRECTORY GET "${GOOGCC_VERSIONS}" "${DEPENDENCY}" directory)
    set(CHECKOUT "${SUNSHINE_GOOGCC_DEPS_DIR}/${DEPENDENCY_DIRECTORY}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" rev-parse HEAD
        OUTPUT_VARIABLE ACTUAL_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT ACTUAL_REVISION STREQUAL EXPECTED_REVISION)
        message(FATAL_ERROR "${DEPENDENCY} revision differs from versions.json")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" diff --exit-code HEAD
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHECKOUT}" status --porcelain
        OUTPUT_VARIABLE CHECKOUT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT CHECKOUT_STATUS STREQUAL "")
        message(FATAL_ERROR "${DEPENDENCY} checkout must be pristine, including untracked files; use bootstrap.py")
    endif()
endforeach()

set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
add_subdirectory("${SUNSHINE_GOOGCC_DEPS_DIR}/chromium-third-party/abseil-cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/abseil" EXCLUDE_FROM_ALL)
set(WEBRTC_ROOT "${SUNSHINE_GOOGCC_DEPS_DIR}/webrtc")
set(GOOGCC_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GOOGCC_GENERATED}/experiments")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${WEBRTC_ROOT}/experiments/field_trials.py"
    header --output "${GOOGCC_GENERATED}/experiments/registered_field_trials.h.tmp"
    COMMAND_ERROR_IS_FATAL ANY)
configure_file("${GOOGCC_GENERATED}/experiments/registered_field_trials.h.tmp"
    "${GOOGCC_GENERATED}/experiments/registered_field_trials.h" COPYONLY)
file(REMOVE "${GOOGCC_GENERATED}/experiments/registered_field_trials.h.tmp")
function(googcc_write_generated PATH CONTENT)
    file(WRITE "${PATH}.tmp" "${CONTENT}")
    configure_file("${PATH}.tmp" "${PATH}" COPYONLY)
    file(REMOVE "${PATH}.tmp")
endfunction()

set(GOOGCC_ALGORITHMS
    acknowledged_bitrate_estimator.cc
    acknowledged_bitrate_estimator_interface.cc
    alr_detector.cc
    bitrate_estimator.cc
    congestion_window_pushback_controller.cc
    delay_based_bwe.cc
    goog_cc_network_control.cc
    inter_arrival_delta.cc
    link_capacity_estimator.cc
    loss_based_bwe_v2.cc
    probe_bitrate_estimator.cc
    probe_controller.cc
    robust_throughput_estimator.cc
    send_side_bandwidth_estimation.cc
    trendline_estimator.cc)
list(TRANSFORM GOOGCC_ALGORITHMS PREPEND "${WEBRTC_ROOT}/modules/congestion_controller/goog_cc/")
set(GOOGCC_SUPPORT
    api/environment/deprecated_global_field_trials.cc
    api/environment/environment_factory.cc
    api/environment/force_test_environment.cc
    api/field_trials.cc
    api/field_trials_registry.cc
    api/rtc_event_log/rtc_event_log.cc
    api/task_queue/task_queue_base.cc
    api/task_queue/default_task_queue_factory_win.cc
    api/transport/network_types.cc
    api/units/data_rate.cc
    api/units/data_size.cc
    api/units/frequency.cc
    api/units/time_delta.cc
    api/units/timestamp.cc
    modules/pacing/interval_budget.cc
    modules/remote_bitrate_estimator/aimd_rate_control.cc
    modules/remote_bitrate_estimator/bwe_defines.cc
    rtc_base/checks.cc
    rtc_base/bitstream_reader.cc
    rtc_base/event.cc
    rtc_base/logging.cc
    rtc_base/platform_thread.cc
    rtc_base/platform_thread_types.cc
    rtc_base/race_checker.cc
    rtc_base/ref_counter.cc
    rtc_base/string_encode.cc
    rtc_base/string_utils.cc
    rtc_base/strings/string_builder.cc
    rtc_base/synchronization/yield_policy.cc
    rtc_base/system_time.cc
    rtc_base/task_queue_win.cc
    rtc_base/time_utils.cc
    rtc_base/experiments/alr_experiment.cc
    rtc_base/experiments/field_trial_parser.cc
    rtc_base/experiments/field_trial_list.cc
    rtc_base/experiments/field_trial_units.cc
    rtc_base/experiments/struct_parameters_parser.cc
    rtc_base/experiments/rate_control_settings.cc
    system_wrappers/source/clock.cc
    system_wrappers/source/metrics.cc
    logging/rtc_event_log/events/rtc_event_field_encoding.cc
    logging/rtc_event_log/events/rtc_event_field_encoding_parser.cc
    logging/rtc_event_log/events/rtc_event_field_extraction.cc
    logging/rtc_event_log/events/fixed_length_encoding_parameters_v3.cc
    logging/rtc_event_log/encoder/rtc_event_log_encoder_common.cc
    logging/rtc_event_log/encoder/var_int.cc
    logging/rtc_event_log/events/rtc_event_alr_state.cc
    logging/rtc_event_log/events/rtc_event_bwe_update_delay_based.cc
    logging/rtc_event_log/events/rtc_event_bwe_update_loss_based.cc
    logging/rtc_event_log/events/rtc_event_probe_cluster_created.cc
    logging/rtc_event_log/events/rtc_event_probe_result_success.cc
    logging/rtc_event_log/events/rtc_event_probe_result_failure.cc)

if(MINGW)
    # Keep the pinned checkout pristine. GCC cannot compile MSVC's legacy SEH
    # debugger-name fallback; the preceding SetThreadDescription remains intact.
    file(READ "${WEBRTC_ROOT}/rtc_base/platform_thread_types.cc" THREAD_NAME_SOURCE)
    string(REPLACE "\r\n" "\n" THREAD_NAME_SOURCE "${THREAD_NAME_SOURCE}")
    string(SHA256 THREAD_NAME_HASH "${THREAD_NAME_SOURCE}")
    string(JSON EXPECTED_THREAD_NAME_HASH GET "${GOOGCC_VERSIONS}" webrtc thread_name_source_lf_sha256)
    if(NOT THREAD_NAME_HASH STREQUAL EXPECTED_THREAD_NAME_HASH)
        message(FATAL_ERROR "Thread-name compatibility source differs from the pinned original")
    endif()
    string(REPLACE "#pragma pack(push, 8)" "#if defined(_MSC_VER)\n#pragma pack(push, 8)"
        THREAD_NAME_SOURCE "${THREAD_NAME_SOURCE}")
    string(REPLACE "#pragma warning(pop)" "#pragma warning(pop)\n#endif  // defined(_MSC_VER)"
        THREAD_NAME_SOURCE "${THREAD_NAME_SOURCE}")
    googcc_write_generated("${GOOGCC_GENERATED}/platform_thread_types_mingw.cc" "${THREAD_NAME_SOURCE}")
    list(REMOVE_ITEM GOOGCC_SUPPORT rtc_base/platform_thread_types.cc)
    list(APPEND GOOGCC_ALGORITHMS "${GOOGCC_GENERATED}/platform_thread_types_mingw.cc")
endif()
list(TRANSFORM GOOGCC_SUPPORT PREPEND "${WEBRTC_ROOT}/")
add_library(webrtc_googcc STATIC ${GOOGCC_ALGORITHMS} ${GOOGCC_SUPPORT})
target_compile_features(webrtc_googcc PUBLIC cxx_std_23)
target_include_directories(webrtc_googcc SYSTEM PUBLIC "${WEBRTC_ROOT}" "${GOOGCC_GENERATED}")
target_compile_definitions(webrtc_googcc PUBLIC WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN
    RTC_DISABLE_LOGGING WEBRTC_STRICT_FIELD_TRIALS=1)
target_link_libraries(webrtc_googcc PUBLIC absl::strings absl::any_invocable absl::algorithm_container
    winmm ws2_32)

if(NOT DEFINED SUNSHINE_GOOGCC_BUILD_UPSTREAM_TESTS)
    set(SUNSHINE_GOOGCC_BUILD_UPSTREAM_TESTS "${BUILD_TESTING}")
endif()
if(SUNSHINE_GOOGCC_BUILD_UPSTREAM_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory("${SUNSHINE_GOOGCC_DEPS_DIR}/googletest" "${CMAKE_CURRENT_BINARY_DIR}/gtest" EXCLUDE_FROM_ALL)
    # Include-layout forwarders; all test implementation comes from pinned GTest.
    file(MAKE_DIRECTORY "${GOOGCC_GENERATED}/testing/gtest/include/gtest"
        "${GOOGCC_GENERATED}/testing/gmock/include/gmock")
    googcc_write_generated("${GOOGCC_GENERATED}/testing/gtest/include/gtest/gtest.h" "#include <gtest/gtest.h>\n")
    googcc_write_generated("${GOOGCC_GENERATED}/testing/gtest/include/gtest/gtest-spi.h" "#include <gtest/gtest-spi.h>\n")
    googcc_write_generated("${GOOGCC_GENERATED}/testing/gmock/include/gmock/gmock.h" "#include <gmock/gmock.h>\n")
    set(UPSTREAM_TESTS
        acknowledged_bitrate_estimator_unittest.cc
        robust_throughput_estimator_unittest.cc
        trendline_estimator_unittest.cc
        loss_based_bwe_v2_test.cc
        congestion_window_pushback_controller_unittest.cc
        probe_bitrate_estimator_unittest.cc
        probe_controller_unittest.cc
        send_side_bandwidth_estimation_unittest.cc
        delay_based_bwe_unittest.cc
        delay_based_bwe_unittest_helper.cc
        alr_detector_unittest.cc)
    list(TRANSFORM UPSTREAM_TESTS PREPEND "${WEBRTC_ROOT}/modules/congestion_controller/goog_cc/")
    add_executable(googcc_upstream_tests ${UPSTREAM_TESTS}
        "${WEBRTC_ROOT}/test/create_test_field_trials.cc"
        "${WEBRTC_ROOT}/logging/rtc_event_log/mock/mock_rtc_event_log.cc")
    target_link_libraries(googcc_upstream_tests PRIVATE webrtc_googcc gmock_main absl::flags)
    add_test(NAME googcc_upstream_tests COMMAND googcc_upstream_tests --gtest_output=xml:googcc-upstream-results.xml)
endif()
