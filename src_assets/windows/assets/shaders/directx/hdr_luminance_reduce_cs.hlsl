/**
 * @file hdr_luminance_reduce_cs.hlsl
 * @brief Second-pass GPU reduction shader for HDR luminance analysis.
 *
 * Reduces per-group results from the first-pass analysis shader into a single
 * final result. This eliminates CPU iteration over thousands of groups.
 *
 * Input:  StructuredBuffer of GroupResult from first pass (N groups)
 * Output: RWStructuredBuffer with 1 FinalResult containing:
 *         - Global min/max/sum/count
 *         - Merged 128-bin histogram
 *
 * Dispatch: (1, 1, 1) — single thread group of 256 threads
 * Each thread processes ceil(N/256) groups.
 *
 * cbuffer provides numGroups so the shader knows how many to reduce.
 */

static const uint HISTOGRAM_BINS = 128;

struct GroupResult {
    float minMaxRGB;
    float maxMaxRGB;
    float sumMaxRGB;
    uint  pixelCount;
    uint  histogram[HISTOGRAM_BINS];
};

struct FinalResult {
    float minMaxRGB;
    float maxMaxRGB;
    float sumMaxRGB;
    uint  pixelCount;
    uint  histogram[HISTOGRAM_BINS];
};

// Input: per-group results from first pass (SRV)
StructuredBuffer<GroupResult> groupResults : register(t0);

// Output: single merged result (UAV)
RWStructuredBuffer<FinalResult> finalResult : register(u0);

// Number of groups to reduce
cbuffer ReduceParams : register(b0) {
    uint numGroups;
    uint3 _pad;
};

// Shared memory for parallel reduction
groupshared float gs_min[256];
groupshared float gs_max[256];
groupshared float gs_sum[256];
groupshared uint  gs_count[256];
groupshared uint  gs_histogram[HISTOGRAM_BINS];

[numthreads(256, 1, 1)]
void main_cs(uint GIndex : SV_GroupIndex)
{
    // Initialize histogram bins (256 threads > 128 bins, first 128 threads init)
    if (GIndex < HISTOGRAM_BINS) {
        gs_histogram[GIndex] = 0;
    }

    // Each thread sequentially processes its assigned groups
    float local_min = 100000.0;
    float local_max = 0.0;
    float local_sum = 0.0;
    uint  local_count = 0;

    // Distribute groups across 256 threads
    uint groupsPerThread = (numGroups + 255) / 256;
    uint startGroup = GIndex * groupsPerThread;
    uint endGroup = min(startGroup + groupsPerThread, numGroups);

    for (uint g = startGroup; g < endGroup; g++) {
        GroupResult gr = groupResults[g];
        if (gr.pixelCount > 0) {
            local_min = min(local_min, gr.minMaxRGB);
            local_max = max(local_max, gr.maxMaxRGB);
            local_sum += gr.sumMaxRGB;
            local_count += gr.pixelCount;
        }
    }

    gs_min[GIndex] = local_min;
    gs_max[GIndex] = local_max;
    gs_sum[GIndex] = local_sum;
    gs_count[GIndex] = local_count;

    GroupMemoryBarrierWithGroupSync();

    // Merge histogram bins: each of the first 128 threads handles one bin
    // across all groups (sequential accumulation per bin)
    if (GIndex < HISTOGRAM_BINS) {
        uint binSum = 0;
        for (uint g = 0; g < numGroups; g++) {
            binSum += groupResults[g].histogram[GIndex];
        }
        gs_histogram[GIndex] = binSum;
    }

    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction of min/max/sum/count (log2(256) = 8 steps)
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

    // Thread 0 writes final merged result (scalars only; histogram written below by 128 threads)
    if (GIndex == 0) {
        finalResult[0].minMaxRGB = gs_min[0];
        finalResult[0].maxMaxRGB = gs_max[0];
        finalResult[0].sumMaxRGB = gs_sum[0];
        finalResult[0].pixelCount = gs_count[0];
    }

    GroupMemoryBarrierWithGroupSync();

    // First 128 threads write histogram bins to output
    if (GIndex < HISTOGRAM_BINS) {
        finalResult[0].histogram[GIndex] = gs_histogram[GIndex];
    }
}
