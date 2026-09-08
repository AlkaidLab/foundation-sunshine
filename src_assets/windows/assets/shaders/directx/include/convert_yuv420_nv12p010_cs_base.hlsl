// Compute-shader RGB -> NV12/P010 converter (Phase 1: type0, no rotation/scaling).
//
// Each threadgroup writes a 16x16 block of Y plane + 8x8 block of UV plane.
// One source RGB sample per thread is cached in groupshared memory, then reused
// for both Y (per pixel) and UV (type-0 sited chroma tent, matching the
// LEFT_SUBSAMPLING PS).
//
// Includer must `#define CONVERT_FUNCTION fn` before including this file.
//
// Bindings:
//   t0 : Texture2D<float4>      source RGB (sRGB BGRA8 or scRGB FP16)
//   s0 : SamplerState           (unused on this fast path; we do integer Load)
//   u0 : RWTexture2D<float>     Y plane view  (R8_UNORM for NV12, R16_UNORM for P010)
//   u1 : RWTexture2D<float2>    UV plane view (R8G8_UNORM / R16G16_UNORM)
//   b0 : color_matrix_cbuffer   (same layout as PS path)
//   b1 : layout_cbuffer         (output rect, source size)

Texture2D<float4> source_image : register(t0);

RWTexture2D<float>  y_plane_uav  : register(u0);
RWTexture2D<float2> uv_plane_uav : register(u1);

cbuffer color_matrix_cbuffer : register(b0) {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
};

// Output rect: where we write inside the destination texture (aspect-ratio padded).
// Source size: source texture dimensions. For type0 these are equal to rect size.
cbuffer cs_layout_cbuffer : register(b1) {
    int2  out_rect_offset;   // top-left of active rect in dest (Y plane coords)
    int2  out_rect_size;     // active rect width/height (== source size for type0)
    int2  src_size;          // source texture size (Load needs integer coords)
    int2  cs_layout_pad;
};

#include "include/hdr_analysis_snapshot.hlsl"

#define CS_TILE 16

// Column 0 is a left halo: s_rgb[y][k] holds the pixel at tile column (k - 1).
// The type-0 chroma tent reaches one pixel to the left of each 2x2 block, which
// for GTid.x == 0 lives in the previous threadgroup.
groupshared float3 s_rgb[CS_TILE][CS_TILE + 1];

[numthreads(CS_TILE, CS_TILE, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID,
             uint3 GTid : SV_GroupThreadID)
{
    // Position inside the output rect (one pixel per thread, Y-plane resolution).
    int2 rect_pos = int2(DTid.xy);

    // Source position == rect position for type0 (no scale, no rotation).
    int2 src_pos = rect_pos;

    // Out-of-rect / out-of-source pixels load black (still need LDS write so neighbors are valid).
    float3 src_rgb = float3(0.0, 0.0, 0.0);
    bool inside_rect = (rect_pos.x < out_rect_size.x && rect_pos.y < out_rect_size.y);
    if (inside_rect && src_pos.x < src_size.x && src_pos.y < src_size.y) {
        src_rgb = source_image.Load(int3(src_pos, 0)).rgb;
    }
    s_rgb[GTid.y][GTid.x + 1] = src_rgb;

    // Left halo column, needed by the chroma tent of the leftmost 2x2 block.
    // At the rect's left edge we replicate the first column, which is exactly what
    // the pixel shader's CLAMP sampler does.
    if (GTid.x == 0u) {
        float3 halo_rgb = src_rgb;
        int halo_x = rect_pos.x - 1;
        if (halo_x >= 0 && halo_x < src_size.x && inside_rect && src_pos.y < src_size.y) {
            halo_rgb = source_image.Load(int3(halo_x, src_pos.y, 0)).rgb;
        }
        s_rgb[GTid.y][0] = halo_rgb;
    }

    GroupMemoryBarrierWithGroupSync();

#ifdef HDR_ANALYSIS_SNAPSHOT
    // One thread per analysis cell scans every output pixel assigned to that cell.
    // This preserves true extrema and average while keeping the snapshot capped.
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
                float3 cell_rgb = (x == uint(rect_pos.x) && y == uint(rect_pos.y))
                    ? src_rgb
                    : source_image.Load(int3(uint2(x, y), 0)).rgb;
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
    // Only threads at even (gx, gy) within the tile emit a UV pixel.
    if ((GTid.x & 1u) == 0u && (GTid.y & 1u) == 0u) {
        if (rect_pos.x >= out_rect_size.x || rect_pos.y >= out_rect_size.y) {
            return;
        }

        // Type-0 chroma siting (the H.264/HEVC default when chroma_sample_loc is
        // unspecified): horizontally co-sited with the left luma column, vertically
        // centred between the two luma rows. That is a [1/4, 1/2, 1/4] horizontal
        // tent times a [1/2, 1/2] vertical average -- the same effective filter the
        // LEFT_SUBSAMPLING pixel shader gets from its two bilinear taps. A plain 2x2
        // box would instead sit half a luma pixel to the right.
        uint lx = GTid.x;       // pixel at rect_pos.x - 1 (halo column when GTid.x == 0)
        uint cx = GTid.x + 1u;  // pixel at rect_pos.x
        // Replicate at the right/bottom edges, matching the clamped sampler.
        uint rx = (rect_pos.x + 1 < out_rect_size.x) ? (GTid.x + 2u) : cx;
        uint ty = GTid.y;
        uint by = (rect_pos.y + 1 < out_rect_size.y) ? (GTid.y + 1u) : ty;

        float3 rgb_avg = (s_rgb[ty][lx] + s_rgb[by][lx]) * 0.125 +
                         (s_rgb[ty][cx] + s_rgb[by][cx]) * 0.25 +
                         (s_rgb[ty][rx] + s_rgb[by][rx]) * 0.125;

        float3 rgb_uv = CONVERT_FUNCTION(rgb_avg);
        float u = dot(color_vec_u.xyz, rgb_uv) + color_vec_u.w;
        float v = dot(color_vec_v.xyz, rgb_uv) + color_vec_v.w;
        u = u * range_uv.x + range_uv.y;
        v = v * range_uv.x + range_uv.y;

        int2 uv_dst = (rect_pos + out_rect_offset) >> 1;
        uv_plane_uav[uint2(uv_dst)] = float2(u, v);
    }
}
