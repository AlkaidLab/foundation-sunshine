#include "include/common.hlsl"

cbuffer hlg_display_cbuffer : register(b3) {
    float hlg_peak_nits;
    float hlg_system_gamma;
    float hlg_sdr_band_gain;
    float hlg_sdr_band_top;
};

float3 ConvertScRGBTo2100HLG(float3 rgb)
{
    // Re-anchor SDR-referenced content (composed at the Windows SDR white) to
    // the client's SDR reference white. Full gain at or below the band top,
    // fading back to 1.0 by 2x so HDR highlights keep their absolute values.
    // band top <= 0 or gain == 1 leaves the signal untouched.
    if (hlg_sdr_band_top > 0.0 && hlg_sdr_band_gain != 1.0) {
        float level = max(rgb.r, max(rgb.g, rgb.b));
        float t = saturate((level - hlg_sdr_band_top) / max(hlg_sdr_band_top, 1e-4));
        rgb *= lerp(hlg_sdr_band_gain, 1.0, smoothstep(0.0, 1.0, t));
    }
    return scRGBTo2100HLG(rgb, hlg_peak_nits, hlg_system_gamma);
}

#define CONVERT_FUNCTION ConvertScRGBTo2100HLG
