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
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(qiin-taptip qiin-taptip.cpp)
set_target_properties(qiin-taptip PROPERTIES CXX_STANDARD 23)
target_compile_definitions(qiin-taptip PRIVATE UNICODE _UNICODE)
target_link_libraries(qiin-taptip
        ${CMAKE_THREAD_LIBS_INIT}
        shell32
        advapi32
        ole32
        oleaut32
        uuid
        ${PLATFORM_LIBRARIES})
target_compile_options(qiin-taptip PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# 为 MinGW 设置正确的入口点和子系统
if(MINGW)
    # 优化大小：-Os 优化大小, -s 移除调试符号
    target_compile_options(qiin-taptip PRIVATE -Os)
    target_link_options(qiin-taptip PRIVATE 
        -municode 
        -s  # Strip symbols
        -Wl,--gc-sections  # 移除未使用的section
    )
endif()