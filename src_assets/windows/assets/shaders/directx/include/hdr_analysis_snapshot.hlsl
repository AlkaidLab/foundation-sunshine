// Optional low-resolution HDR analysis snapshot output for RGB->P010 compute shaders.
//
// The conversion dispatch already owns and samples the shared scRGB source. On analysis
// frames, one output thread per analysis texel stores its converted-content sample in a
// private FP16 texture. The expensive luminance reduction can then run after releasing
// the source keyed mutex without copying the full-resolution capture texture.

#ifdef HDR_ANALYSIS_SNAPSHOT

RWTexture2D<float4> hdr_analysis_snapshot_uav : register(u2);

cbuffer hdr_analysis_snapshot_cbuffer : register(b2) {
    uint2 hdr_analysis_snapshot_size;
    uint2 hdr_analysis_snapshot_pad;
};

void StoreHdrAnalysisSnapshot(int2 output_position, int2 output_size, float3 sc_rgb, bool valid)
{
    if (!valid) {
        return;
    }

    uint2 position = uint2(output_position);
    uint2 extent = uint2(output_size);

    // Map each output pixel to an analysis cell, then allow only the output pixel
    // nearest that cell's center to write it. This fills every analysis texel exactly
    // once without another source-texture dispatch.
    uint2 analysis_position = min(
        ((position * 2 + 1) * hdr_analysis_snapshot_size) / (extent * 2),
        hdr_analysis_snapshot_size - 1
    );
    uint2 selected_output_position =
        ((analysis_position * 2 + 1) * extent) / (hdr_analysis_snapshot_size * 2);

    if (all(position == selected_output_position)) {
        hdr_analysis_snapshot_uav[analysis_position] = float4(sc_rgb, 1.0);
    }
}

#else

void StoreHdrAnalysisSnapshot(int2 output_position, int2 output_size, float3 sc_rgb, bool valid)
{
}

#endif
