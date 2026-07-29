#include "include/common.hlsl"

cbuffer hlg_display_cbuffer : register(b3) {
    float hlg_peak_nits;
    float hlg_system_gamma;
    float2 hlg_display_pad;
};

float3 ConvertScRGBTo2100HLG(float3 rgb)
{
    return scRGBTo2100HLG(rgb, hlg_peak_nits, hlg_system_gamma);
}

#define CONVERT_FUNCTION ConvertScRGBTo2100HLG
