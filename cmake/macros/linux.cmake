# Generate both outputs at build time; each command and XML dependency is explicit.
macro(GEN_WAYLAND wayland_directory subdirectory filename)
    find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
    set(_xml "${wayland_directory}/${subdirectory}/${filename}.xml")
    set(_generated "${PROJECT_BINARY_DIR}/generated-src")
    add_custom_command(OUTPUT "${_generated}/${filename}.c" "${_generated}/${filename}.h"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_generated}"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${_xml}" "${_generated}/${filename}.c"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${_xml}" "${_generated}/${filename}.h"
            DEPENDS "${_xml}" "${WAYLAND_SCANNER_EXECUTABLE}"
            VERBATIM)
    list(APPEND PLATFORM_TARGET_FILES "${_generated}/${filename}.c" "${_generated}/${filename}.h")
endmacro()
