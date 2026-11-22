cmake_minimum_required(VERSION 3.20)

# Source drop of the Brotli decoder (google/brotli v1.1.0, MIT): decode-only,
# no encoder, no tools. The Web UI ships each text asset exactly once, already
# Brotli-compressed; a client that does not accept Brotli gets an identity
# representation decoded on the fly from that same file, so no second copy of
# the asset exists anywhere on disk or in the packages.
add_library(brotli_dec STATIC
        common/constants.c
        common/context.c
        common/dictionary.c
        common/platform.c
        common/shared_dictionary.c
        common/transform.c
        dec/bit_reader.c
        dec/decode.c
        dec/huffman.c
        dec/state.c)
target_include_directories(brotli_dec PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(brotli_dec PUBLIC c_std_11)
set_target_properties(brotli_dec PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

