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

// Mitchell-Netravali bicubic weight function (B=1/3, C=1/3)
// Better for HDR than Catmull-Rom as it reduces ringing artifacts
// while maintaining good sharpness
float bicubic_weight(float x) {
    x = abs(x);
    // Mitchell-Netravali with B=1/3, C=1/3
    // Precomputed coefficients:
    // p0 = (12 - 9B - 6C) / 6 = (12 - 3 - 2) / 6 = 7/6
    // p2 = (-18 + 12B + 6C) / 6 = (-18 + 4 + 2) / 6 = -12/6 = -2
    // p3 = (6 - 2B) / 6 = (6 - 2/3) / 6 = 16/18 = 8/9
    // q0 = (-B - 6C) / 6 = (-1/3 - 2) / 6 = -7/18
    // q1 = (6B + 30C) / 6 = (2 + 10) / 6 = 2
    // q2 = (-12B - 48C) / 6 = (-4 - 16) / 6 = -10/3
    // q3 = (8B + 24C) / 6 = (8/3 + 8) / 6 = 32/18 = 16/9
    const float B = 1.0 / 3.0;
    const float C = 1.0 / 3.0;
    
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x 
              + (-18.0 + 12.0 * B + 6.0 * C) * x * x 
              + (6.0 - 2.0 * B)) / 6.0;
    } else if (x < 2.0) {
        return ((-B - 6.0 * C) * x * x * x 
              + (6.0 * B + 30.0 * C) * x * x 
              + (-12.0 * B - 48.0 * C) * x 
              + (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}

// Bicubic interpolation using 4x4 sample grid
// Unrolled loop for better performance and compatibility
float3 bicubic_sample(Texture2D tex, float2 uv, float2 texel_size) {
    // Get the base pixel coordinate
    float2 pixel_coord = uv / texel_size;
    float2 base_coord = floor(pixel_coord - 0.5) + 0.5;
    float2 frac_part = pixel_coord - base_coord;
    
    float3 result = float3(0, 0, 0);
    float total_weight = 0.0;
    // Anti-ringing clamp (libplacebo-style): clamp filtered value to the sample neighborhood.
    float3 min_rgb = float3( 3.402823e+38,  3.402823e+38,  3.402823e+38);
    float3 max_rgb = float3(-3.402823e+38, -3.402823e+38, -3.402823e+38);
    
    // Sample 4x4 grid (unrolled for compatibility)
    // Row -1
    float2 sample_coord = (base_coord + float2(-1, -1)) * texel_size;
    float weight = bicubic_weight(frac_part.x - (-1)) * bicubic_weight(frac_part.y - (-1));
    float3 s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(0, -1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 0) * bicubic_weight(frac_part.y - (-1));
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(1, -1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 1) * bicubic_weight(frac_part.y - (-1));
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(2, -1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 2) * bicubic_weight(frac_part.y - (-1));
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    // Row 0
    sample_coord = (base_coord + float2(-1, 0)) * texel_size;
    weight = bicubic_weight(frac_part.x - (-1)) * bicubic_weight(frac_part.y - 0);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(0, 0)) * texel_size;
    weight = bicubic_weight(frac_part.x - 0) * bicubic_weight(frac_part.y - 0);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(1, 0)) * texel_size;
    weight = bicubic_weight(frac_part.x - 1) * bicubic_weight(frac_part.y - 0);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(2, 0)) * texel_size;
    weight = bicubic_weight(frac_part.x - 2) * bicubic_weight(frac_part.y - 0);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    // Row 1
    sample_coord = (base_coord + float2(-1, 1)) * texel_size;
    weight = bicubic_weight(frac_part.x - (-1)) * bicubic_weight(frac_part.y - 1);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(0, 1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 0) * bicubic_weight(frac_part.y - 1);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(1, 1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 1) * bicubic_weight(frac_part.y - 1);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(2, 1)) * texel_size;
    weight = bicubic_weight(frac_part.x - 2) * bicubic_weight(frac_part.y - 1);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    // Row 2
    sample_coord = (base_coord + float2(-1, 2)) * texel_size;
    weight = bicubic_weight(frac_part.x - (-1)) * bicubic_weight(frac_part.y - 2);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(0, 2)) * texel_size;
    weight = bicubic_weight(frac_part.x - 0) * bicubic_weight(frac_part.y - 2);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(1, 2)) * texel_size;
    weight = bicubic_weight(frac_part.x - 1) * bicubic_weight(frac_part.y - 2);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    sample_coord = (base_coord + float2(2, 2)) * texel_size;
    weight = bicubic_weight(frac_part.x - 2) * bicubic_weight(frac_part.y - 2);
    s = tex.Sample(point_sampler, sample_coord).rgb;
    min_rgb = min(min_rgb, s);
    max_rgb = max(max_rgb, s);
    result += s * weight;
    total_weight += weight;
    
    float3 filtered = result / max(total_weight, 0.0001);
    // Anti-ringing clamp: prevent overshoot/undershoot which is especially visible in HDR UI/text.
    return clamp(filtered, min_rgb, max_rgb);
}

struct bicubic_vertex_t {
    float4 viewpoint_pos : SV_Position;
    float2 tex_coord : TEXCOORD;
};

float main_ps(bicubic_vertex_t input) : SV_Target
{
    // Get texture dimensions for texel size calculation
    uint width, height;
    image.GetDimensions(width, height);
    float2 texel_size = float2(1.0 / width, 1.0 / height);
    
    // Use bicubic interpolation
    float3 rgb = bicubic_sample(image, input.tex_coord, texel_size);

    rgb = CONVERT_FUNCTION(rgb);

    float y = dot(color_vec_y.xyz, rgb) + color_vec_y.w;

    return y * range_y.x + range_y.y;
}
