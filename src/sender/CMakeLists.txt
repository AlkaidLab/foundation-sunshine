# CMakeLists.txt for sunshine-sender subprocess
# This builds the sender executable that handles the data plane

set(SENDER_TARGET_FILES
    "${CMAKE_SOURCE_DIR}/src/sender/sender_main.cpp"
    "${CMAKE_SOURCE_DIR}/src/subprocess/ipc_pipe.cpp"
    "${CMAKE_SOURCE_DIR}/src/subprocess/ipc_protocol.h"
)

add_executable(sunshine-sender ${SENDER_TARGET_FILES})

target_include_directories(sunshine-sender PRIVATE
    "${CMAKE_SOURCE_DIR}"
)

target_compile_definitions(sunshine-sender PRIVATE
    SUBPROCESS_STANDALONE=1
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

set_target_properties(sunshine-sender PROPERTIES
    CXX_STANDARD 23
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(sunshine-sender PRIVATE
        ws2_32
    )
else()
    target_link_libraries(sunshine-sender PRIVATE
        pthread
    )
endif()

# Install sender alongside main executable
install(TARGETS sunshine-sender
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime
)
