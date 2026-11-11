cmake_minimum_required(VERSION 3.25)

# The parent version, taken before project() below resets PROJECT_VERSION for this directory.
set(SUNSHINE_PARENT_VERSION "${PROJECT_VERSION}")
project(sunshine_tools)

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

add_executable(sunshinesvc sunshinesvc.cpp)
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
