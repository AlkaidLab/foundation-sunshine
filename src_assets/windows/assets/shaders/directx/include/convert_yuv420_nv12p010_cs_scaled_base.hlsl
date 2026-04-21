// Compute-shader RGB -> NV12/P010 converter, scaling variant.
//
// Differs from convert_yuv420_nv12p010_cs_base.hlsl in that it supports an
// arbitrary scale between source and destination (out_rect_size / src_size),
// using:
//   - Y plane: 5-tap Catmull-Rom via bilinear samples (matches PS bicubic
//     quality with ~5 SampleLevel taps vs PS's 16 raw taps).
//   - UV plane: hardware bilinear box filter at the 2x2 chroma center.
//
// Includer must `#define CONVERT_FUNCTION fn` before including this file.
//
// Bindings:
//   t0 : Texture2D<float4>      source RGB (sRGB BGRA8 or scRGB FP16)
//   s0 : SamplerState           bilinear, edge-clamped
//   u0 : RWTexture2D<float>     Y plane view  (R8/R16_UNORM)
//   u1 : RWTexture2D<float2>    UV plane view (R8G8/R16G16_UNORM)
//   b0 : color_matrix_cbuffer
//   b1 : layout_cbuffer

Texture2D<float4>  source_image  : register(t0);
SamplerState       linear_sampler: register(s0);

RWTexture2D<float>  y_plane_uav  : register(u0);
RWTexture2D<float2> uv_plane_uav : register(u1);

cbuffer color_matrix_cbuffer : register(b0) {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
};

cbuffer cs_layout_cbuffer : register(b1) {
    int2  out_rect_offset;   // top-left of active rect in dest (Y plane coords)
    int2  out_rect_size;     // active rect width/height (output extent)
    int2  src_size;          // source texture size
    int2  cs_layout_pad;
};

#define CS_TILE 16

// 5-tap Catmull-Rom interpolation using bilinear samples.
// Skips the 4 corner taps (small visual impact vs the 9-tap version) for speed.
// Reference: vec3.ca/bicubic-filtering-in-fewer-taps + corner-skip optimization.
float3 SampleCatmullRom5Tap(float2 uv_norm, float2 src_size_f)
{
    float2 src_pos = uv_norm * src_size_f - 0.5;
    float2 i_pos   = floor(src_pos);
    float2 f       = src_pos - i_pos;

    // Catmull-Rom (b=0, c=0.5) tap weights along one axis.
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    // Combine the middle two taps so each pair becomes one bilinear fetch.
    float2 w12     = w1 + w2;
    float2 offset12 = w2 / w12;

    float2 inv_size = 1.0 / src_size_f;
    float2 tex0  = (i_pos - 1.0)            * inv_size;
    float2 tex3  = (i_pos + 2.0)            * inv_size;
    float2 tex12 = (i_pos + offset12)       * inv_size;

    // 5 samples in a "+" pattern, dropping the 4 corner taps. Sum of weights
    // along the centerline cross is approximately 1; divide by it to keep
    // energy/normalized output even when w0/w3 are noticeable.
    float3 c_tt = source_image.SampleLevel(linear_sampler, float2(tex12.x, tex0.y),  0).rgb;
    float3 c_lc = source_image.SampleLevel(linear_sampler, float2(tex0.x,  tex12.y), 0).rgb;
    float3 c_cc = source_image.SampleLevel(linear_sampler, float2(tex12.x, tex12.y), 0).rgb;
    float3 c_rc = source_image.SampleLevel(linear_sampler, float2(tex3.x,  tex12.y), 0).rgb;
    float3 c_bc = source_image.SampleLevel(linear_sampler, float2(tex12.x, tex3.y),  0).rgb;

    float3 sum = c_tt * (w12.x * w0.y)
               + c_lc * (w0.x  * w12.y)
               + c_cc * (w12.x * w12.y)
               + c_rc * (w3.x  * w12.y)
               + c_bc * (w12.x * w3.y);

    float weight_sum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return sum / max(weight_sum, 1e-5);
}

[numthreads(CS_TILE, CS_TILE, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID,
             uint3 GTid : SV_GroupThreadID)
{
    int2 rect_pos = int2(DTid.xy);
    bool inside_rect = (rect_pos.x < out_rect_size.x && rect_pos.y < out_rect_size.y);

    float2 src_size_f = float2(src_size);

    // ---- Y plane (per pixel) ----
    if (inside_rect) {
        float2 uv_norm = (float2(rect_pos) + 0.5) / float2(out_rect_size);
        float3 src_rgb = SampleCatmullRom5Tap(uv_norm, src_size_f);
        float3 rgb_y = CONVERT_FUNCTION(src_rgb);
        float y = dot(color_vec_y.xyz, rgb_y) + color_vec_y.w;
        y = y * range_y.x + range_y.y;

        int2 y_dst = rect_pos + out_rect_offset;
        y_plane_uav[uint2(y_dst)] = y;
    }

    // ---- UV plane (one thread per 2x2 block) ----
    if ((GTid.x & 1u) == 0u && (GTid.y & 1u) == 0u) {
        bool uv_inside_pair = (rect_pos.x + 1 < out_rect_size.x) && (rect_pos.y + 1 < out_rect_size.y);
        if (!uv_inside_pair && !inside_rect) {
            return;
        }
        // Hardware bilinear box: sample at the center of the 2x2 output pixel
        // group, mapped into source UV space. For non-edge UV pixels this is
        // equivalent to averaging the 4 surrounding source taps weighted by
        // the inverse-scale; cheaper than 4x Catmull-Rom and visually fine for
        // chroma (already half-resolution).
        float2 uv_pair_center_out = float2(rect_pos + 1);  // center of (rect_pos, rect_pos+1) box
        float2 uv_norm = uv_pair_center_out / float2(out_rect_size);
        float3 rgb_avg = source_image.SampleLevel(linear_sampler, uv_norm, 0).rgb;

        float3 rgb_uv = CONVERT_FUNCTION(rgb_avg);
        float u = dot(color_vec_u.xyz, rgb_uv) + color_vec_u.w;
        float v = dot(color_vec_v.xyz, rgb_uv) + color_vec_v.w;
        u = u * range_uv.x + range_uv.y;
        v = v * range_uv.x + range_uv.y;

        int2 uv_dst = (rect_pos + out_rect_offset) >> 1;
        uv_plane_uav[uint2(uv_dst)] = float2(u, v);
    }
}
