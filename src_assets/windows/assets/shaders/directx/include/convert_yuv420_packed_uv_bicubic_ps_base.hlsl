Texture2D image : register(t0);
SamplerState def_sampler : register(s0);
// Point sampler used by high-quality resampling to avoid double-filtering.
SamplerState point_sampler : register(s1);

cbuffer color_matrix_cbuffer : register(b0) {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
};

// Optimized Mitchell-Netravali bicubic weight function (B=1/3, C=1/3)
// Branchless implementation using precomputed coefficients for better performance
// Better for HDR than Catmull-Rom as it reduces ringing artifacts
// while maintaining good sharpness
float bicubic_weight(float x) {
    x = abs(x);
    
    // Precomputed coefficients for B=1/3, C=1/3 (Mitchell-Netravali)
    // For x < 1.0: (7/6)*x^3 - 2*x^2 + (8/9)
    // For 1.0 <= x < 2.0: (-7/18)*x^3 + 2*x^2 - (10/3)*x + (16/9)
    // For x >= 2.0: 0
    
    // Use step() and lerp() to avoid branches
    // step(a, x) returns 1.0 if x >= a, else 0.0
    float in_range_1 = 1.0 - step(1.0, x);  // 1.0 if x < 1.0, else 0.0
    float in_range_2 = step(1.0, x) * (1.0 - step(2.0, x));  // 1.0 if 1.0 <= x < 2.0, else 0.0
    
    // Calculate both polynomial branches
    float x2 = x * x;
    float x3 = x2 * x;
    
    // Branch 1: x < 1.0
    float weight1 = (7.0 / 6.0) * x3 - 2.0 * x2 + (8.0 / 9.0);
    
    // Branch 2: 1.0 <= x < 2.0
    float weight2 = (-7.0 / 18.0) * x3 + 2.0 * x2 - (10.0 / 3.0) * x + (16.0 / 9.0);
    
    // Select result based on range (branchless)
    return in_range_1 * weight1 + in_range_2 * weight2;
}

// Separable bicubic interpolation (optimized: weight calculations reduced from 32 to 8)
// First applies horizontal filtering (4 samples per row), then vertical (combines 4 rows)
// This reduces weight calculations by 75% (8 vs 32) while maintaining identical quality
// Texture fetches remain 16 (4x4 grid), but weight computation is optimized
float3 bicubic_sample(Texture2D tex, float2 uv, float2 texel_size) {
    // Get the base pixel coordinate
    float2 pixel_coord = uv / texel_size;
    float2 base_coord = floor(pixel_coord - 0.5) + 0.5;
    float2 frac_part = pixel_coord - base_coord;
    
    // Precompute horizontal weights (used for all 4 rows)
    float w_h0 = bicubic_weight(frac_part.x - (-1));
    float w_h1 = bicubic_weight(frac_part.x - 0);
    float w_h2 = bicubic_weight(frac_part.x - 1);
    float w_h3 = bicubic_weight(frac_part.x - 2);
    
    // Precompute vertical weights
    float w_v0 = bicubic_weight(frac_part.y - (-1));
    float w_v1 = bicubic_weight(frac_part.y - 0);
    float w_v2 = bicubic_weight(frac_part.y - 1);
    float w_v3 = bicubic_weight(frac_part.y - 2);
    
    // Step 1: Horizontal filtering - sample 4 rows, each with 4 horizontal samples
    // Row -1
    float2 coord_y1 = (base_coord + float2(0, -1)) * texel_size;
    float3 row_m1 = tex.Sample(point_sampler, coord_y1 + float2(-1, 0) * texel_size).rgb * w_h0
                   + tex.Sample(point_sampler, coord_y1).rgb * w_h1
                   + tex.Sample(point_sampler, coord_y1 + float2(1, 0) * texel_size).rgb * w_h2
                   + tex.Sample(point_sampler, coord_y1 + float2(2, 0) * texel_size).rgb * w_h3;
    
    // Row 0
    float2 coord_y0 = base_coord * texel_size;
    float3 row_0 = tex.Sample(point_sampler, coord_y0 + float2(-1, 0) * texel_size).rgb * w_h0
                 + tex.Sample(point_sampler, coord_y0).rgb * w_h1
                 + tex.Sample(point_sampler, coord_y0 + float2(1, 0) * texel_size).rgb * w_h2
                 + tex.Sample(point_sampler, coord_y0 + float2(2, 0) * texel_size).rgb * w_h3;
    
    // Row 1
    float2 coord_y1_pos = (base_coord + float2(0, 1)) * texel_size;
    float3 row_1 = tex.Sample(point_sampler, coord_y1_pos + float2(-1, 0) * texel_size).rgb * w_h0
                 + tex.Sample(point_sampler, coord_y1_pos).rgb * w_h1
                 + tex.Sample(point_sampler, coord_y1_pos + float2(1, 0) * texel_size).rgb * w_h2
                 + tex.Sample(point_sampler, coord_y1_pos + float2(2, 0) * texel_size).rgb * w_h3;
    
    // Row 2
    float2 coord_y2 = (base_coord + float2(0, 2)) * texel_size;
    float3 row_2 = tex.Sample(point_sampler, coord_y2 + float2(-1, 0) * texel_size).rgb * w_h0
                 + tex.Sample(point_sampler, coord_y2).rgb * w_h1
                 + tex.Sample(point_sampler, coord_y2 + float2(1, 0) * texel_size).rgb * w_h2
                 + tex.Sample(point_sampler, coord_y2 + float2(2, 0) * texel_size).rgb * w_h3;
    
    // Step 2: Vertical filtering - combine the 4 horizontally-filtered rows
    float3 result = row_m1 * w_v0 + row_0 * w_v1 + row_1 * w_v2 + row_2 * w_v3;
    
    // Anti-ringing clamp: find min/max from the 4 rows to prevent overshoot/undershoot
    float3 min_rgb = min(min(row_m1, row_0), min(row_1, row_2));
    float3 max_rgb = max(max(row_m1, row_0), max(row_1, row_2));
    
    // Clamp result to prevent ringing artifacts (especially visible in HDR UI/text)
    return clamp(result, min_rgb, max_rgb);
}

struct bicubic_vertex_t {
    float4 viewpoint_pos : SV_Position;
    float2 tex_coord : TEXCOORD;
};

float2 main_ps(bicubic_vertex_t input) : SV_Target
{
    // Get texture dimensions for texel size calculation
    uint width, height;
    image.GetDimensions(width, height);
    float2 texel_size = float2(1.0 / width, 1.0 / height);
    
    // Use bicubic interpolation
    float3 rgb = bicubic_sample(image, input.tex_coord, texel_size);

    rgb = CONVERT_FUNCTION(rgb);

    float u = dot(color_vec_u.xyz, rgb) + color_vec_u.w;
    float v = dot(color_vec_v.xyz, rgb) + color_vec_v.w;

    u = u * range_uv.x + range_uv.y;
    v = v * range_uv.x + range_uv.y;

    return float2(u, v);
}
