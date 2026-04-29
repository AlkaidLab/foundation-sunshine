// Compute-shader RGB -> NV12/P010 converter (Phase 1: type0, no rotation/scaling).
//
// Each threadgroup writes a 16x16 block of Y plane + 8x8 block of UV plane.
// One source RGB sample per thread is cached in groupshared memory, then reused
// for both Y (per pixel) and UV (2x2 box filter, matching LEFT_SUBSAMPLING PS).
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

groupshared float3 s_rgb[16][16];

#define CS_TILE 16

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
    s_rgb[GTid.y][GTid.x] = src_rgb;

    GroupMemoryBarrierWithGroupSync();

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
        // Bounds check on the UV pixel: we need 2x2 source coverage.
        bool uv_inside = (rect_pos.x + 1 < out_rect_size.x) && (rect_pos.y + 1 < out_rect_size.y);
        // Allow edge UV pixels even when the second column/row is out-of-rect: just clamp the average.
        // Initialize to silence fxc X4000 (the else-branch returns, so the read is safe at runtime,
        // but the compiler's flow analysis cannot prove it through the early `return`).
        float3 rgb_avg = float3(0, 0, 0);
        if (uv_inside) {
            rgb_avg = (s_rgb[GTid.y    ][GTid.x    ] +
                       s_rgb[GTid.y    ][GTid.x + 1] +
                       s_rgb[GTid.y + 1][GTid.x    ] +
                       s_rgb[GTid.y + 1][GTid.x + 1]) * 0.25;
        }
        else if (rect_pos.x < out_rect_size.x && rect_pos.y < out_rect_size.y) {
            // Edge: fall back to a single sample.
            rgb_avg = s_rgb[GTid.y][GTid.x];
        }
        else {
            return;
        }

        float3 rgb_uv = CONVERT_FUNCTION(rgb_avg);
        float u = dot(color_vec_u.xyz, rgb_uv) + color_vec_u.w;
        float v = dot(color_vec_v.xyz, rgb_uv) + color_vec_v.w;
        u = u * range_uv.x + range_uv.y;
        v = v * range_uv.x + range_uv.y;

        int2 uv_dst = (rect_pos + out_rect_offset) >> 1;
        uv_plane_uav[uint2(uv_dst)] = float2(u, v);
    }
}
