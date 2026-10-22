#pragma once
#include "protocol.h"
namespace q4 {
constexpr uint32_t version=4, backend=5, magic=0x3451424f, domainCapacity=8, objectCapacity=64;
enum Kind : uint32_t { DomainBegin=1,DomainEnd,PassBegin,ObjectBefore,SubmitResult,ProcessResult,PassEnd };
enum Flags : uint32_t { PreCall=1,PostCall=2,ObservedNotCreated=4,NullPcm=8 };
enum Reason : uint32_t { Discovery=1,Reset,Destroy,BindingChanged,CaptureStop };
#pragma pack(push,1)
struct Header {
    Record audio;
    uint32_t version,kind,properties,flags;
    uint64_t lossEpoch,renderer,encoder,implementation,domain,pass,call;
    uint32_t inputFrames,producedBytes,capacity;
    int32_t result;
    uint32_t reason,reserved;
};
#pragma pack(pop)
static_assert(sizeof(Header)==160);
struct Slot {Header header;float pcm[kSamples];};
#if defined(_MSC_VER)
// Same compiler/header for both ends. Existing v1/v2/v3 layouts are untouched.
// The MinGW offline bridge consumes Header/files only, never this IPC layout.
struct alignas(64) Ring : RingT<Slot> {
    alignas(64) volatile LONG64 ignoredBeforeFirstRender,unboundAfterArming;
    volatile LONG64 domainsOpened,domainsClosed,submitResults,processResults;
};
static_assert(offsetof(Ring,write)==64);
static_assert(offsetof(Ring,slots)==offsetof(LegacyRing,slots));
#endif
}
