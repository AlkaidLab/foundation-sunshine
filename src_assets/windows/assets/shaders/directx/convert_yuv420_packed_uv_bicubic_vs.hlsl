cbuffer rotate_texture_steps_cbuffer : register(b1) {
    int rotate_texture_steps;
};

#include "include/base_vs.hlsl"

vertex_t main_vs(uint vertex_id : SV_VertexID)
{
    // For bicubic sampling, we don't need subsample_offset, just use standard texture coordinates
    return generate_fullscreen_triangle_vertex(vertex_id, float2(0, 0), rotate_texture_steps);
}
