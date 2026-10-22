#pragma once
#include <windows.h>
#include <cstdint>
constexpr uint32_t kByteMagic=0x42594153,kByteSlots=128,kByteCapacity=65536;
#pragma pack(push,1)
struct EncodedRecord {
    uint32_t magic,bytes;
    uint64_t sequence;
    int64_t qpc;
    uint64_t encoder;
    uint32_t inputFrames,capacity,produced,threadId;
    int32_t result;
    uint32_t flags;
    uint64_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(EncodedRecord)==64);
struct EncodedSlot {EncodedRecord record;BYTE data[kByteCapacity];};
struct alignas(64) EncodedRing {
    uint32_t magic,version;
    volatile LONG status,stop,error,writer;
    alignas(64) volatile LONG64 write,read,heartbeat;
    alignas(64) volatile LONG64 calls,dropped,invalid,failures,bytes;
    EncodedSlot slots[kByteSlots];
};
