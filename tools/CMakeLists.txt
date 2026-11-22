cmake_minimum_required(VERSION 3.25)

# The parent version, taken before project() below resets PROJECT_VERSION for this directory.
set(SUNSHINE_PARENT_VERSION "${PROJECT_VERSION}")
project(sunshine_tools)

# fs-tools-no-boost-separate-20261117: none of these tools uses Boost.Asio or Boost.Beast, so they do not take the separately compiled
# copy that cmake/dependencies/boost_separate.cmake links into every other target (link_libraries is inherited by this directory).
# That library is one object file. A tool only needs a libstdc++ symbol that the object also defines (a weak copy of
# std::string::_M_replace), LLD loads the whole object for it, and the global initializers in it keep the Beast, Asio and OpenSSL
# code alive: dxgi-info and audio-info grew from 1.1 MB to 7 MB with the Clang toolchain, while the linker option --gc-sections
# was already on the command line. GNU ld does not load the object, so GCC builds never paid for it.
if(TARGET sunshine_boost_separate)
    get_directory_property(_sunshine_tools_link_libraries LINK_LIBRARIES)
    list(REMOVE_ITEM _sunshine_tools_link_libraries sunshine_boost_separate)
    set_directory_properties(PROPERTIES LINK_LIBRARIES "${_sunshine_tools_link_libraries}")
endif()

include_directories("${CMAKE_SOURCE_DIR}")

