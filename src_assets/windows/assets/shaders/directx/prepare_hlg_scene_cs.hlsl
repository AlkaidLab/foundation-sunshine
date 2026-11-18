// Prepare display-linear Windows scRGB for AMF's scene-linear HLG input.
// The capture display's peak and BT.2100 system gamma match the shader fallback.
#include "include/common.hlsl"
#include "include/hdr_encoding_params.hlsl"

Texture2D<float4> source_image : register(t0);
RWTexture2D<float4> scene_image : register(u0);

[numthreads(16, 16, 1)]
void main_cs(uint3 position : SV_DispatchThreadID)
{
    uint width, height;
    source_image.GetDimensions(width, height);
    if (position.x >= width || position.y >= height) return;
    float3 display_nits = Rec709toRec2020(source_image.Load(int3(position.xy, 0)).rgb) * 80.0;
    scene_image[position.xy] = float4(DisplayLinearToHLGScene(
        display_nits, hdr_nominal_peak_nits, hdr_hlg_system_gamma), 1.0);
}
