#pragma once
#include <windows.h>
#include <cstdint>
#include "../resident/encoded_protocol.h"
constexpr uint32_t kLifeMagic=0x4d41544c,kLifeVersion=1,kLifeCases=2,kLifeBlocks=240,kLifeObjects=20;
constexpr uint32_t kLifeFrames=480,kLifeRecords=256,kLifeBytes=8*1024*1024,kLifeRequest=30720;
static_assert(kLifeRecords*kLifeRequest<=kLifeBytes);
struct LifeObject {uint32_t id,flags;float xyz[3];float pcm[kLifeFrames];};
struct LifeBlock {uint32_t count;LifeObject objects[kLifeObjects];};
static_assert(sizeof(LifeObject)==1940 && sizeof(LifeBlock)==38804);
struct LifeTiming {int64_t target,start,processStart,finish;uint32_t submissions,active;};
static_assert(sizeof(LifeTiming)==40);
struct LifeCase {
    uint32_t kind,blocks,paceMs;
    LifeBlock input[kLifeBlocks];
    int32_t createHr,mapperHr,initializeHr,submitHr,processHr;
    uint32_t initializationAttempted,initialized,submissions,processedBlocks,records,bytes;
    int64_t elapsedTicks;
    LifeTiming timing[kLifeBlocks];
    EncodedRecord outputRecords[kLifeRecords];
    BYTE output[kLifeBytes];
};
struct LifeEvent {char label[80];int32_t hr;uint32_t caseIndex;};
struct alignas(64) LifeState {
    uint32_t magic,version;
    volatile LONG status,stop,error,eventCount;
    volatile LONG64 heartbeat;
    uint32_t guardPassed,hostPid;
    wchar_t vendorDirectory[MAX_PATH];
    LifeEvent events[64];
    LifeCase cases[kLifeCases];
};
struct LifeArgs {HANDLE mapping,collector;};
static_assert(sizeof(LifeState)<40*1024*1024);
