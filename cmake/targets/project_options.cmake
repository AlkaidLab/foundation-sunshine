include_guard(GLOBAL)

# Require the selected standard rather than silently accepting an older dialect.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SUNSHINE_TESTS_ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)

# The GCC build is the fallback toolchain, not the release toolchain. Its -O3 code on this tree is larger
# than Clang's; -Os keeps the -O2 optimizations except the ones that often increase code size (GCC 16.2
# manual, Optimize Options, source gcc-optimize-options-16.2.0).
function(sunshine_gcc_release_size_flag out_var compiler_id)
    if(compiler_id STREQUAL "GNU")
        set(${out_var} "-Os" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# GCC 16.2 at -Os keeps a call to basic_string's C4 (base-object) move constructor; the pinned
# libstdc++.a exports only the C1 (complete-object) form. basic_string has no virtual bases, so
# both forms are the same constructor; alias C4 to C1 for every GNU link.
function(sunshine_gcc_string_constructor_aliases out_var compiler_id)
    if(compiler_id STREQUAL "GNU")
        set(${out_var}
                "-Wl,--defsym,_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC4EOS4_=_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EOS4_"
                "-Wl,--defsym,_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC4EOS4_=_ZNSt7__cxx1112basic_stringIwSt11char_traitsIwESaIwEEC1EOS4_"
                PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

add_library(sunshine_project_options INTERFACE)
target_compile_features(sunshine_project_options INTERFACE cxx_std_23)
if(MINGW)
    target_link_options(sunshine_project_options INTERFACE -static)
    target_compile_options(sunshine_project_options INTERFACE $<$<COMPILE_LANGUAGE:RC>:--use-temp-file>)
    # fs-gnu-ld-gc-sections-20261116: discard unreferenced Release sections after
    # each function/data item is emitted separately.
    target_compile_options(sunshine_project_options INTERFACE
            "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
    target_link_options(sunshine_project_options INTERFACE
            $<$<CONFIG:Release>:LINKER:--gc-sections>)
    # fs-llvm-clang-cli-20261116 and fs-llvm-lld-20261116: Clang's MinGW
    # release executable keeps the same 2 MiB reserve as the GCC baseline.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(sunshine_project_options INTERFACE -Wl,--stack,2097152)
    endif()
endif()

# Keep per-configuration source policy in one place for both host variants.
function(sunshine_configure_debug_sources test_directory)
    if(WIN32)
        set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                DIRECTORY "${CMAKE_SOURCE_DIR}" "${test_directory}"
                PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
    endif()
endfunction()

# fs-clang-fec-novec-20261117: nanors keeps its inner loops in explicit SIMD intrinsics, chosen at run time by CPU feature bits. What is left to the
# compiler is the scalar head and tail of each call. Clang's loop and SLP vectorizers turn that tail into a large vectorized function, which
# costs an 8-register prologue on every call of the 1.4 KB shards: 4-7% less FEC throughput than GCC on the same machine. Without them the
# Clang build matches GCC within 1.5%. GCC is not affected.
function(sunshine_configure_fec_sources test_directory)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(
                "${CMAKE_SOURCE_DIR}/third-party/nanors/rs.c"
                "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl/oblas_common.c"
                "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl/oblas_lite.c"
                DIRECTORY "${CMAKE_SOURCE_DIR}" "${test_directory}"
                PROPERTIES COMPILE_OPTIONS "-fno-vectorize;-fno-slp-vectorize")
    endif()
endfunction()

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
