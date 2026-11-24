#include "include/common.hlsl"
#include "include/hdr_encoding_params.hlsl"

float3 ConvertScRGBTo2100HLG(float3 rgb)
{
    return scRGBTo2100HLG(
        rgb,
        hdr_nominal_peak_nits,
        hdr_hlg_system_gamma);
}

#define CONVERT_FUNCTION ConvertScRGBTo2100HLG
