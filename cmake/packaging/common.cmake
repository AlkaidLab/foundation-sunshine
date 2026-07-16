# common packaging

# common cpack options
set(CPACK_PACKAGE_NAME ${CMAKE_PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "qiin2333")
string(REGEX REPLACE "^v" "" CPACK_PACKAGE_VERSION ${PROJECT_VERSION})  # remove the v prefix if it exists
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cpack_artifacts)
set(CPACK_PACKAGE_CONTACT "https://alkaidlab.com")
set(CPACK_PACKAGE_DESCRIPTION ${CMAKE_PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_HOMEPAGE_URL ${CMAKE_PROJECT_HOMEPAGE_URL})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_ICON ${PROJECT_SOURCE_DIR}/sunshine.png)
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_STRIP_FILES YES)

# install common assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        PATTERN "web" EXCLUDE)

# install ABR prompt template
install(FILES "${PROJECT_SOURCE_DIR}/src/assets/abr_prompt.md"
        DESTINATION "${SUNSHINE_ASSETS_DIR}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets")
configure_file("${PROJECT_SOURCE_DIR}/src/assets/abr_prompt.md"
               "${CMAKE_CURRENT_BINARY_DIR}/assets/abr_prompt.md"
               COPYONLY)
# copy assets to build directory, for running without install
file(GLOB_RECURSE ALL_ASSETS
        RELATIVE "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/" "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/*")
list(FILTER ALL_ASSETS EXCLUDE REGEX "^web/.*$")  # Filter out the web directory
foreach(asset ${ALL_ASSETS})  # Copy assets to build directory, excluding the web directory
    file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/${asset}"
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/assets")
endforeach()

# install built vite assets
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/assets/web"
        DESTINATION "${SUNSHINE_ASSETS_DIR}")

# install sunshine control panel (Tauri GUI) — from pre-built download
if(WIN32)
    option(SUNSHINE_PREFER_LOCAL_GUI "Prefer a locally-built Tauri GUI over the downloaded GUI binary" ON)

    set(_local_gui_dir "")
    if(SUNSHINE_PREFER_LOCAL_GUI)
        set(TAURI_TARGET_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel/src-tauri/target")
        set(_candidate_gui_dirs
                "${TAURI_TARGET_DIR}/release"
                "${TAURI_TARGET_DIR}/x86_64-pc-windows-gnu/release"
                "${TAURI_TARGET_DIR}/x86_64-pc-windows-msvc/release")
        set(_local_gui_timestamp "")
        foreach(_candidate_gui_dir IN LISTS _candidate_gui_dirs)
            set(_candidate_gui_exe "${_candidate_gui_dir}/sunshine-gui.exe")
            if(EXISTS "${_candidate_gui_exe}")
                file(TIMESTAMP "${_candidate_gui_exe}" _candidate_gui_timestamp "%Y%m%d%H%M%S" UTC)
                if(NOT _local_gui_dir OR _candidate_gui_timestamp STRGREATER _local_gui_timestamp)
                    set(_local_gui_dir "${_candidate_gui_dir}")
                    set(_local_gui_timestamp "${_candidate_gui_timestamp}")
                endif()
            endif()
        endforeach()
    endif()

    if(_local_gui_dir)
        set(GUI_DIR "${_local_gui_dir}")
        set(GUI_DIR "${_local_gui_dir}" CACHE PATH "GUI binary directory" FORCE)
        message(STATUS "Using local Sunshine GUI binary at ${GUI_DIR}")
    else()
        include(${CMAKE_MODULE_PATH}/packaging/FetchGUI.cmake)
    endif()

    if(EXISTS "${GUI_DIR}/sunshine-gui.exe")
        install(PROGRAMS "${GUI_DIR}/sunshine-gui.exe"
            DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
            COMPONENT gui)
        # WebView2Loader.dll (optional — Tauri 2 may embed it)
        if(EXISTS "${GUI_DIR}/WebView2Loader.dll")
            install(FILES "${GUI_DIR}/WebView2Loader.dll"
                DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
                COMPONENT gui)
        endif()
    else()
        if(SUNSHINE_ENABLE_TRAY AND NOT SUNSHINE_ENABLE_LEGACY_TRAY)
            message(FATAL_ERROR
                "Sunshine GUI is required by the Windows GUI tray build, but sunshine-gui.exe is unavailable. "
                "Build the local GUI or configure an explicit GUI release before packaging.")
        endif()
        message(WARNING "Sunshine GUI binary is unavailable; continuing only because the GUI tray is disabled")
    endif()
endif()

# platform specific packaging
if(WIN32)
    include(${CMAKE_MODULE_PATH}/packaging/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/packaging/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/packaging/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/packaging/linux.cmake)
    endif()
endif()

include(CPack)
