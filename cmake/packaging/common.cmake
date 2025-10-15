# common packaging

# common cpack options
set(CPACK_PACKAGE_NAME ${CMAKE_PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "LizardByte")
string(REGEX REPLACE "^v" "" CPACK_PACKAGE_VERSION ${PROJECT_VERSION})  # remove the v prefix if it exists
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cpack_artifacts)
set(CPACK_PACKAGE_CONTACT "https://app.lizardbyte.dev")
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

# install sunshine control panel (Tauri GUI)
if(WIN32)
    set(TAURI_TARGET_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/common/sunshine-control-panel/src-tauri/target")
    
    # 分别尝试安装每个可能的 GUI 路径
    install(PROGRAMS 
        "${TAURI_TARGET_DIR}/x86_64-pc-windows-gnu/release/sunshine-gui.exe"
        DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
        RENAME "sunshine-gui.exe"
        OPTIONAL)
    install(PROGRAMS 
        "${TAURI_TARGET_DIR}/x86_64-pc-windows-msvc/release/sunshine-gui.exe"
        DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
        RENAME "sunshine-gui.exe"
        OPTIONAL)
    install(PROGRAMS 
        "${TAURI_TARGET_DIR}/release/sunshine-gui.exe"
        DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
        RENAME "sunshine-gui.exe"
        OPTIONAL)
    
    # 安装 WebView2Loader.dll
    install(FILES 
        "${TAURI_TARGET_DIR}/x86_64-pc-windows-gnu/release/WebView2Loader.dll"
        "${TAURI_TARGET_DIR}/x86_64-pc-windows-msvc/release/WebView2Loader.dll"
        "${TAURI_TARGET_DIR}/release/WebView2Loader.dll"
        DESTINATION "${SUNSHINE_ASSETS_DIR}/gui"
        OPTIONAL)
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
