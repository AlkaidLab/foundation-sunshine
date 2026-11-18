option(BUILD_WEB_UI "Build the web UI via npm" ON)
if(NOT BUILD_WEB_UI)
    return()
endif()
find_program(NPM npm REQUIRED)
set(_npm_command "${NPM}")
if(WIN32)
    list(PREPEND _npm_command cmd /D /C call)
endif()
set(_npm_flags)
if(NPM_OFFLINE)
    list(APPEND _npm_flags --offline)
endif()
set(_npm_stamp "${PROJECT_SOURCE_DIR}/node_modules/.sunshine-cmake-dependencies")
add_custom_command(OUTPUT "${_npm_stamp}"
        COMMAND ${_npm_command} ci ${_npm_flags}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_npm_stamp}"
        DEPENDS "${PROJECT_SOURCE_DIR}/package.json" "${PROJECT_SOURCE_DIR}/package-lock.json"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Installing locked Web UI dependencies"
        VERBATIM)

# Track additions as well as edits; Vite config/plugins and prebuild scripts are
# inputs too. A deleted output page causes the complete bundle to rebuild.
file(GLOB_RECURSE _web_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/*" "${PROJECT_SOURCE_DIR}/scripts/vite/*")
# Vite resolves foundation-ui from this checkout, not from the web source tree.
# Cache the selection for automatic reconfiguration; change it with -DFOUNDATION_UI_DIR.
if(NOT DEFINED FOUNDATION_UI_DIR OR "${FOUNDATION_UI_DIR}" STREQUAL "")
    if(DEFINED ENV{FOUNDATION_UI_DIR} AND NOT "$ENV{FOUNDATION_UI_DIR}" STREQUAL "")
        set(_foundation_ui_default "$ENV{FOUNDATION_UI_DIR}")
    else()
        set(_foundation_ui_default "${PROJECT_SOURCE_DIR}/../ds5-ui-m3e")
    endif()
    get_filename_component(_foundation_ui_default "${_foundation_ui_default}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    set(FOUNDATION_UI_DIR "${_foundation_ui_default}" CACHE PATH "Foundation UI source directory" FORCE)
endif()
get_filename_component(_foundation_ui_dir "${FOUNDATION_UI_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
file(GLOB_RECURSE _foundation_ui_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${_foundation_ui_dir}/src/*" "${_foundation_ui_dir}/scripts/*")
list(APPEND _web_inputs "${_foundation_ui_dir}/package.json" ${_foundation_ui_inputs})
set(_web_outputs)
file(GLOB _web_pages RELATIVE "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web"
        CONFIGURE_DEPENDS "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/*.html")
list(FILTER _web_pages EXCLUDE REGEX "^template_")
foreach(_page IN LISTS _web_pages)
    list(APPEND _web_outputs "${PROJECT_BINARY_DIR}/assets/web/${_page}")
endforeach()
set(_web_stamp "${PROJECT_BINARY_DIR}/web-ui.stamp")
add_custom_command(OUTPUT "${_web_stamp}" ${_web_outputs}
        COMMAND "${CMAKE_COMMAND}" -E env
            "SUNSHINE_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}"
            "SUNSHINE_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}"
            "SUNSHINE_ASSETS_DIR=${NPM_ASSETS_DIR}"
            "FOUNDATION_UI_DIR=${_foundation_ui_dir}"
            ${_npm_command} run build -- --outDir "${PROJECT_BINARY_DIR}/assets/web"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_web_stamp}"
        DEPENDS "${_npm_stamp}" ${_web_inputs}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Building changed Web UI assets"
        VERBATIM)
add_custom_target(web-ui ALL DEPENDS "${_web_stamp}" ${_web_outputs})
