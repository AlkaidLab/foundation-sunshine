if (NOT DEFINED ANALYSIS_DXIL OR
    NOT DEFINED REDUCE_DXIL OR
    NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "ANALYSIS_DXIL, REDUCE_DXIL and OUTPUT_HEADER are required")
endif ()

function(dxil_to_array input_path symbol output_var)
    file(READ "${input_path}" binary_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," binary_bytes "${binary_hex}")
    string(LENGTH "${binary_hex}" binary_hex_length)
    math(EXPR binary_size "${binary_hex_length} / 2")
    set(${output_var}
            "inline constexpr unsigned char ${symbol}[] = {${binary_bytes}};\ninline constexpr std::size_t ${symbol}_size = ${binary_size};\n"
            PARENT_SCOPE)
endfunction()

dxil_to_array("${ANALYSIS_DXIL}" hdr_luminance_analysis_cs_dxil analysis_array)
dxil_to_array("${REDUCE_DXIL}" hdr_luminance_reduce_cs_dxil reduce_array)

file(WRITE "${OUTPUT_HEADER}"
        "#pragma once\n#include <cstddef>\n\nnamespace platf::dxgi::d3d12::shaders {\n${analysis_array}${reduce_array}}  // namespace platf::dxgi::d3d12::shaders\n")
