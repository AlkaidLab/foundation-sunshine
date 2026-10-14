# Small reusable modules; variant-bearing host/backend sources stay on their owners.
add_library(sunshine_crypto STATIC "${PROJECT_SOURCE_DIR}/src/crypto.cpp")
target_link_libraries(sunshine_crypto PUBLIC sunshine::tls)

add_library(sunshine_ai_provider STATIC "${PROJECT_SOURCE_DIR}/src/ai/provider.cpp")
target_link_libraries(sunshine_ai_provider PUBLIC nlohmann_json::nlohmann_json)

add_library(sunshine_ai_stream STATIC "${PROJECT_SOURCE_DIR}/src/ai/http_stream.cpp")
target_link_libraries(sunshine_ai_stream PUBLIC sunshine_http Threads::Threads)

foreach(_module IN ITEMS crypto ai_provider ai_stream)
    target_link_libraries("sunshine_${_module}" PRIVATE sunshine_project_options sunshine_coverage)
    target_include_directories("sunshine_${_module}" PUBLIC "${PROJECT_SOURCE_DIR}")
    target_compile_features("sunshine_${_module}" PUBLIC cxx_std_23)
    target_compile_options("sunshine_${_module}" PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>)
endforeach()

if(WIN32)
    # RC needs no application/Boost include graph. A separate object target
    # also prevents windres preprocessing from exceeding cmd.exe's size limit.
    add_library(sunshine_windows_resources OBJECT "${PROJECT_BINARY_DIR}/windows.rc")
    target_link_libraries(sunshine_windows_resources PRIVATE sunshine_project_options)
endif()
