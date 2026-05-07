# rust_tray.cmake
# CMake configuration for building and linking the Rust tray library

set(RUST_TRAY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/rust_tray")
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust_tray")

# Determine the Rust target and output filename based on platform
if(WIN32)
    # Windows uses MinGW/UCRT toolchain - must use gnu target
    set(RUST_TARGET "x86_64-pc-windows-gnu")
    set(RUST_LIB_NAME "libtray.a")
elseif(APPLE)
    set(RUST_LIB_NAME "libtray.a")
    # Check for ARM64
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(RUST_TARGET "aarch64-apple-darwin")
    else()
        set(RUST_TARGET "x86_64-apple-darwin")
    endif()
else()
    set(RUST_LIB_NAME "libtray.a")
    # Check for ARM64
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        set(RUST_TARGET "aarch64-unknown-linux-gnu")
    else()
        set(RUST_TARGET "x86_64-unknown-linux-gnu")
    endif()
endif()

# Set the output path based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RUST_BUILD_TYPE "debug")
    set(CARGO_BUILD_FLAGS "")
else()
    set(RUST_BUILD_TYPE "release")
    set(CARGO_BUILD_FLAGS "--release")
endif()

# Output path: target/<target>/<profile>/
set(RUST_OUTPUT_LIB "${RUST_TARGET_DIR}/${RUST_TARGET}/${RUST_BUILD_TYPE}/${RUST_LIB_NAME}")

# Find cargo
find_program(CARGO_EXECUTABLE cargo HINTS $ENV{HOME}/.cargo/bin $ENV{USERPROFILE}/.cargo/bin)
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR "Cargo (Rust package manager) not found. Please install Rust: https://rustup.rs/")
endif()

message(STATUS "Found Cargo: ${CARGO_EXECUTABLE}")
message(STATUS "Rust target: ${RUST_TARGET}")
message(STATUS "Rust tray library will be built at: ${RUST_OUTPUT_LIB}")

# Custom command to build the Rust library
add_custom_command(
    OUTPUT ${RUST_OUTPUT_LIB}
    COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TARGET_DIR=${RUST_TARGET_DIR}
        ${CARGO_EXECUTABLE} build
        --manifest-path ${RUST_TRAY_SOURCE_DIR}/Cargo.toml
        --target ${RUST_TARGET}
        ${CARGO_BUILD_FLAGS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Rust tray library (${RUST_BUILD_TYPE})"
    DEPENDS
        ${RUST_TRAY_SOURCE_DIR}/Cargo.toml
        ${RUST_TRAY_SOURCE_DIR}/build.rs
        ${RUST_TRAY_SOURCE_DIR}/src/lib.rs
        ${RUST_TRAY_SOURCE_DIR}/src/i18n.rs
        ${RUST_TRAY_SOURCE_DIR}/src/actions.rs
    VERBATIM
)

# Create a custom target for the Rust library
add_custom_target(rust_tray ALL DEPENDS ${RUST_OUTPUT_LIB})

# Create an imported static library target
add_library(rust_tray_lib STATIC IMPORTED GLOBAL)
set_target_properties(rust_tray_lib PROPERTIES
    IMPORTED_LOCATION ${RUST_OUTPUT_LIB}
)
add_dependencies(rust_tray_lib rust_tray)

# Export the library path for use in other CMake files
set(RUST_TRAY_LIBRARY ${RUST_OUTPUT_LIB} CACHE FILEPATH "Path to the Rust tray library")

# Platform-specific dependencies for the Rust library
if(WIN32)
    # MinGW/UCRT dependencies for Rust static library
    set(RUST_TRAY_PLATFORM_LIBS
        user32
        gdi32
        shell32
        ole32
        oleaut32
        uuid
        comctl32
        bcrypt
        ntdll
        userenv
        ws2_32
        pthread
    )
elseif(APPLE)
    # macOS dependencies for tray-icon crate
    set(RUST_TRAY_PLATFORM_LIBS
        "-framework Cocoa"
        "-framework AppKit"
        "-framework Foundation"
    )
else()
    # Linux dependencies for tray-icon crate
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    pkg_check_modules(GLIB REQUIRED glib-2.0)

    set(RUST_TRAY_PLATFORM_LIBS
        ${GTK3_LIBRARIES}
        ${GLIB_LIBRARIES}
    )
endif()

message(STATUS "Rust tray platform libraries: ${RUST_TRAY_PLATFORM_LIBS}")
