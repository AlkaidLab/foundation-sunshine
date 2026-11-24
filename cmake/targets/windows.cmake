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
find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell powershell.exe)

if(NPM AND CARGO AND POWERSHELL_EXECUTABLE)
  # Match the Web UI's configured source when explicitly set in CMake. Without
  # it, inherit FOUNDATION_UI_DIR from the build environment if present.
  set(_panel_frontend_command ${NPM} run build:renderer)
  if(DEFINED FOUNDATION_UI_DIR AND NOT "${FOUNDATION_UI_DIR}" STREQUAL "")
    get_filename_component(_panel_ui_dir "${FOUNDATION_UI_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    set(_panel_frontend_command ${CMAKE_COMMAND} -E env "FOUNDATION_UI_DIR=${_panel_ui_dir}"
            cmd /D /C call ${NPM} run build:renderer)
  endif()
  add_custom_target(sunshine-control-panel
          WORKING_DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel"
          COMMENT "Building Sunshine Control Panel (Tauri GUI)"
          COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies..."
          COMMAND ${NPM} install
          COMMAND ${CMAKE_COMMAND} -E echo "Building native tool plugins..."
          COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
                  -File "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel/scripts/build-native-plugins.ps1"
                  -Configuration Release
          COMMAND ${CMAKE_COMMAND} -E echo "Building frontend with Vite..."
          COMMAND ${_panel_frontend_command}
          COMMAND ${CMAKE_COMMAND} -E echo "Building Tauri backend with Cargo..."
          COMMAND ${CARGO} build --manifest-path src-tauri/Cargo.toml --release
          USES_TERMINAL
          VERBATIM)
else()
  message(STATUS "npm/cargo/PowerShell not found — sunshine-control-panel target disabled (GUI will be fetched from release)")
endif()
