include_guard(GLOBAL)

# Require the selected standard rather than silently accepting an older dialect.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SUNSHINE_TESTS_ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)

add_library(sunshine_project_options INTERFACE)
target_compile_features(sunshine_project_options INTERFACE cxx_std_23)
if(MINGW)
    target_link_options(sunshine_project_options INTERFACE -static)
    target_compile_options(sunshine_project_options INTERFACE $<$<COMPILE_LANGUAGE:RC>:--use-temp-file>)
endif()

add_library(sunshine_coverage INTERFACE)
if(SUNSHINE_TESTS_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "SUNSHINE_TESTS_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    target_compile_options(sunshine_coverage INTERFACE
            $<$<COMPILE_LANGUAGE:C,CXX>:--coverage;-g;-O0>)
    target_link_options(sunshine_coverage INTERFACE --coverage)
    if(MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Instrumented Beast templates can exceed the standard COFF section limit.
        target_compile_options(sunshine_coverage INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wa,-mbig-obj>)
    endif()
endif()
