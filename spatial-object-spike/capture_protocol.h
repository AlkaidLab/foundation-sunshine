#pragma once
#include <cstdint>

// Local-only fixed capacity SPSC ring. Producer never waits on disk or IPC.
constexpr uint32_t kTapSlots = 1024;
constexpr uint32_t kTapMaxSamples = 2048;
constexpr uint32_t kTapMagic = 0x53504154;

struct TapBlock {
    uint64_t sequence;
    uint64_t objectId;
    uint64_t frameOffset;
    int64_t qpc;
    uint32_t pid;
    uint32_t sampleRate;
    uint32_t frames;
    uint32_t objectType;
    float x, y, z, volume;
    float samples[kTapMaxSamples];
};

struct TapRing {
    uint32_t magic;
    uint32_t version;
    volatile LONG write;
    volatile LONG read;
    volatile LONG dropped;
    volatile LONG ready;
    volatile LONG deviceActivations;
    volatile LONG spatialActivations;
    volatile LONG streamActivations;
    volatile LONG objectActivations;
    volatile LONG bufferCalls;
    volatile LONG endCalls;
    volatile LONG patchFailures;
    TapBlock blocks[kTapSlots];
};
