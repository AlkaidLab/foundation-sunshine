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

if (WIN32)
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

set(ABR_TEST_STUB_SOURCE ${CMAKE_SOURCE_DIR}/tests/unit/abr_test_stubs.cpp)
list(REMOVE_ITEM TEST_SOURCES ${ABR_TEST_STUB_SOURCE})

set(SUNSHINE_SOURCES
        ${SUNSHINE_TARGET_FILES})

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
target_compile_definitions(${PROJECT_NAME} PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
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

