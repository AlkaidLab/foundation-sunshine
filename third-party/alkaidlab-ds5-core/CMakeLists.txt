cmake_minimum_required(VERSION 3.20)

# The source drop is deliberately a pair of plain-C static libraries.  It has
# no dependency on the Platform Runtime, Module SDK, or Foundation Sunshine.
add_library(alkaidlab_ds5_core STATIC
        ds5-core/src/ds5_core.c)
target_include_directories(alkaidlab_ds5_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/ds5-core/include")
target_compile_features(alkaidlab_ds5_core PUBLIC c_std_11)
set_target_properties(alkaidlab_ds5_core PROPERTIES C_EXTENSIONS OFF)

add_library(alkaidlab_usbip_core STATIC
        usbip-core/src/usbip_core.c)
target_include_directories(alkaidlab_usbip_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/usbip-core/include")
target_compile_features(alkaidlab_usbip_core PUBLIC c_std_11)
set_target_properties(alkaidlab_usbip_core PROPERTIES C_EXTENSIONS OFF)
