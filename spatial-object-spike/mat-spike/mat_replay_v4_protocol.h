#pragma once
#include "mat_lifecycle_protocol.h" // Frozen EncodedRecord/lifecycle ABI types only.
constexpr uint32_t kReplayMagic=0x3452564d,kReplayVersion=1,kReplayCases=2,kReplayBlocks=400,kReplayObjects=20;
constexpr uint32_t kReplayFrames=480,kReplayRecords=400,kReplayRequest=30720,kReplayBytes=kReplayRecords*kReplayRequest;
struct ReplayBlock {uint32_t phase,sourceIndex,count;LifeObject objects[kReplayObjects];};
static_assert(sizeof(ReplayBlock)==38812);
struct ReplayCase {
    uint64_t domain;
    uint32_t blocks,sourceBlocks,prerollBlocks,drainBlocks,paceMs,prerollBefore;
    ReplayBlock input[kReplayBlocks];
    int32_t createHr,mapperHr,initializeHr,submitHr,processHr;
    uint32_t initializationAttempted,initialized,submissions,processedBlocks,records,bytes;
    int64_t elapsedTicks;
    LifeTiming timing[kReplayBlocks];
    EncodedRecord outputRecords[kReplayRecords];
    BYTE output[kReplayBytes];
};
struct alignas(64) ReplayState {
    uint32_t magic,version;volatile LONG status,stop,error,eventCount;volatile LONG64 heartbeat;
    uint32_t guardPassed,hostPid;wchar_t vendorDirectory[MAX_PATH];LifeEvent events[64];ReplayCase cases[kReplayCases];
};
struct ReplayArgs {HANDLE mapping,collector;};
static_assert(sizeof(ReplayState)<64*1024*1024);
