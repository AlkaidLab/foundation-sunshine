/**
 * @file hdr_luminance_reduce_cs.hlsl
 * @brief Second-pass GPU reduction shader for HDR luminance analysis.
 *
 * Reduces per-group scalar results from the first-pass analysis shader into a single
 * final result. The 128-bin histogram is not merged here — pass 1 already accumulated
 * it into a frame-global buffer via atomics, so this shader only copies it out.
 *
 * Input:  StructuredBuffer of GroupResult from first pass (N groups, 16 bytes each)
 *         RWBuffer of the frame-global 128-bin histogram
 * Output: RWStructuredBuffer with 1 FinalResult containing:
 *         - Global min/max/sum/count
 *         - The merged 128-bin histogram
 *
 * Dispatch: (1, 1, 1) — single thread group of 256 threads
 * Threads walk the group array with a grid-stride loop so the loads coalesce.
 *
 * cbuffer provides numGroups so the shader knows how many to reduce.
 */

static const uint HISTOGRAM_BINS = 128;

struct GroupResult {
    float minMaxRGB;
    float maxMaxRGB;
    float sumMaxRGB;
    uint  pixelCount;
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

// Frame-global histogram already accumulated by pass 1's atomics — just copied out here.
RWBuffer<uint> globalHistogram : register(u1);

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

[numthreads(256, 1, 1)]
void main_cs(uint GIndex : SV_GroupIndex)
{
    float local_min = 100000.0;
    float local_max = 0.0;
    float local_sum = 0.0;
    uint  local_count = 0;

    // Grid-stride loop: at each step the 256 threads read 256 *consecutive* GroupResults,
    // so the loads coalesce. Partitioning into contiguous per-thread blocks instead would
    // make every thread in the wave touch a different cache line on every iteration.
    for (uint g = GIndex; g < numGroups; g += 256) {
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

    // Thread 0 writes the reduced scalars
    if (GIndex == 0) {
        finalResult[0].minMaxRGB = gs_min[0];
        finalResult[0].maxMaxRGB = gs_max[0];
        finalResult[0].sumMaxRGB = gs_sum[0];
        finalResult[0].pixelCount = gs_count[0];
    }

    // First 128 threads copy the already-merged global histogram into the readback struct
    if (GIndex < HISTOGRAM_BINS) {
        finalResult[0].histogram[GIndex] = globalHistogram[GIndex];
    }
}
