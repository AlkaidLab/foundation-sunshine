# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        Windowsapp.lib
        Wtsapi32.lib)

#GUI build
# Find required tools (always find, but only build if requested)
find_program(NPM npm REQUIRED)
find_program(CARGO cargo REQUIRED)

# GUI target is optional - not part of ALL target
# Use 'ninja -C build sunshine-control-panel' to build it explicitly
add_custom_target(sunshine-control-panel
        WORKING_DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel"
        COMMENT "Building Sunshine Control Panel (Tauri GUI)"
        # Step 1: Install npm dependencies
        COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies..."
        COMMAND ${NPM} install
        # Step 2: Build frontend with Vite
        COMMAND ${CMAKE_COMMAND} -E echo "Building frontend with Vite..."
        COMMAND ${NPM} run build:renderer
        # Step 3: Build Tauri backend with Cargo directly (bypass @tauri-apps/cli Node-API issue)
        COMMAND ${CMAKE_COMMAND} -E echo "Building Tauri backend with Cargo..."
        COMMAND ${CARGO} build --manifest-path src-tauri/Cargo.toml --release
        USES_TERMINAL)
