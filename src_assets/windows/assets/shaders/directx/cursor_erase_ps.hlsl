Texture2D current_frame : register(t0);
Texture2D reference_frame : register(t1);
Texture2D cursor_mask : register(t2);
SamplerState linear_sampler : register(s0);
SamplerState point_sampler : register(s1);

cbuffer cursor_erase_cbuffer : register(b3) {
    float4 frame_size;
    float4 cursor_rect;
    int4 options;
};

#include "include/base_vs_types.hlsl"

float4 sample_current(float2 tex_coord)
{
    return current_frame.Sample(linear_sampler, saturate(tex_coord), 0);
}

float4 inpaint_from_current(float2 screen_uv)
{
    float2 inv_size = 1.0 / max(frame_size.xy, float2(1.0, 1.0));
    float2 left_uv = float2((cursor_rect.x - 1.0) * inv_size.x, screen_uv.y);
    float2 right_uv = float2((cursor_rect.x + cursor_rect.z + 1.0) * inv_size.x, screen_uv.y);
    float2 top_uv = float2(screen_uv.x, (cursor_rect.y - 1.0) * inv_size.y);
    float2 bottom_uv = float2(screen_uv.x, (cursor_rect.y + cursor_rect.w + 1.0) * inv_size.y);

    return (sample_current(left_uv) +
            sample_current(right_uv) +
            sample_current(top_uv) +
            sample_current(bottom_uv)) * 0.25;
}

float4 main_ps(vertex_t input) : SV_Target
{
    float mask = cursor_mask.Sample(point_sampler, input.tex_coord, 0).a;
    if (mask <= 0.001) {
        discard;
    }

    float2 screen_uv = input.viewpoint_pos.xy / max(frame_size.xy, float2(1.0, 1.0));
    float4 replacement = options.x != 0 ?
        reference_frame.Sample(linear_sampler, saturate(screen_uv), 0) :
        inpaint_from_current(screen_uv);

    replacement.a = saturate(mask);
    return replacement;
}
