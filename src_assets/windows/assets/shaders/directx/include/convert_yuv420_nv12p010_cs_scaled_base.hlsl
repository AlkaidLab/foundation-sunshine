// Compute-shader RGB -> NV12/P010 converter, scaling variant.
//
// Differs from convert_yuv420_nv12p010_cs_base.hlsl in that it supports an
// arbitrary scale between source and destination (out_rect_size / src_size),
// using:
//   - Y plane: 5-tap Catmull-Rom via bilinear samples (matches PS bicubic
//     quality with ~5 SampleLevel taps vs PS's 16 raw taps).
//   - UV plane: 6 bilinear taps forming the type-0 sited chroma filter
//     ([1,2,1]/4 horizontal x [1,1]/2 vertical), matching the
//     LEFT_SUBSAMPLING_SCALE PS.
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

#include "include/hdr_analysis_snapshot.hlsl"

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
    float3 src_rgb = float3(0.0, 0.0, 0.0);
    if (inside_rect) {
        float2 uv_norm = (float2(rect_pos) + 0.5) / float2(out_rect_size);
        src_rgb = SampleCatmullRom5Tap(uv_norm, src_size_f);
    }

#ifdef HDR_ANALYSIS_SNAPSHOT
    // Analyze the same Catmull-Rom output samples that feed the encoded Y plane.
    // The capped snapshot stores exact per-cell scalar statistics, not one point sample.
    uint2 analysis_position;
    uint2 cell_begin;
    uint2 cell_end;
    if (inside_rect &&
        GetHdrAnalysisCell(rect_pos, out_rect_size, analysis_position, cell_begin, cell_end)) {
        float min_maxrgb_nits = 10000.0;
        float max_maxrgb_nits = 0.0;
        float sum_maxrgb_nits = 0.0;
        // Accumulated here rather than derived from the linear average: PQ is concave,
        // so mean(PQ(nits)) is not PQ(mean(nits)). HDR Vivid needs the former.
        float sum_maxrgb_pq = 0.0;
        uint pixel_count = 0;

        for (uint y = cell_begin.y; y < cell_end.y; ++y) {
            for (uint x = cell_begin.x; x < cell_end.x; ++x) {
                float3 cell_rgb = src_rgb;
                if (x != uint(rect_pos.x) || y != uint(rect_pos.y)) {
                    float2 cell_uv = (float2(x, y) + 0.5) / float2(out_rect_size);
                    cell_rgb = SampleCatmullRom5Tap(cell_uv, src_size_f);
                }
#ifdef HDR_PRE_ENCODE_TRANSFORM_AVAILABLE
                cell_rgb = ApplyHdrPreEncodeTransform(cell_rgb);
#endif
                float maxrgb_nits = HdrAnalysisMaxRgbNits(cell_rgb);
                min_maxrgb_nits = min(min_maxrgb_nits, maxrgb_nits);
                max_maxrgb_nits = max(max_maxrgb_nits, maxrgb_nits);
                sum_maxrgb_nits += maxrgb_nits;
                sum_maxrgb_pq += NitsToPQ(maxrgb_nits.xxx).x;
                ++pixel_count;
            }
        }

        StoreHdrAnalysisCellStats(
            analysis_position,
            min_maxrgb_nits,
            max_maxrgb_nits,
            sum_maxrgb_nits,
            sum_maxrgb_pq,
            pixel_count,
#ifdef HDR_PRE_ENCODE_TRANSFORM_AVAILABLE
            HdrAnalysisMaxRgbNits(ApplyHdrPreEncodeTransform(src_rgb))
#else
            HdrAnalysisMaxRgbNits(src_rgb)
#endif
        );
    }
#endif

    // ---- Y plane (per pixel) ----
    if (inside_rect) {
        float3 rgb_y = CONVERT_FUNCTION(src_rgb);
        float y = dot(color_vec_y.xyz, rgb_y) + color_vec_y.w;
        y = y * range_y.x + range_y.y;

        int2 y_dst = rect_pos + out_rect_offset;
        y_plane_uav[uint2(y_dst)] = y;
    }

    // ---- UV plane (one thread per 2x2 block) ----
    if ((GTid.x & 1u) == 0u && (GTid.y & 1u) == 0u) {
        if (!inside_rect) {
            return;
        }
        // Type-0 chroma siting: horizontally co-sited with the left luma column,
        // vertically centred between the two luma rows. Six bilinear taps at output
        // pixel centres, weighted [1, 2, 1] / 8 per row across both rows -- the same
        // tap layout and weights as the LEFT_SUBSAMPLING_SCALE pixel shader.
        //
        // A single tap at the 2x2 centre (what this used to do) is both mis-sited by
        // half a luma pixel and far too narrow when downscaling: at 2x it reads one
        // 2x2 source neighbourhood where the chroma pixel actually spans 4x4.
        // The CLAMP sampler supplies edge replication at the rect borders.
        float2 inv_out = 1.0 / float2(out_rect_size);
        float xl = (float(rect_pos.x) - 0.5) * inv_out.x;
        float xc = (float(rect_pos.x) + 0.5) * inv_out.x;
        float xr = (float(rect_pos.x) + 1.5) * inv_out.x;
        float yt = (float(rect_pos.y) + 0.5) * inv_out.y;
        float yb = (float(rect_pos.y) + 1.5) * inv_out.y;

        float3 rgb_avg =
            (source_image.SampleLevel(linear_sampler, float2(xl, yt), 0).rgb +
             source_image.SampleLevel(linear_sampler, float2(xl, yb), 0).rgb) * 0.125 +
            (source_image.SampleLevel(linear_sampler, float2(xc, yt), 0).rgb +
             source_image.SampleLevel(linear_sampler, float2(xc, yb), 0).rgb) * 0.25 +
            (source_image.SampleLevel(linear_sampler, float2(xr, yt), 0).rgb +
             source_image.SampleLevel(linear_sampler, float2(xr, yb), 0).rgb) * 0.125;

        float3 rgb_uv = CONVERT_FUNCTION(rgb_avg);
        float u = dot(color_vec_u.xyz, rgb_uv) + color_vec_u.w;
        float v = dot(color_vec_v.xyz, rgb_uv) + color_vec_v.w;
        u = u * range_uv.x + range_uv.y;
        v = v * range_uv.x + range_uv.y;

        int2 uv_dst = (rect_pos + out_rect_offset) >> 1;
        uv_plane_uav[uint2(uv_dst)] = float2(u, v);
    }
}
