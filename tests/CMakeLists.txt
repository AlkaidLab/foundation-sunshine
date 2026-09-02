cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sunshine)

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
option(SUNSHINE_TESTS_ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)
if (SUNSHINE_TESTS_ENABLE_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -ggdb -O0")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -ggdb -O0")
endif ()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

add_executable(tray_state_unit_tests
        ${CMAKE_SOURCE_DIR}/src/tray/tray_state.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_http_util.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_tray_state.cpp)
target_include_directories(tray_state_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(tray_state_unit_tests
        nlohmann_json::nlohmann_json
        gtest_main)
target_compile_options(tray_state_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(tray_state_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(tray_state_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME tray_state_unit_tests COMMAND tray_state_unit_tests)

add_executable(sunshinesvc_state_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_sunshinesvc_state.cpp)
target_include_directories(sunshinesvc_state_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(sunshinesvc_state_unit_tests gtest_main)
target_compile_options(sunshinesvc_state_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(sunshinesvc_state_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(sunshinesvc_state_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME sunshinesvc_state_unit_tests COMMAND sunshinesvc_state_unit_tests)

add_executable(video_probe_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_video_probe.cpp)
target_include_directories(video_probe_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(video_probe_unit_tests gtest_main)
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
target_link_libraries(frame_contract_unit_tests gtest_main)
target_compile_options(frame_contract_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(frame_contract_unit_tests PROPERTIES CXX_STANDARD 23)
if (WIN32)
    set_target_properties(frame_contract_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()
add_test(NAME frame_contract_unit_tests COMMAND frame_contract_unit_tests)

if (WIN32)
    set(PRE_ENCODE_FILTER_TEST_SOURCE
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_pre_encode_filter.cpp)
    set(TRUEHDR_BACKEND_LOADER_TEST_SOURCE
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_truehdr_backend_loader.cpp)

    add_executable(pre_encode_filter_unit_tests
            ${CMAKE_SOURCE_DIR}/src/platform/windows/pre_encode_filter.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/rtx_hdr/backend_loader.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/pre_encode_filter_test_stubs.cpp
            ${PRE_ENCODE_FILTER_TEST_SOURCE})
    target_include_directories(pre_encode_filter_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(pre_encode_filter_unit_tests
            Boost::log
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

    add_library(fake_truehdr_backend SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_backend.cpp)
    target_include_directories(fake_truehdr_backend PRIVATE "${CMAKE_SOURCE_DIR}")
    set_target_properties(fake_truehdr_backend PROPERTIES
            CXX_STANDARD 23
            PREFIX "")

    add_library(fake_truehdr_bad_backend SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_backend.cpp)
    target_include_directories(fake_truehdr_bad_backend PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(fake_truehdr_bad_backend PRIVATE FAKE_TRUEHDR_BAD_ABI)
    set_target_properties(fake_truehdr_bad_backend PROPERTIES
            CXX_STANDARD 23
            PREFIX "")

    add_library(fake_truehdr_failing_backend SHARED
            ${CMAKE_SOURCE_DIR}/tests/tools/fake_truehdr_backend.cpp)
    target_include_directories(fake_truehdr_failing_backend PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(fake_truehdr_failing_backend PRIVATE FAKE_TRUEHDR_PROCESS_FAILS)
    set_target_properties(fake_truehdr_failing_backend PROPERTIES
            CXX_STANDARD 23
            PREFIX "")

    add_executable(truehdr_backend_loader_unit_tests
            ${CMAKE_SOURCE_DIR}/src/platform/windows/rtx_hdr/backend_loader.cpp
            ${TRUEHDR_BACKEND_LOADER_TEST_SOURCE})
    target_include_directories(truehdr_backend_loader_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(truehdr_backend_loader_unit_tests gtest_main)
    target_compile_definitions(truehdr_backend_loader_unit_tests PRIVATE
            FAKE_TRUEHDR_BACKEND_PATH="$<TARGET_FILE:fake_truehdr_backend>"
            FAKE_TRUEHDR_BAD_BACKEND_PATH="$<TARGET_FILE:fake_truehdr_bad_backend>")
    target_compile_options(truehdr_backend_loader_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    set_target_properties(truehdr_backend_loader_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_dependencies(truehdr_backend_loader_unit_tests
            fake_truehdr_backend
            fake_truehdr_bad_backend)
    add_dependencies(pre_encode_filter_unit_tests
            fake_truehdr_backend
            fake_truehdr_failing_backend)
    target_compile_definitions(pre_encode_filter_unit_tests PRIVATE
            FAKE_TRUEHDR_BACKEND_PATH="$<TARGET_FILE:fake_truehdr_backend>"
            FAKE_TRUEHDR_FAILING_BACKEND_PATH="$<TARGET_FILE:fake_truehdr_failing_backend>")
    add_test(NAME truehdr_backend_loader_unit_tests COMMAND truehdr_backend_loader_unit_tests)

    add_executable(vdd_prerequisite_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/test_vdd_prerequisite.cpp)
    target_include_directories(vdd_prerequisite_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vdd_prerequisite_unit_tests
            Boost::optional
            gtest_main)
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
            advapi32
            crypt32
            gtest_main)
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
set(REMOTE_USB_TLS_PLATFORM_LIBRARIES)
if (WIN32)
    list(APPEND REMOTE_USB_SOCKET_LIBRARIES mswsock ws2_32)
    list(APPEND REMOTE_USB_TLS_PLATFORM_LIBRARIES crypt32)
endif ()

set(RUSB_VECTOR_JSON
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-remote-usb-core/contract/vectors-v1.json")
if (NOT EXISTS "${RUSB_VECTOR_JSON}")
    message(FATAL_ERROR
            "Missing ${RUSB_VECTOR_JSON}. Run: git submodule update --init --recursive")
endif ()

add_executable(loopback_usbip_bridge_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_loopback_usbip_bridge.cpp)
target_include_directories(loopback_usbip_bridge_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(loopback_usbip_bridge_unit_tests
        Boost::asio
        Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        gtest_main)
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
        Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        gtest_main)
target_compile_options(virtual_touchscreen_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(virtual_touchscreen_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME virtual_touchscreen_unit_tests COMMAND virtual_touchscreen_unit_tests)

add_executable(remote_usb_broker_adapter_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_adapter.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_broker_adapter.cpp)
target_include_directories(remote_usb_broker_adapter_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_broker_adapter_unit_tests
        Boost::asio
        Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_broker_adapter_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_broker_adapter_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_broker_adapter_unit_tests COMMAND remote_usb_broker_adapter_unit_tests)

add_executable(remote_usb_broker_server_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_server.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_capability.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_broker_server.cpp)
target_include_directories(remote_usb_broker_server_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_broker_server_unit_tests
        Boost::asio
        Boost::system
        ${OPENSSL_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        ${REMOTE_USB_TLS_PLATFORM_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_broker_server_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_broker_server_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_broker_server_unit_tests COMMAND remote_usb_broker_server_unit_tests)

add_executable(remote_usb_contract_vectors_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_server.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_capability.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_contract_vectors.cpp)
target_include_directories(remote_usb_contract_vectors_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_compile_definitions(remote_usb_contract_vectors_unit_tests PRIVATE
        RUSB_VECTOR_FILE="${RUSB_VECTOR_JSON}")
target_link_libraries(remote_usb_contract_vectors_unit_tests
        Boost::asio
        Boost::system
        ${OPENSSL_LIBRARIES}
        nlohmann_json::nlohmann_json
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        ${REMOTE_USB_TLS_PLATFORM_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_contract_vectors_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_contract_vectors_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_contract_vectors_unit_tests COMMAND remote_usb_contract_vectors_unit_tests)

add_executable(remote_usb_capability_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_capability.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_http.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_capability.cpp)
target_include_directories(remote_usb_capability_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_capability_unit_tests
        Boost::asio
        Boost::system
        ${OPENSSL_LIBRARIES}
        nlohmann_json::nlohmann_json
        ${REMOTE_USB_SOCKET_LIBRARIES}
        ${REMOTE_USB_TLS_PLATFORM_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_capability_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_capability_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_capability_unit_tests COMMAND remote_usb_capability_unit_tests)

add_executable(remote_usb_tombstone_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_tombstone.cpp)
target_include_directories(remote_usb_tombstone_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_tombstone_unit_tests gtest_main)
target_compile_options(remote_usb_tombstone_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_tombstone_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_tombstone_unit_tests COMMAND remote_usb_tombstone_unit_tests)

add_executable(remote_usb_host_controller_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_host_controller.cpp)
target_include_directories(remote_usb_host_controller_unit_tests PRIVATE
        "${CMAKE_SOURCE_DIR}")
target_link_libraries(remote_usb_host_controller_unit_tests
        Boost::asio
        Boost::process
        Boost::system
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_host_controller_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_host_controller_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_host_controller_unit_tests COMMAND remote_usb_host_controller_unit_tests)

add_executable(remote_usb_service_unit_tests
        ${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_adapter.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_broker_server.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_capability.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_host_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/remote_usb/remote_usb_service.cpp
        ${CMAKE_SOURCE_DIR}/tests/unit/test_remote_usb_service.cpp)
target_include_directories(remote_usb_service_unit_tests PRIVATE
        # Avoid adding src as a broad include root: on MinGW it can shadow the
        # system <process.h> with Sunshine's src/process.h through pthread.h.
        "${CMAKE_SOURCE_DIR}"
        "${OPENSSL_INCLUDE_DIR}")
target_link_libraries(remote_usb_service_unit_tests
        Boost::asio
        Boost::process
        Boost::system
        ${OPENSSL_LIBRARIES}
        nlohmann_json::nlohmann_json
        ${CMAKE_THREAD_LIBS_INIT}
        ${REMOTE_USB_SOCKET_LIBRARIES}
        ${REMOTE_USB_TLS_PLATFORM_LIBRARIES}
        gtest_main)
target_compile_options(remote_usb_service_unit_tests PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(remote_usb_service_unit_tests PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
add_test(NAME remote_usb_service_unit_tests COMMAND remote_usb_service_unit_tests)

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

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)

# Standalone diagnostic programs under tests/tools have their own targets and
# must not be linked into the aggregate GoogleTest executable.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/tests/tools/")
# The touchscreen device test has a dedicated target that links the device
# implementation; do not compile a second copy into the aggregate suite.
list(FILTER TEST_SOURCES EXCLUDE REGEX "test_virtual_touchscreen_device\.cpp")

set(ABR_TEST_STUB_SOURCE ${CMAKE_SOURCE_DIR}/tests/unit/abr_test_stubs.cpp)
list(REMOVE_ITEM TEST_SOURCES ${ABR_TEST_STUB_SOURCE})
if (WIN32)
    # These tests depend on generated fake backend paths and run in dedicated
    # targets above; do not compile a second copy into the aggregate suite.
    list(REMOVE_ITEM TEST_SOURCES
            ${PRE_ENCODE_FILTER_TEST_SOURCE}
            ${TRUEHDR_BACKEND_LOADER_TEST_SOURCE})
endif ()

set(SUNSHINE_SOURCES
        ${SUNSHINE_TARGET_FILES}
        ${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Ds5HapticsIrStream.c)

# remove main.cpp from the list of sources
list(REMOVE_ITEM SUNSHINE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
        ${SUNSHINE_SOURCES})

foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sunshine
endforeach()

set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
target_link_libraries(${PROJECT_NAME}
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest
        ${PLATFORM_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC
        ${SUNSHINE_DEFINITIONS}
        ${TEST_DEFINITIONS})
target_compile_definitions(${PROJECT_NAME} PRIVATE
        RUSB_VECTOR_FILE="${RUSB_VECTOR_JSON}")
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE)

# The aggregate suite had an executable but no ctest registration, so every
# assertion globbed into it was dead weight. The timeout is a guard, not a
# target: nothing in here has ever run on a headless runner, so a test that
# waits on a display or a socket should fail the job rather than hang it.
#
# DownloadFileTests fetches from httpbin.org, so as a gate it reports on a third
# party's uptime rather than on this repository. It stays in the binary — run it
# deliberately with --gtest_filter when changing http::download_file — but it
# does not get a vote on whether a pull request is mergeable.
add_test(NAME ${PROJECT_NAME}
        COMMAND ${PROJECT_NAME} --gtest_filter=-DownloadFileTests/*)
set_tests_properties(${PROJECT_NAME} PROPERTIES TIMEOUT 600)

add_executable(abr_unit_tests
        ${CMAKE_SOURCE_DIR}/src/abr.cpp
        ${ABR_TEST_STUB_SOURCE})
target_include_directories(abr_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(abr_unit_tests
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES})
target_compile_definitions(abr_unit_tests PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
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
        gtest_main)
target_compile_definitions(input_activity_unit_tests PRIVATE INPUT_ACTIVITY_STANDALONE_TEST)
target_compile_options(input_activity_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(input_activity_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME input_activity_unit_tests COMMAND input_activity_unit_tests)

add_executable(launch_session_manager_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_rtsp.cpp
        ${CMAKE_SOURCE_DIR}/src/launch_session_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/crypto.cpp)
target_include_directories(launch_session_manager_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(launch_session_manager_unit_tests
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES})
target_compile_definitions(launch_session_manager_unit_tests PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(launch_session_manager_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(launch_session_manager_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME launch_session_manager_unit_tests COMMAND launch_session_manager_unit_tests)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(launch_session_manager_unit_tests ${dep})
endforeach()

add_executable(client_fingerprint_unit_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/test_client_fingerprint.cpp
        ${CMAKE_SOURCE_DIR}/src/client_fingerprint.cpp
        ${CMAKE_SOURCE_DIR}/src/crypto.cpp)
target_include_directories(client_fingerprint_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(client_fingerprint_unit_tests
        ${SUNSHINE_EXTERNAL_LIBRARIES}
        gtest_main
        ${PLATFORM_LIBRARIES})
target_compile_definitions(client_fingerprint_unit_tests PRIVATE
        ${SUNSHINE_DEFINITIONS}
        CLIENT_FINGERPRINT_STANDALONE_TEST)
target_compile_options(client_fingerprint_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(client_fingerprint_unit_tests PROPERTIES CXX_STANDARD 23)
add_test(NAME client_fingerprint_unit_tests COMMAND client_fingerprint_unit_tests)
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(client_fingerprint_unit_tests ${dep})
endforeach()

if (WIN32)
    add_executable(ds5_fake_sidecar
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_fake_sidecar.cpp)
    set_target_properties(ds5_fake_sidecar PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
    add_dependencies(${PROJECT_NAME} ds5_fake_sidecar)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            SUNSHINE_DS5_FAKE_SIDECAR_PATH="$<TARGET_FILE:ds5_fake_sidecar>"
            SUNSHINE_DS5_SIDECAR_TEST_HOOK)

    add_executable(ds5_sidecar_client_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_ds5_sidecar_client.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/ds5_sidecar_test_stubs.cpp
            ${CMAKE_SOURCE_DIR}/src/ds5/config.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/ds5/ds5_sidecar_client.cpp)
    target_include_directories(ds5_sidecar_client_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(ds5_sidecar_client_unit_tests
            Boost::log
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
    target_include_directories(microphone_client_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(microphone_client_unit_tests
            Boost::log
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
    set_target_properties(client_fingerprint_unit_tests PROPERTIES LINK_SEARCH_START_STATIC 1)
endif ()

if (WIN32)
    add_executable(vmouse_unit_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/platform/windows/test_virtual_mouse.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_platform_support.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_mouse.cpp)
    target_include_directories(vmouse_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vmouse_unit_tests
            ${SUNSHINE_EXTERNAL_LIBRARIES}
            gtest_main
            ${PLATFORM_LIBRARIES})
    target_compile_definitions(vmouse_unit_tests PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS} SUNSHINE_VIRTUAL_MOUSE_STANDALONE_TEST)
    target_compile_options(vmouse_unit_tests PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>
            -fno-profile-arcs
            -fno-test-coverage)
    target_link_options(vmouse_unit_tests PRIVATE
            -fno-profile-arcs
            -fno-test-coverage)
    set_target_properties(vmouse_unit_tests PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)

    add_executable(vmouse_probe
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_probe.cpp
            ${CMAKE_SOURCE_DIR}/tests/tools/vmouse_platform_support.cpp
            ${CMAKE_SOURCE_DIR}/src/platform/windows/virtual_mouse.cpp)
    target_include_directories(vmouse_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vmouse_probe
            ${SUNSHINE_EXTERNAL_LIBRARIES}
            ${PLATFORM_LIBRARIES})
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
            ${PLATFORM_LIBRARIES})
    target_compile_definitions(vmouse_send_diag PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
    set_target_properties(vmouse_send_diag PROPERTIES
            CXX_STANDARD 23
            LINK_SEARCH_START_STATIC 1)
endif ()
