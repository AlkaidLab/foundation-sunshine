cmake_minimum_required(VERSION 3.20)
project(sunshine_nvfbc_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to Sunshine source root (for plugin API header)
set(SUNSHINE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to Sunshine source root")

add_library(sunshine_nvfbc SHARED
    sunshine_nvfbc_plugin.cpp
)

target_include_directories(sunshine_nvfbc PRIVATE
    "${SUNSHINE_SOURCE_DIR}"
)

# NvFBC does not have a public import library on Windows.
# The plugin loads NvFBC64.dll at runtime via LoadLibrary.

# Output to plugins/ directory for easy deployment
set_target_properties(sunshine_nvfbc PROPERTIES
    OUTPUT_NAME "sunshine_nvfbc"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
