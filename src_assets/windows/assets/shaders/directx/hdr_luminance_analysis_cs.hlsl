/**
 * @file hdr_luminance_analysis_cs.hlsl
 * @brief GPU compute shader for per-frame HDR luminance analysis.
 *
 * Analyzes captured scRGB FP16 frames to extract per-frame luminance statistics
 * for generating accurate HDR dynamic metadata (CUVA HDR Vivid / HDR10+).
 *
 * Input: scRGB FP16 texture (R16G16B16A16_FLOAT)
 *   - scRGB uses BT.709 primaries in linear light
 *   - 1.0 in scRGB = 80 nits (SDR reference white)
 *
 * Output: Per-group scalar reductions in a structured buffer, plus a frame-global
 *   histogram accumulated by atomics.
 *   Each thread group (16x16 = 256 threads) processes one tile, writes
 *   {min, max, sum, count} of maxRGB values (in nits) to the output buffer, and folds
 *   its local 128-bin histogram into the global one with one atomic per occupied bin.
 *   A second-pass shader reduces the per-tile scalars to one result.
 *
 * Histogram: 128 bins covering 0-10000 nits, each bin = 78.125 nits wide.
 *   Used for P95/P99 percentile computation for stable peak luminance.
 *
 * Thread group size: 16x16 = 256 threads
 * Dispatch: (ceil(analysisWidth/16), ceil(analysisHeight/16), 1)
 */

// scRGB to nits conversion factor
static const float SCRGB_NITS_PER_UNIT = 80.0;

// Histogram parameters
static const uint HISTOGRAM_BINS = 128;
static const float HISTOGRAM_MAX_NITS = 10000.0;
static const float NITS_PER_BIN = HISTOGRAM_MAX_NITS / HISTOGRAM_BINS;  // 78.125

// Input texture (scRGB FP16)
Texture2D<float4> inputTexture : register(t0);
SamplerState linearSampler : register(s0);

cbuffer AnalysisParams : register(b0) {
    uint analysisWidth;
    uint analysisHeight;
    uint2 _pad;
};

// Per-group reduction results (scalars only — the histogram goes straight to the
// global accumulator below, so it is not carried per tile)
struct GroupResult {
    float minMaxRGB;                  // Minimum of max(R,G,B) in nits
    float maxMaxRGB;                  // Maximum of max(R,G,B) in nits
    float sumMaxRGB;                  // Sum of max(R,G,B) in nits (for average)
    uint  pixelCount;                 // Number of valid pixels processed
};

RWStructuredBuffer<GroupResult> groupResults : register(u0);

// Frame-global 128-bin histogram. Each group folds its local histogram in with one
// atomic per *occupied* bin, so pass 2 never has to merge per-tile histograms.
// Cleared by the host before every dispatch.
RWBuffer<uint> globalHistogram : register(u1);

// Shared memory for intra-group parallel reduction
groupshared float gs_min[256];
groupshared float gs_max[256];
groupshared float gs_sum[256];
groupshared uint  gs_count[256];
groupshared uint  gs_histogram[HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void main_cs(uint3 DTid : SV_DispatchThreadID,
             uint3 GTid : SV_GroupThreadID,
             uint3 Gid  : SV_GroupID,
             uint  GIndex : SV_GroupIndex)
{
    // Initialize shared histogram bins (each thread zeroes ~1 bin, 256 threads > 128 bins)
    if (GIndex < HISTOGRAM_BINS) {
        gs_histogram[GIndex] = 0;
    }

    // Compute maxRGB for this analysis sample
    float maxRGB_nits = 0.0;
    bool valid = (DTid.x < analysisWidth && DTid.y < analysisHeight);

    if (valid) {
        // Sample the full-resolution frame on a lower-resolution analysis grid.
        float2 uv = (float2(DTid.xy) + 0.5) / float2(analysisWidth, analysisHeight);
        float4 pixel = inputTexture.SampleLevel(linearSampler, uv, 0.0);

        // maxRGB = max(R, G, B) — the brightest channel per pixel
        // This is the key statistic used by CUVA HDR Vivid and HDR10+
        float maxRGB = max(max(pixel.r, pixel.g), pixel.b);

        // Clamp negative values (out-of-gamut in scRGB)
        maxRGB = max(maxRGB, 0.0);

        // Convert to nits: scRGB 1.0 = 80 nits
        maxRGB_nits = maxRGB * SCRGB_NITS_PER_UNIT;
    }

    // Initialize shared memory for min/max/sum/count reduction
    gs_min[GIndex] = valid ? maxRGB_nits : 100000.0;  // Large sentinel for min
    gs_max[GIndex] = valid ? maxRGB_nits : 0.0;
    gs_sum[GIndex] = valid ? maxRGB_nits : 0.0;
    gs_count[GIndex] = valid ? 1u : 0u;

    GroupMemoryBarrierWithGroupSync();

    // Accumulate into shared histogram using atomic add
    if (valid) {
        uint bin = min((uint)(maxRGB_nits / NITS_PER_BIN), HISTOGRAM_BINS - 1);
        InterlockedAdd(gs_histogram[bin], 1);
    }

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction for min/max/sum/count (log2(256) = 8 steps)
    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (GIndex < stride) {
            gs_min[GIndex] = min(gs_min[GIndex], gs_min[GIndex + stride]);
            gs_max[GIndex] = max(gs_max[GIndex], gs_max[GIndex + stride]);
            gs_sum[GIndex] += gs_sum[GIndex + stride];
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
