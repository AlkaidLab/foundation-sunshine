/**
 * @file hdr_luminance_analysis_cs.hlsl
 * @brief GPU compute shader for per-frame HDR luminance analysis.
 *
 * Analyzes captured scRGB FP16 frames to extract per-frame luminance statistics
 * for generating accurate HDR dynamic metadata (CUVA HDR Vivid / HDR10+).
 *
 * Input: either an scRGB FP16 frame or the converter's capped FP16 statistics
 *   snapshot. For the full-frame fallback, each analysis thread scans one disjoint
 *   integer cell so extrema and the average cover every source pixel.
 *
 * Output: Per-group scalar reductions in a structured buffer, plus a frame-global
 *   histogram accumulated by atomics. The PQ-domain sum is accumulated per pixel
 *   rather than taken from the histogram: the histogram is populated from one
 *   representative sample per cell, which describes the distribution the percentiles
 *   read but is not an exact mean.
 *   Each thread group (16x16 = 256 threads) processes one tile, writes
 *   {min, max, sum, sum of PQ, count} of maxRGB values to the output buffer, and folds
 *   its local PQ-domain histogram into the global one with one atomic per occupied bin.
 *   A second-pass shader reduces the per-tile scalars to one result.
 *
 * Histogram: 256 uniform bins in normalized PQ signal space.
 *   HDR Vivid variance is P90-P10 in PQ space; PQ-domain bins retain useful
 *   precision in dark regions that a linear-nits histogram would discard.
 *
 * Thread group size: 16x16 = 256 threads
 * Dispatch: (ceil(analysisWidth/16), ceil(analysisHeight/16), 1)
 */

#include "include/common.hlsl"
#include "include/hdr_pre_encode_transform.hlsl"

// scRGB to nits conversion factor
static const float SCRGB_NITS_PER_UNIT = 80.0;

// Histogram parameters
static const uint HISTOGRAM_BINS = 256;

// Input texture (scRGB FP16)
Texture2D<float4> inputTexture : register(t0);

// Per-cell average PQ-coded maxRGB, written alongside the cell statistics above.
// Only bound on the snapshot path; the full-frame fallback below computes its own.
Texture2D<float> cellPqAverage : register(t1);

cbuffer AnalysisParams : register(b0) {
    uint analysisWidth;
    uint analysisHeight;
    uint sourceWidth;
    uint sourceHeight;
    uint inputHasCellStatistics;
    float maxAnalysisNits;
    uint2 _pad;
};

// Per-group reduction results (scalars only — the histogram goes straight to the
// global accumulator below, so it is not carried per tile)
struct GroupResult {
    float minMaxRGB;                  // Minimum of max(R,G,B) in nits
    float maxMaxRGB;                  // Maximum of max(R,G,B) in nits
    float sumMaxRGB;                  // Sum of max(R,G,B) in nits (for average)
    float sumMaxRGB_PQ;               // Sum of PQ-coded max(R,G,B) (for HDR Vivid's average)
    uint  pixelCount;                 // Number of valid pixels processed
};

RWStructuredBuffer<GroupResult> groupResults : register(u0);

// Frame-global 256-bin histogram. Each group folds its local histogram in with one
// atomic per *occupied* bin, so pass 2 never has to merge per-tile histograms.
// Cleared by the host before every dispatch.
RWBuffer<uint> globalHistogram : register(u1);

// Shared memory for intra-group parallel reduction
groupshared float gs_min[256];
groupshared float gs_max[256];
groupshared float gs_sum[256];
groupshared float gs_sum_pq[256];
groupshared uint  gs_count[256];
groupshared uint  gs_histogram[HISTOGRAM_BINS];

float HdrAnalysisMaxRgbNits(float3 sc_rgb)
{
    float3 rec2020_nits = max(Rec709toRec2020(sc_rgb) * SCRGB_NITS_PER_UNIT, 0.0);
    return min(max(max(rec2020_nits.r, rec2020_nits.g), rec2020_nits.b), maxAnalysisNits);
}

