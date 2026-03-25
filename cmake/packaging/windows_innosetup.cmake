# Inno Setup Packaging
# Replaces NSIS packaging with Inno Setup for Windows installer generation
# 
# Usage:
#   cmake --build build --target innosetup
#   或直接:
#   iscc build/sunshine_installer.iss
#
# 依赖: Inno Setup 6.x (https://jrsoftware.org/isinfo.php)

# Find Inno Setup compiler
find_program(ISCC_EXECUTABLE iscc
    PATHS
        "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
        "$ENV{ProgramFiles}/Inno Setup 6"
        "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    DOC "Inno Setup Compiler (iscc.exe)"
)

if(NOT ISCC_EXECUTABLE)
    message(STATUS "Inno Setup not found - 'innosetup' target will not be available")
    message(STATUS "Install from: https://jrsoftware.org/isdl.php")
    return()
endif()

message(STATUS "Found Inno Setup: ${ISCC_EXECUTABLE}")

# Configure the .iss template with CMake variables
set(INNO_STAGING_DIR "${CMAKE_BINARY_DIR}/inno_staging")
# Use a separate variable so we don't overwrite CMAKE_INSTALL_PREFIX
set(CMAKE_INSTALL_PREFIX_SAVED "${CMAKE_INSTALL_PREFIX}")
set(CMAKE_INSTALL_PREFIX "${INNO_STAGING_DIR}")

configure_file(
    "${CMAKE_MODULE_PATH}/packaging/sunshine.iss.in"
    "${CMAKE_BINARY_DIR}/sunshine_installer.iss"
    @ONLY
)

# Restore CMAKE_INSTALL_PREFIX
set(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX_SAVED}")

# Create a custom target for building the Inno Setup installer
# Step 1: Install files to staging directory
# Step 2: Strip debug symbols from executables to reduce installer size
# Step 3: Run Inno Setup compiler
add_custom_target(innosetup
    COMMENT "Building Inno Setup installer..."
    
    # Install to staging directory
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${CMAKE_BINARY_DIR}/inno_staging"
    
    # Strip debug symbols from executables to reduce installer size
    COMMAND ${CMAKE_COMMAND} -E echo "Stripping debug symbols from executables..."
    COMMAND strip --strip-debug "${CMAKE_BINARY_DIR}/inno_staging/sunshine.exe"
    COMMAND strip --strip-debug "${CMAKE_BINARY_DIR}/inno_staging/tools/sunshinesvc.exe"
    COMMAND strip --strip-debug "${CMAKE_BINARY_DIR}/inno_staging/tools/dxgi-info.exe"
    COMMAND strip --strip-debug "${CMAKE_BINARY_DIR}/inno_staging/tools/audio-info.exe"
    
    # Run Inno Setup compiler
    COMMAND "${ISCC_EXECUTABLE}" "${CMAKE_BINARY_DIR}/sunshine_installer.iss"
    
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VERBATIM
)

# Make sure sunshine is built before creating the installer
add_dependencies(innosetup sunshine)

# Also provide a convenience target to just generate the staging directory
add_custom_target(innosetup-staging
    COMMENT "Generating Inno Setup staging directory..."
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${CMAKE_BINARY_DIR}/inno_staging"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VERBATIM
)
