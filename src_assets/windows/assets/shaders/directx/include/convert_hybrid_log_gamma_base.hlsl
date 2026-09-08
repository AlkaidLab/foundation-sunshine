#include "include/common.hlsl"
#include "include/hdr_pre_encode_transform.hlsl"

float3 ConvertScRGBTo2100HLG(float3 rgb)
{
    return scRGBTo2100HLG(
        ApplyHdrPreEncodeTransform(rgb),
        hdr_nominal_peak_nits,
        hdr_hlg_system_gamma);
}

#define CONVERT_FUNCTION ConvertScRGBTo2100HLG
