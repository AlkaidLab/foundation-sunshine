// Decode the actual VPE output for scene statistics, after scaling and quantization.
// inputMode: 2=PQ limited, 3=PQ full, 4=HLG limited, 5=HLG full.
// Texture2D<float4> inputTexture is the R16_UNORM luma plane; chroma is at t2.
Texture2D<float2> encodedChroma : register(t2);
#include "include/hdr_encoding_params.hlsl"

float3 AnalysisPQToNits(float3 signal)
{
    float3 p = pow(saturate(signal), 32.0 / 2523.0);
    return 10000.0 * pow(max(p - 3424.0 / 4096.0, 0.0) /
        max(2413.0 / 128.0 - (2392.0 / 128.0) * p, 1e-6), 16384.0 / 2610.0);
}

float3 AnalysisHLGToNits(float3 signal)
{
    signal = saturate(signal);
    float3 scene = lerp(signal * signal / 3.0,
        (exp((signal - 0.55991073) / 0.17883277) + 0.28466892) / 12.0,
        step(0.5, signal));
    float luminance = dot(scene, float3(0.2627, 0.6780, 0.0593));
    return hdr_nominal_peak_nits * scene * pow(max(luminance, 1e-8), hdr_hlg_system_gamma - 1.0);
}

float LoadAnalysisMaxRgbNits(uint2 position)
{
    if (inputMode < 2) {
        return HdrAnalysisMaxRgbNits(inputTexture.Load(int3(position, 0)).rgb);
    }
    uint2 pixel = position + sourceOffset;
    // P010 stores ten-bit codes in the high bits of each sixteen-bit sample.
    float yCode = inputTexture.Load(int3(pixel, 0)).r * (65535.0 / 64.0);
    float2 uvCode = encodedChroma.Load(int3(pixel / 2, 0)) * (65535.0 / 64.0);
    bool fullRange = (inputMode & 1u) != 0;
    float y = fullRange ? yCode / 1023.0 : (yCode - 64.0) / 876.0;
    float2 uv = (uvCode - 512.0) / (fullRange ? 1023.0 : 896.0);
    float3 rgb = float3(y + 1.4746 * uv.y,
        y - 0.164553 * uv.x - 0.571353 * uv.y, y + 1.8814 * uv.x);
    float3 nits = inputMode >= 4 ? AnalysisHLGToNits(rgb) : AnalysisPQToNits(rgb);
    return min(max(max(nits.r, nits.g), nits.b), maxAnalysisNits);
}
