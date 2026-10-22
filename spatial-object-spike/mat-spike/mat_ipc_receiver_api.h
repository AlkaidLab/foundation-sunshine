#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include "../resident/encoded_protocol.h"

namespace receiver {
constexpr uint32_t magic=0x3149504d,version=1,capacity=32,frameMagic=0x3146504d;
constexpr uint32_t callCapacity=64,frames=480,roundLimit=400,request=30720,byteLimit=roundLimit*request;
enum Error:LONG {none,badContract,identity,badFrame,checksumError,sequence,overflow,cancelled,producerExited,consumerExited,producerStale,consumerStale,missingEOF,codec,counters,timeout};
struct alignas(64) Contract {uint32_t magic,version,headerBytes,mapBytes,slotBytes,capacity,packetBytes,callBytes;uint64_t domain,epoch,nonce[2];uint32_t producerPid,rate,frames,reserved;int64_t qpcHz;};
struct alignas(64) Common {volatile LONG fault,cancel,producerStatus,consumerStatus,consumerPid,producerDone;};
struct alignas(64) Writer {volatile LONG64 write,heartbeat;};
struct alignas(64) Reader {volatile LONG64 read,heartbeat;};
#pragma pack(push,1)
struct Message {uint32_t magic,bytes;uint64_t sequence,domain,epoch;uint32_t index,frames,calls,kind;int64_t firstQpc,lastQpc,enqueuedQpc;uint32_t checksum,reserved;};
struct SourceHeader {char magic[8];uint32_t version,rate,frames,records;uint64_t hz;};
struct SourceFrame {uint32_t index,frames,calls,flags;int64_t firstQpc,lastQpc;};
#pragma pack(pop)
struct Call {uint32_t id,slot,type,flags;float xyz[3];uint32_t frames;float pcm[480];};
struct alignas(64) Slot {Message header;Call calls[callCapacity];};
struct alignas(64) Shared {Contract contract;Common common;Writer writer;Reader reader;Slot slots[capacity];};
static_assert(sizeof(Contract)==128&&offsetof(Shared,slots)==320&&sizeof(Message)==80&&sizeof(Call)==1952&&sizeof(Slot)==125056&&sizeof(Shared)==4002112);
static_assert(sizeof(SourceHeader)==32&&sizeof(SourceFrame)==32&&std::is_trivial_v<Shared>);
constexpr uint32_t outputMagic=0x31524f4d,apiVersion=1;
struct OutputHeader {
    uint32_t magic,version,bytes,kind; // kind 1=native, 2=fake; never infer MAT validity
    volatile LONG status,error;uint32_t hostGuardPassed,hostPid,workerTid;volatile LONG workerFinished;uint32_t workerTimedOut;
    uint32_t vendorLoads,activations,initializations,submissions,processes,sourceRounds,warmRounds,drainRounds;
    uint32_t messages,records,outputBytes,localHighWater,eofSeen,helperPinned;
    int32_t createHr,mapperHr,initializeHr,submitHr,processHr;
    uint64_t sourceSamples;uint32_t fakeCallsVerified,endpointCalls,hooksInstalled;
};
struct Tick {uint32_t phase,sourceIndex,count;int64_t startQpc,finishQpc;Call calls[callCapacity];};
struct NativeOutput {OutputHeader header;EncodedRecord records[roundLimit];BYTE bytes[byteLimit];};
struct FakeOutput {OutputHeader header;Tick ticks[roundLimit];};
static_assert(sizeof(FakeOutput)<64*1024*1024&&sizeof(NativeOutput)<16*1024*1024);
struct Args {
    uint32_t bytes,version;
    HANDLE input,data,cancel,owner,output;
    uint64_t domain,epoch,nonce[2],ownerCreationTime;
    uint32_t ownerPid,maxSourceRounds,maxSeconds,reserved;
};
struct FakeConfig {uint32_t bytes,version;int32_t failInitialize,failSubmitAt,failProcessAt;uint32_t oversizedProcess;};
using Start=DWORD(WINAPI*)(const Args*);
using FakeStart=DWORD(WINAPI*)(const Args*,const FakeConfig*);
inline LONG get(volatile LONG& v){return InterlockedCompareExchange(&v,0,0);}
inline LONG64 get(volatile LONG64& v){return InterlockedCompareExchange64(&v,0,0);}
inline void put(volatile LONG& v,LONG n){InterlockedExchange(&v,n);}
inline void put(volatile LONG64& v,LONG64 n){InterlockedExchange64(&v,n);}
inline int64_t qpc(){LARGE_INTEGER n;QueryPerformanceCounter(&n);return n.QuadPart;}
inline int64_t hz(){LARGE_INTEGER n;QueryPerformanceFrequency(&n);return n.QuadPart;}
inline void fail(Shared& s,LONG why){InterlockedCompareExchange(&s.common.fault,why,0);put(s.common.cancel,1);}
inline uint32_t fnv(uint32_t n,const void* data,size_t size){auto p=static_cast<const BYTE*>(data);for(size_t i=0;i<size;++i){n^=p[i];n*=16777619u;}return n;}
inline uint32_t checksum(Message m,const Call* calls){m.checksum=0;return fnv(fnv(2166136261u,&m,sizeof(m)),calls,m.calls*sizeof(Call));}
inline bool contract(const Shared& s,const Args& a){const auto& c=s.contract;return c.magic==magic&&c.version==version&&c.headerBytes==320&&c.mapBytes==sizeof(Shared)&&c.slotBytes==sizeof(Slot)&&c.capacity==capacity&&c.packetBytes==124968&&c.callBytes==sizeof(Call)&&c.rate==48000&&c.frames==480&&!c.reserved&&c.qpcHz==hz()&&c.domain==a.domain&&c.epoch==a.epoch&&c.nonce[0]==a.nonce[0]&&c.nonce[1]==a.nonce[1]&&c.producerPid==a.ownerPid;}
inline bool argsValid(const Args& a){return a.bytes==sizeof(a)&&a.version==apiVersion&&a.input&&a.data&&a.cancel&&a.owner&&a.output&&a.domain&&a.epoch&&a.ownerPid&&a.ownerCreationTime&&a.maxSourceRounds>0&&a.maxSourceRounds<=350&&a.maxSeconds>0&&a.maxSeconds<=10&&!a.reserved;}
} // namespace receiver
