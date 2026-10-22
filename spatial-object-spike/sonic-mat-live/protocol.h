#pragma once
#include "../resident/protocol.h"
namespace sonic_live {
constexpr uint32_t magic=0x314e4f53,version=1,capacity=64,objects=20,frames=480;
// Same compiler on both sides. No STL/pointers/allocators cross this mapping.
struct Object {
    uint64_t host;uint32_t id,type,offset,frames;float xyz[3],volume,gain;
    int32_t result;uint32_t ended,nullPcm;float pcm[sonic_live::frames];
};
struct Pass {
    uint64_t sequence,renderer;int64_t firstQpc,lastQpc;
    uint32_t deviceFrames,requestedFrames,rate,count,endCount;
    struct End {uint64_t host;uint32_t id,reserved;} ends[sonic_live::objects];
    Object objects[sonic_live::objects];
};
struct alignas(64) Ring {
    uint32_t magic,version;volatile LONG status,stop,error,writer;
    alignas(64) volatile LONG64 write,read,heartbeat;
    alignas(64) volatile LONG64 passes,monoCalls,unbound,dropped,invalid;
    alignas(64) Pass slots[capacity];
};
static_assert(sizeof(Ring)<4*1024*1024);
}
