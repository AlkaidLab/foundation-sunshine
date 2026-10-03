cmake_minimum_required(VERSION 3.20)
project(sunshine_googcc_validation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
get_filename_component(SUNSHINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SUNSHINE_ROOT}/cmake/dependencies/googcc.cmake")
sunshine_add_pinned_googcc(TRUE)
add_executable(googcc_input_tests
    "${SUNSHINE_ROOT}/src/transport/transport_feedback.cpp"
    "${SUNSHINE_ROOT}/src/transport/transport_feedback_wire.cpp"
    "${SUNSHINE_ROOT}/third-party/moonlight-common-c/src/TransportFeedbackWire.c"
    "${SUNSHINE_ROOT}/src/transport/transport_fec_controller.cpp"
    "${SUNSHINE_ROOT}/src/transport/transport_policy.cpp"
    "${SUNSHINE_ROOT}/src/transport/transport_budget.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/test_googcc_runtime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/test_googcc_input.cpp")
target_include_directories(googcc_input_tests PRIVATE "${SUNSHINE_ROOT}")
target_link_libraries(googcc_input_tests PRIVATE sunshine::googcc gtest_main)
if(MSVC)
    target_compile_options(googcc_input_tests PRIVATE /W4 /WX)
else()
    target_compile_options(googcc_input_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME googcc_input_tests COMMAND googcc_input_tests --gtest_output=xml:googcc-input-results.xml)
add_executable(googcc_wire_tests
    "${SUNSHINE_ROOT}/src/transport/transport_feedback.cpp"
    "${SUNSHINE_ROOT}/src/transport/transport_feedback_wire.cpp"
    "${SUNSHINE_ROOT}/third-party/moonlight-common-c/src/TransportFeedbackWire.c"
    "${SUNSHINE_ROOT}/tests/unit/test_transport_feedback_wire.cpp")
target_include_directories(googcc_wire_tests PRIVATE "${SUNSHINE_ROOT}")
target_link_libraries(googcc_wire_tests PRIVATE gtest_main)
if(MSVC)
    target_compile_options(googcc_wire_tests PRIVATE /W4 /WX)
else()
    target_compile_options(googcc_wire_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME googcc_wire_tests COMMAND googcc_wire_tests --gtest_output=xml:googcc-wire-results.xml)
