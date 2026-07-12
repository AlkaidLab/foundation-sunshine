cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories("${CMAKE_SOURCE_DIR}")

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        userenv
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(qiin-tabtip qiin-tabtip.cpp)
set_target_properties(qiin-tabtip PROPERTIES CXX_STANDARD 23)
target_compile_definitions(qiin-tabtip PRIVATE UNICODE _UNICODE)
target_link_libraries(qiin-tabtip
        ${CMAKE_THREAD_LIBS_INIT}
        shell32
        advapi32
        ole32
        oleaut32
        uuid
        ${PLATFORM_LIBRARIES})
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
        ${CMAKE_THREAD_LIBS_INIT}
        ${PLATFORM_LIBRARIES})
if(WIN32)
    target_link_libraries(test_args advapi32 userenv)
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
            setupapi
            ${PLATFORM_LIBRARIES})
    target_compile_options(vdd-frame-channel-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

    add_custom_target(check-vdd-ioctl-abi
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/check_vdd_ioctl_abi.ps1"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Checking Sunshine/VDD IOCTL ABI header sync; set VDD_DRIVER_HEADER when DriverHeader is not passed")
endif()
