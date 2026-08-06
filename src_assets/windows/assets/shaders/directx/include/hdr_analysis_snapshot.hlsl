// Optional low-resolution HDR analysis snapshot output for RGB->P010 compute shaders.
//
// The conversion dispatch already owns the shared scRGB source. On analysis frames,
// one output thread per analysis cell scans that cell and stores:
//   R = minimum maxRGB nits, G = maximum maxRGB nits,
//   B = average maxRGB nits, A = representative maxRGB nits.
// The expensive reduction can then run after releasing the source keyed mutex without
// copying the full-resolution capture texture or losing extrema to point sampling.

#ifdef HDR_ANALYSIS_SNAPSHOT

RWTexture2D<float4> hdr_analysis_snapshot_uav : register(u2);

// Matches AnalysisParams so one buffer can serve this converter and pass 1.
// source_size and has_cell_statistics are intentionally unused by the converter.
cbuffer hdr_analysis_snapshot_cbuffer : register(b2) {
    uint2 hdr_analysis_snapshot_size;
    uint2 hdr_analysis_snapshot_source_size;
    uint hdr_analysis_snapshot_has_cell_statistics;
    float hdr_analysis_snapshot_max_nits;
    uint2 hdr_analysis_snapshot_pad;
};

float HdrAnalysisMaxRgbNits(float3 sc_rgb)
{
    float3 rec2020_nits = max(Rec709toRec2020(sc_rgb) * 80.0, 0.0);
    return min(
        max(max(rec2020_nits.r, rec2020_nits.g), rec2020_nits.b),
        hdr_analysis_snapshot_max_nits
    );
}

bool GetHdrAnalysisCell(
    int2 output_position,
    int2 output_size,
    out uint2 analysis_position,
    out uint2 cell_begin,
    out uint2 cell_end)
{
    uint2 position = uint2(output_position);
    uint2 extent = uint2(output_size);

    // This is the inverse of the floor-partition below:
    // [floor(i * extent / cells), floor((i + 1) * extent / cells)).
    analysis_position = min(
        (((position + 1) * hdr_analysis_snapshot_size) - 1) / extent,
        hdr_analysis_snapshot_size - 1
    );
    cell_begin = (analysis_position * extent) / hdr_analysis_snapshot_size;
    cell_end = ((analysis_position + 1) * extent) / hdr_analysis_snapshot_size;
    uint2 selected_output_position = (cell_begin + cell_end - 1) / 2;
    return all(position == selected_output_position);
}

void StoreHdrAnalysisCellStats(
    uint2 analysis_position,
    float min_maxrgb_nits,
    float max_maxrgb_nits,
    float sum_maxrgb_nits,
    uint pixel_count,
    float representative_maxrgb_nits)
{
    // Store the average rather than the sum because a cell sum can overflow FP16.
    // The compact snapshot intentionally accepts FP16 quantization (up to about
    // 8 nits ULP near 10,000 nits) instead of copying a full-resolution texture.
    hdr_analysis_snapshot_uav[analysis_position] = float4(
        min_maxrgb_nits,
        max_maxrgb_nits,
        sum_maxrgb_nits / max(pixel_count, 1u),
        representative_maxrgb_nits
    );
}

#endif
