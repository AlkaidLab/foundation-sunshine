#pragma once
#include <windows.h>
#include <cstdint>
constexpr uint32_t kSlots = 4096, kSamples = 2048, kMagic = 0x52534153;
constexpr DWORD kMonoRva = 0x2fdc8, kImageSize = 0x1ef000, kTimestamp = 0x0627570d;
#pragma pack(push, 1)
struct Record {
    uint32_t magic, frames;
    uint64_t host;
    int64_t qpc;
    uint32_t objectId, type;
    float x, y, z, volume, callGain;
    uint32_t tid;
    uint64_t sequence;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 64);
struct Slot { Record record; float samples[kSamples]; };
struct StatefulSlot {
    Record record;
    float samples[kSamples];
    uint32_t propertyFlags, reserved;
    uint64_t lossEpoch;
};
// Experimental v3: raw object packets AND an explicit encoder Process boundary.
// Never reinterpret this as disk-v2 StateRecord or as a legacy ring slot.
struct QuantumSlot {
    Record record;
    float samples[kSamples];
    uint32_t propertyFlags, kind;
    uint64_t lossEpoch, encoder;
    uint32_t inputFrames;
    int32_t result;
    uint32_t producedBytes, capacity;
};
#pragma pack(push, 1)
struct QuantumRecord {
    Record audio;
    uint32_t propertyFlags, kind;
    uint64_t lossEpoch, encoder;
    uint32_t inputFrames;
    int32_t result;
    uint32_t producedBytes, capacity;
    uint64_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(QuantumRecord) == 112);
constexpr uint32_t kQuantumObject=1, kQuantumProcess=2;
template<class S> struct alignas(64) RingT {
    uint32_t magic, version, collectorPid, enginePid;
    volatile LONG status, stop, error, writer;
    // Occupies the original 32 bytes of alignment padding (v2 ABI unchanged).
    volatile LONG64 renderCalls;
    uint64_t encoderRva;
    volatile LONG encoderReady;
    uint32_t backend;
    volatile LONG64 extendedCalls;
    alignas(64) volatile LONG64 write;
    alignas(64) volatile LONG64 read;
    alignas(64) volatile LONG64 heartbeat;
    alignas(64) volatile LONG64 calls, droppedFull, droppedBusy, invalid, silent;
    volatile LONG64 frames, copyTicks, maxCopyTicks, highWater;
    int64_t qpcFrequency;
    S slots[kSlots];
};
using LegacyRing = RingT<Slot>;
using StatefulRing = RingT<StatefulSlot>;
using QuantumRing = RingT<QuantumSlot>;
#ifdef QUANTUM_CAPTURE
using Ring = QuantumRing;
#elif defined(STATEFUL_CAPTURE)
using Ring = StatefulRing;
#else
using Ring = LegacyRing;
#endif
static_assert(offsetof(LegacyRing, slots) == offsetof(StatefulRing, slots));
static_assert(offsetof(LegacyRing, slots) == offsetof(QuantumRing, slots));
struct StartArgs { HANDLE mapping, collector; };
