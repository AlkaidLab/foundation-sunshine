#pragma once
#include <windows.h>
#include <cstdint>
constexpr uint32_t kInstanceMagic=0x4d415449,kInstanceVersion=1,kInstanceEvents=96;
struct InstanceEvent {char label[80];int32_t hr;uint32_t component;uint64_t a,b;};
struct InstanceComponent {
    int32_t loadHr,factoryHr,activateHr[2],encoderQiHr[2];
    uint32_t preloaded,distinctCanonicalIdentities;
    uint64_t encoderVtableRvas[8];uint32_t encoderVtableOwners[8];
};
struct alignas(64) InstanceState {
    uint32_t magic,version;
    volatile LONG status,stop,error,eventCount; // 0=pending,1=running,2=diagnostic complete,-1=guard failure
    volatile LONG64 heartbeat;
    uint32_t hostPid,hostTid,guardPassed,eventsDropped;
    wchar_t vendorDirectory[MAX_PATH];
    InstanceComponent components[2];
    InstanceEvent events[kInstanceEvents];
};
struct InstanceArgs {HANDLE mapping,collector;};
static_assert(sizeof(InstanceState)<65536);
