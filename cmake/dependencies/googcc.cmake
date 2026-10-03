# Shared production/validation interface to the actual pinned upstream target.
# Inclusion is inert; dependencies are loaded only by an explicit caller.
include_guard(GLOBAL)

function(sunshine_add_pinned_googcc build_upstream_tests)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Experimental GoogCC supports only the validated Windows x64 build; disable SUNSHINE_EXPERIMENTAL_GOOGCC")
    endif()
    if(NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
            NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "15.2.0")
        message(FATAL_ERROR "Experimental GoogCC is validated only with MinGW-w64 UCRT GCC 15.2.0; this toolchain is unsupported")
    endif()
    # Keep Abseil's feature probes and every upstream object in the same
    # language/ABI configuration as the validated controller build.
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(CheckCXXSourceCompiles)
    # Validate the actual CRT/architecture headers, not the compiler path name.
    unset(SUNSHINE_GOOGCC_VALIDATED_UCRT_X64 CACHE)
    check_cxx_source_compiles("#include <crtdefs.h>
#if !defined(_UCRT) || !defined(__MINGW64__) || !defined(__x86_64__) || !defined(_WIN64)
#error GoogCC has only been validated with Windows x64 MinGW-w64 UCRT
#endif
int main() { return 0; }" SUNSHINE_GOOGCC_VALIDATED_UCRT_X64)
    if(NOT SUNSHINE_GOOGCC_VALIDATED_UCRT_X64)
        message(FATAL_ERROR "Experimental GoogCC requires the validated Windows x64 MinGW-w64 UCRT headers/runtime")
    endif()

    get_filename_component(googcc_source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(googcc_recipe "${googcc_source_root}/third-party/webrtc-googcc")
    if(TARGET webrtc_googcc)
        get_target_property(existing_recipe webrtc_googcc SOURCE_DIR)
        if(NOT existing_recipe STREQUAL googcc_recipe)
            message(FATAL_ERROR "webrtc_googcc must come from Sunshine's pinned dependency recipe")
        endif()
    else()
        # Production passes FALSE, independently of main BUILD_TESTING/BUILD_TESTS.
        # Validation callers may request the pinned upstream tests explicitly.
        set(SUNSHINE_GOOGCC_BUILD_UPSTREAM_TESTS "${build_upstream_tests}")
        add_subdirectory("${googcc_recipe}" "${CMAKE_BINARY_DIR}/googcc")
    endif()

    if(NOT TARGET sunshine_googcc_build_interface)
        # Keep upstream include paths/Windows macros inside this translation
        # boundary. An INTERFACE source+link would leak WIN32_LEAN_AND_MEAN and
        # WebRTC usage requirements into capture, input and every host source.
        # The adapter exposes only a PImpl header; static PRIVATE dependencies
        # remain link dependencies without becoming host compile requirements.
        add_library(sunshine_googcc_build_interface STATIC
                "${googcc_source_root}/src/googcc_runtime.cpp"
                "${googcc_source_root}/src/googcc_runtime.h"
                "${googcc_source_root}/src/googcc_adapter.cpp"
                "${googcc_source_root}/src/googcc_adapter.h")
        add_library(sunshine::googcc ALIAS sunshine_googcc_build_interface)
        target_include_directories(sunshine_googcc_build_interface PUBLIC "${googcc_source_root}")
        target_compile_features(sunshine_googcc_build_interface PUBLIC cxx_std_23)
        target_compile_definitions(sunshine_googcc_build_interface PUBLIC SUNSHINE_HAS_GOOGCC=1)
        target_link_libraries(sunshine_googcc_build_interface PRIVATE webrtc_googcc)
    endif()
endfunction()
