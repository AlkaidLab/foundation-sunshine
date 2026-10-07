# Shared target definitions for the host suite and standalone validation.
get_filename_component(TRANSPORT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(sunshine_transport_validation LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(CTest)
    find_package(nlohmann_json 3.12 REQUIRED)
    add_subdirectory("${TRANSPORT_SOURCE_ROOT}/third-party/googletest" googletest EXCLUDE_FROM_ALL)
endif()

add_executable(transport_unit_tests
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_send_budget.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_send_budget.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_owner_inbox.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_owner_inbox.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_video_deadline.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_pacer.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_pacer.cpp"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/VideoNetwork.c"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/VideoPacketFeedback.c"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_video_packet_feedback.cpp"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/TransportFeedbackWire.c"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_feedback_wire.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_feedback_wire.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_policy.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_policy_notice.cpp"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/TransportPolicyStatus.c"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_policy_notice.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_policy_json.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_policy_json.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_policy.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_budget.cpp"
        "${TRANSPORT_SOURCE_ROOT}/src/transport/transport_feedback.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_budget.cpp"
        "${TRANSPORT_SOURCE_ROOT}/tests/unit/test_transport_feedback.cpp")
target_include_directories(transport_unit_tests PRIVATE "${TRANSPORT_SOURCE_ROOT}")
target_link_libraries(transport_unit_tests PRIVATE gtest_main nlohmann_json::nlohmann_json)
if(TARGET sunshine::googcc)
    target_link_libraries(transport_unit_tests PRIVATE sunshine::googcc)
endif()
set_target_properties(transport_unit_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    if(MSVC)
        target_compile_options(transport_unit_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(transport_unit_tests PRIVATE -Wall -Wextra -Werror)
    endif()
else()
    target_compile_options(transport_unit_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
endif()
add_test(NAME transport_unit_tests COMMAND transport_unit_tests)

add_executable(transport-feedback-wire-tests
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/TransportFeedbackWire.c"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/tests/TransportFeedbackWireTest.c")
target_include_directories(transport-feedback-wire-tests PRIVATE "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src")
set_target_properties(transport-feedback-wire-tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    if(MSVC)
        target_compile_options(transport-feedback-wire-tests PRIVATE /W4 /WX)
    else()
        target_compile_options(transport-feedback-wire-tests PRIVATE -Wall -Wextra -Werror)
    endif()
endif()
add_test(NAME transport-feedback-wire-tests COMMAND transport-feedback-wire-tests)

add_executable(video-network-observer-tests
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/VideoNetwork.c"
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/tests/VideoNetworkObserverTest.c")
target_include_directories(video-network-observer-tests PRIVATE
        "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src")
set_target_properties(video-network-observer-tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    if(MSVC)
        target_compile_options(video-network-observer-tests PRIVATE /W4 /WX)
    else()
        target_compile_options(video-network-observer-tests PRIVATE -Wall -Wextra -Werror)
    endif()
endif()
add_test(NAME video-network-observer-tests COMMAND video-network-observer-tests)

add_executable(video-packet-feedback-tests
        ${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/VideoNetwork.c
        ${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/VideoPacketFeedback.c
        ${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src/TransportFeedbackWire.c
        ${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/tests/VideoPacketFeedbackTest.c)
target_include_directories(video-packet-feedback-tests PRIVATE "${TRANSPORT_SOURCE_ROOT}/third-party/moonlight-common-c/src")
set_target_properties(video-packet-feedback-tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(video-packet-feedback-tests PRIVATE /W4 /WX)
else()
    target_compile_options(video-packet-feedback-tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME video-packet-feedback-tests COMMAND video-packet-feedback-tests)