if(WIN32)
    find_program(SPARXIE_LAUNCH_DOTNET dotnet)
    if(SPARXIE_LAUNCH_DOTNET)
        set(SPARXIE_LAUNCH_PUBLISH_DIR "${CMAKE_CURRENT_BINARY_DIR}/sparxie-launch-publish")
        # cmake/packaging/windows.cmake installs from it in the parent scope
        set(SPARXIE_LAUNCH_PUBLISH_DIR "${SPARXIE_LAUNCH_PUBLISH_DIR}" PARENT_SCOPE)
        file(GLOB SPARXIE_LAUNCH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch/*.cs")
        add_custom_command(
            OUTPUT "${SPARXIE_LAUNCH_PUBLISH_DIR}/sparxie-launch.exe"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPARXIE_LAUNCH_PUBLISH_DIR}"
            COMMAND "${SPARXIE_LAUNCH_DOTNET}" publish "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch/Sparxie.Launch.csproj"
                    --configuration "$<CONFIG>" --output "${SPARXIE_LAUNCH_PUBLISH_DIR}"
                    # MSBuild defaults bin/obj next to the csproj; redirect them into the build tree
                    # so publishing does not drop build artifacts into the source checkout.
                    -p:BaseOutputPath="${CMAKE_CURRENT_BINARY_DIR}/sparxie-launch-bin/"
                    -p:BaseIntermediateOutputPath="${CMAKE_CURRENT_BINARY_DIR}/sparxie-launch-obj/"
                    --nologo --verbosity minimal
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch/Sparxie.Launch.csproj"
                ${SPARXIE_LAUNCH_SOURCES}
            VERBATIM)
        add_custom_target(sparxie-launch-publish DEPENDS "${SPARXIE_LAUNCH_PUBLISH_DIR}/sparxie-launch.exe")
        # The launcher's own checks: a self-contained test program (no test framework, no package restore). It runs only
        # when the build enables tests; it never ships.
        if(BUILD_TESTS)
            file(GLOB SPARXIE_LAUNCH_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch-tests/*.cs")
            add_custom_target(sparxie-launch-tests
                COMMAND "${SPARXIE_LAUNCH_DOTNET}" run --project "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch-tests/Sparxie.Launch.Tests.csproj"
                        --configuration Release --nologo --verbosity quiet
                DEPENDS ${SPARXIE_LAUNCH_SOURCES} ${SPARXIE_LAUNCH_TEST_SOURCES}
                VERBATIM)
            add_test(NAME sparxie_launch_tests
                COMMAND "${SPARXIE_LAUNCH_DOTNET}" run --project "${CMAKE_CURRENT_SOURCE_DIR}/sparxie-launch-tests/Sparxie.Launch.Tests.csproj"
                        --configuration Release --nologo --verbosity quiet)
            set_tests_properties(sparxie_launch_tests PROPERTIES TIMEOUT 300)
        endif()
    else()
        message(WARNING "dotnet was not found; touch profile launch helper will not be published")
    endif()
endif()

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        PRIVATE Threads::Threads
        dxgi
        ${PLATFORM_LIBRARIES}
        sunshine_project_options)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        PRIVATE ${Boost_LIBRARIES}
        Threads::Threads
        ksuser
        ${PLATFORM_LIBRARIES}
        sunshine_project_options)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp
        ${CMAKE_SOURCE_DIR}/src/crash/crash_core.cpp
        ${CMAKE_SOURCE_DIR}/src/crash/crash_common.cpp
        ${CMAKE_SOURCE_DIR}/src/crash/crash_win.cpp
        ${CMAKE_SOURCE_DIR}/src/crash/crash_posix.cpp)
target_compile_definitions(sunshinesvc PRIVATE SUNSHINESVC_VERSION="${SUNSHINE_PARENT_VERSION}")
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshinesvc
        PRIVATE Threads::Threads
        userenv
        wtsapi32
        ${PLATFORM_LIBRARIES}
        sunshine_project_options)
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(qiin-tabtip qiin-tabtip.cpp)
set_target_properties(qiin-tabtip PROPERTIES CXX_STANDARD 23)
target_compile_definitions(qiin-tabtip PRIVATE UNICODE _UNICODE)
target_link_libraries(qiin-tabtip
        PRIVATE Threads::Threads
        shell32
        advapi32
        ole32
        oleaut32
        uuid
        ${PLATFORM_LIBRARIES}
        sunshine_project_options)
target_compile_options(qiin-tabtip PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# 为 MinGW 设置正确的入口点和子系统
if(MINGW)
    # 优化大小：-Os 优化大小, -s 移除调试符号
    target_compile_options(qiin-tabtip PRIVATE -Os)
    target_link_options(qiin-tabtip PRIVATE 
        -municode 
        -s  # Strip symbols
        -Wl,--gc-sections  # 移除未使用的section
    )
endif()

if(WIN32)
    # The package version comes from PROJECT_VERSION here: CPACK_PACKAGE_VERSION is set by packaging/common.cmake,
    # which is included after this directory, so it is still empty at this point.
    string(REGEX REPLACE "^v" "" SUNSHINE_INSTALLER_VERSION "${SUNSHINE_PARENT_VERSION}")  # project(sunshine_tools) resets PROJECT_VERSION
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/installer_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/installer_version.h
        @ONLY)
    add_executable(sunshine-installer-shell WIN32
        installer_shell.cpp
        installer_shell_ui.inc
        installer_preview.inc
        installer_web.inc
        installer_json.h
        installer_d2d.h
        installer_dir.h
        installer_dir_win.h
        installer_failure.h
        installer_guide.h
        installer_state.h
        installer_maintenance.h
        installer_ui.h
        installer_webview2.h
        installer_icon.inc)
    set_target_properties(sunshine-installer-shell PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(sunshine-installer-shell PRIVATE UNICODE _UNICODE)
    target_include_directories(sunshine-installer-shell PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(sunshine-installer-shell
        PRIVATE d2d1 dwrite windowscodecs dwmapi shell32 advapi32 user32 ole32 uuid winhttp wintrust crypt32)
    target_compile_options(sunshine-installer-shell PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    if(MINGW)
        # static: the installer must start on a PC that has none of the MSYS2 runtime DLLs
        target_link_options(sunshine-installer-shell PRIVATE -municode -static)
        # The shell is the elevated worker and the updater too. LLD defaults to a 1 MiB stack reserve (GNU ld: 2 MiB); the
        # same 2 MiB as the other executables is the floor (scripts/ci/check-windows-release.py), 4 MiB here for headroom.
        target_link_options(sunshine-installer-shell PRIVATE -Wl,--stack,4194304)
        # the shell is appended to the installer as it is built: -g1 (cmake/compile_definitions/windows.cmake) must not end up in the package
        target_link_options(sunshine-installer-shell PRIVATE -Wl,--strip-debug)
    endif()
endif()

# 测试工具：test_args - 仅用于开发测试，不会被打包
add_executable(test_args test_args.cpp)
set_target_properties(test_args PROPERTIES CXX_STANDARD 23)
target_link_libraries(test_args
        PRIVATE Threads::Threads
        ${PLATFORM_LIBRARIES}
        sunshine_project_options)
if(WIN32)
    target_link_libraries(test_args PRIVATE advapi32 userenv)
endif()
target_compile_options(test_args PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# 明确标记为不安装（开发测试工具），但可以正常构建
# 注意：不会在 cmake/packaging/windows.cmake 中安装，所以不会被打包

if(WIN32)
    add_executable(vdd-frame-channel-probe vdd_frame_channel_probe.cpp)
    set_target_properties(vdd-frame-channel-probe PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON)
    target_link_libraries(vdd-frame-channel-probe
            PRIVATE setupapi
            ${PLATFORM_LIBRARIES}
            sunshine_project_options)
    target_compile_options(vdd-frame-channel-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

    add_executable(vtouch-poc
            vtouch_poc.cpp
            "${CMAKE_SOURCE_DIR}/src/remote_usb/loopback_usbip_bridge.cpp"
            "${CMAKE_SOURCE_DIR}/src/remote_usb/virtual_touchscreen_device.cpp")
    set_target_properties(vtouch-poc PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON)
    target_include_directories(vtouch-poc PRIVATE
            "${CMAKE_SOURCE_DIR}")
    target_link_libraries(vtouch-poc
            PRIVATE Boost::asio
            Boost::system
            shell32
            ole32
            user32
            advapi32
            ws2_32
            mswsock
            OpenSSL::Crypto
            sunshine_project_options sunshine_crypto)
    target_compile_options(vtouch-poc PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
    if(MINGW)
        target_link_options(vtouch-poc PRIVATE -municode)
    endif()

    add_custom_target(check-vdd-ioctl-abi
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/check_vdd_ioctl_abi.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Checking Sunshine/VDD IOCTL ABI header sync; set VDD_DRIVER_HEADER when DriverHeader is not passed")
endif()
