#pragma once
#include <windows.h>
#include <cstdint>
#include "../resident/encoded_protocol.h"
constexpr uint32_t kEncodeMagic=0x4d415442,kEncodeVersion=2,kEncodeCases=2,kEncodeBlocks=100;
constexpr uint32_t kEncodeFrames=480,kEncodePrerollBlocks=20,kEncodeTailBlocks=8,kEncodeRecords=132,kEncodeBytes=4*1024*1024;
// Exact output request seen in the verified native 480-frame callback, not an arbitrary large drain.
constexpr uint32_t kEncodeRequestBytes=30720;
static_assert(kEncodeRecords*kEncodeRequestBytes<=kEncodeBytes);
struct MatInputObject {uint32_t id;float xyz[3];float pcm[kEncodeFrames];};
struct MatInputBlock {MatInputObject objects[2];};
static_assert(sizeof(MatInputObject)==1936 && sizeof(MatInputBlock)==3872);
struct MatEncodeCase {
    uint32_t kind,blocks;
    MatInputBlock input[kEncodeBlocks];
    int32_t createHr,mapperHr,initializeHr,submitHr,processHr;
    uint32_t initializationAttempted,initialized,submissions,processedBlocks,records,bytes;
    uint32_t processedPrerollBlocks,processedTailBlocks;
    int64_t elapsedTicks;
    EncodedRecord outputRecords[kEncodeRecords];
    BYTE output[kEncodeBytes];
};
struct MatEncodeEvent {char label[80];int32_t hr;uint32_t caseIndex;};
struct alignas(64) MatEncodeState {
    uint32_t magic,version;
    volatile LONG status,stop,error,eventCount;
    volatile LONG64 heartbeat;
    uint32_t guardPassed,hostPid;
    wchar_t vendorDirectory[MAX_PATH];
    MatEncodeEvent events[64];
    MatEncodeCase cases[kEncodeCases];
};
struct MatEncodeArgs {HANDLE mapping,collector;};
static_assert(sizeof(MatEncodeState)<10*1024*1024);
