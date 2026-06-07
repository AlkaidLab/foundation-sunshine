# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        Windowsapp.lib
        Wtsapi32.lib)

set(NVPREFS_PLUGIN_ID "com.alkaidlab.nvidia-control-panel-optimizer")

add_executable(sunshine-plugin-nvprefs
        "${CMAKE_SOURCE_DIR}/src/plugins/nvprefs/nvprefs_plugin.cpp"
        ${NVPREFS_FILES})
target_link_libraries(sunshine-plugin-nvprefs PRIVATE
        advapi32
        nlohmann_json::nlohmann_json
        libstdc++.a
        libwinpthread.a)
target_compile_definitions(sunshine-plugin-nvprefs PRIVATE
        ${SUNSHINE_DEFINITIONS}
        SUNSHINE_NVPREFS_STANDALONE)
target_compile_options(sunshine-plugin-nvprefs PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
set_target_properties(sunshine-plugin-nvprefs PROPERTIES
        CXX_STANDARD 23
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
add_custom_command(TARGET sunshine-plugin-nvprefs POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sunshine>/assets/plugins/${NVPREFS_PLUGIN_ID}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:sunshine-plugin-nvprefs>" "$<TARGET_FILE_DIR:sunshine>/assets/plugins/${NVPREFS_PLUGIN_ID}/$<TARGET_FILE_NAME:sunshine-plugin-nvprefs>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/src_assets/windows/assets/plugins/${NVPREFS_PLUGIN_ID}/plugin.json" "$<TARGET_FILE_DIR:sunshine>/assets/plugins/${NVPREFS_PLUGIN_ID}/plugin.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/src_assets/windows/assets/plugins/${NVPREFS_PLUGIN_ID}/config.schema.json" "$<TARGET_FILE_DIR:sunshine>/assets/plugins/${NVPREFS_PLUGIN_ID}/config.schema.json"
        VERBATIM)

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
