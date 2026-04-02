# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        Windowsapp.lib
        Wtsapi32.lib)

# GUI build (optional — CI uses pre-built binary from GUI repo releases)
# For local development: ninja -C build sunshine-control-panel
find_program(NPM npm)
find_program(CARGO cargo)

if(NPM AND CARGO)
  add_custom_target(sunshine-control-panel
          WORKING_DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel"
          COMMENT "Building Sunshine Control Panel (Tauri GUI)"
          COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies..."
          COMMAND ${NPM} install
          COMMAND ${CMAKE_COMMAND} -E echo "Building frontend with Vite..."
          COMMAND ${NPM} run build:renderer
          COMMAND ${CMAKE_COMMAND} -E echo "Building Tauri backend with Cargo..."
          COMMAND ${CARGO} build --manifest-path src-tauri/Cargo.toml --release
          USES_TERMINAL)
else()
  message(STATUS "npm/cargo not found — sunshine-control-panel target disabled (GUI will be fetched from release)")
endif()