[numthreads(16, 16, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID,
             uint3 GTid : SV_GroupThreadID,
             uint3 Gid  : SV_GroupID,
             uint  GIndex : SV_GroupIndex)
{
    // Initialize shared histogram bins (one bin per thread).
    if (GIndex < HISTOGRAM_BINS) {
        gs_histogram[GIndex] = 0;
    }

    float minMaxRGB_nits = 10000.0;
    float maxMaxRGB_nits = 0.0;
    float sumMaxRGB_nits = 0.0;
    float sumMaxRGB_pq = 0.0;
    float representativeMaxRGB_nits = 0.0;
    uint pixelCount = 0;
    bool valid = (DTid.x < analysisWidth && DTid.y < analysisHeight);

    if (valid) {
        uint2 analysisPosition = DTid.xy;
        uint2 sourceSize = uint2(sourceWidth, sourceHeight);
        uint2 analysisSize = uint2(analysisWidth, analysisHeight);
        uint2 cellBegin = (analysisPosition * sourceSize) / analysisSize;
        uint2 cellEnd = ((analysisPosition + 1) * sourceSize) / analysisSize;
        uint2 cellExtent = cellEnd - cellBegin;
        pixelCount = cellExtent.x * cellExtent.y;

        if (inputHasCellStatistics != 0) {
            // R/G/B/A = cell min/max/average/representative maxRGB nits.
            float4 cellStats = inputTexture.Load(int3(analysisPosition, 0));
            minMaxRGB_nits = cellStats.r;
            maxMaxRGB_nits = cellStats.g;
            sumMaxRGB_nits = cellStats.b * pixelCount;
            sumMaxRGB_pq = cellPqAverage.Load(int3(analysisPosition, 0)) * pixelCount;
            representativeMaxRGB_nits = cellStats.a;
        } else {
            // Full-frame fallback: scan a disjoint integer partition. The union of
            // all cells covers every source pixel exactly once, including non-integer
            // source-to-analysis ratios.
            for (uint y = cellBegin.y; y < cellEnd.y; ++y) {
                for (uint x = cellBegin.x; x < cellEnd.x; ++x) {
                    float3 pixel = inputTexture.Load(int3(uint2(x, y), 0)).rgb;
                    pixel = ApplyHdrPreEncodeTransform(pixel);
                    float maxrgb_nits = HdrAnalysisMaxRgbNits(pixel);
                    minMaxRGB_nits = min(minMaxRGB_nits, maxrgb_nits);
                    maxMaxRGB_nits = max(maxMaxRGB_nits, maxrgb_nits);
                    sumMaxRGB_nits += maxrgb_nits;
                    sumMaxRGB_pq += NitsToPQ(maxrgb_nits.xxx).x;
                }
            }

            uint2 representativePosition = (cellBegin + cellEnd - 1) / 2;
            float3 representative = inputTexture.Load(int3(representativePosition, 0)).rgb;
            representative = ApplyHdrPreEncodeTransform(representative);
            representativeMaxRGB_nits = HdrAnalysisMaxRgbNits(representative);
        }
    }

    // Initialize shared memory for min/max/sum/count reduction
    gs_min[GIndex] = valid ? minMaxRGB_nits : 10000.0;
    gs_max[GIndex] = valid ? maxMaxRGB_nits : 0.0;
    gs_sum[GIndex] = valid ? sumMaxRGB_nits : 0.0;
    gs_sum_pq[GIndex] = valid ? sumMaxRGB_pq : 0.0;
    gs_count[GIndex] = valid ? pixelCount : 0u;

    GroupMemoryBarrierWithGroupSync();

    // Accumulate into shared histogram using atomic add
    if (valid) {
        float maxRGB_pq = NitsToPQ(representativeMaxRGB_nits.xxx).x;
        uint bin = min((uint)(maxRGB_pq * HISTOGRAM_BINS), HISTOGRAM_BINS - 1);
        InterlockedAdd(gs_histogram[bin], pixelCount);
    }

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction for min/max/sum/count (log2(256) = 8 steps)
    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (GIndex < stride) {
            gs_min[GIndex] = min(gs_min[GIndex], gs_min[GIndex + stride]);
            gs_max[GIndex] = max(gs_max[GIndex], gs_max[GIndex + stride]);
            gs_sum[GIndex] += gs_sum[GIndex + stride];
            gs_sum_pq[GIndex] += gs_sum_pq[GIndex + stride];
            gs_count[GIndex] += gs_count[GIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Thread 0 writes the group's scalar result
    if (GIndex == 0) {
        // Compute flat group index
        uint dispatchWidth = (analysisWidth + 15) / 16;
        uint groupIndex = Gid.y * dispatchWidth + Gid.x;

        GroupResult result;
        result.minMaxRGB = gs_min[0];
        result.maxMaxRGB = gs_max[0];
        result.sumMaxRGB = gs_sum[0];
        result.sumMaxRGB_PQ = gs_sum_pq[0];
        result.pixelCount = gs_count[0];

        groupResults[groupIndex] = result;
    }

    // Fold this tile's histogram into the frame-global one. Only occupied bins need an
    // atomic: a 16x16 tile usually spans a handful of bins, not all 128, so in practice
    // this is a few atomics per group rather than one per bin.
    if (GIndex < HISTOGRAM_BINS && gs_histogram[GIndex] != 0) {
        InterlockedAdd(globalHistogram[GIndex], gs_histogram[GIndex]);
    }
}
