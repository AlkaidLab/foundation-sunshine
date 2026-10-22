#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "cpp_mat_stream.hpp"
#include <cstddef>
#include <type_traits>

namespace mat_ipc {
using namespace mat_stream;
constexpr uint32_t magic=0x3149504d,version=1,capacity=32,frameMagic=0x3146504d;
enum class Error:LONG {none,badContract,identity,badFrame,checksum,sequence,overflow,cancelled,
    producerExited,consumerExited,producerStale,consumerStale,missingEOF,codec,counters,timeout};
enum class Status:LONG {initial,attached,streaming,draining,completed,failed};
struct alignas(64) Contract {
    uint32_t magic,version,headerBytes,mapBytes,slotBytes,capacity,packetBytes,callBytes;
    uint64_t domain,epoch,nonce[2];
    uint32_t producerPid,rate,frames,reserved;
    int64_t qpcHz;
};
struct alignas(64) Common {volatile LONG fault,cancel,producerStatus,consumerStatus,consumerPid,producerDone;};
struct alignas(64) Writer {volatile LONG64 write,heartbeat;};
struct alignas(64) Reader {volatile LONG64 read,heartbeat;};
#pragma pack(push,1)
struct Message {
    uint32_t magic,bytes;
    uint64_t sequence,domain,epoch;
    uint32_t index,frames,calls,kind;
    int64_t firstQpc,lastQpc,enqueuedQpc;
    uint32_t checksum,reserved;
};
#pragma pack(pop)
struct WireCall {uint32_t id,slot,type,flags;float xyz[3];uint32_t frames;float pcm[480];};
struct alignas(64) Slot {Message header;WireCall calls[callCapacity];};
struct alignas(64) Shared {Contract contract;Common common;Writer writer;Reader reader;Slot slots[capacity];};
static_assert(sizeof(Message)==80&&sizeof(CallHeader)==32&&sizeof(Call)==1952&&sizeof(WireCall)==1952&&offsetof(Call,pcm)==32&&offsetof(WireCall,pcm)==32);
static_assert(sizeof(LONG)==4&&sizeof(LONG64)==8&&alignof(Shared)==64);
static_assert(std::is_trivial_v<Shared>&&std::is_trivially_copyable_v<Shared>&&std::is_standard_layout_v<Shared>);
static_assert(offsetof(Shared,writer)%64==0&&offsetof(Shared,reader)%64==0);
inline int64_t qpc(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
inline int64_t hz(){LARGE_INTEGER v;QueryPerformanceFrequency(&v);return v.QuadPart;}
inline LONG get(volatile LONG& v){return InterlockedCompareExchange(&v,0,0);}
inline LONG64 get(volatile LONG64& v){return InterlockedCompareExchange64(&v,0,0);}
inline void put(volatile LONG& v,LONG x){InterlockedExchange(&v,x);}
inline void put(volatile LONG64& v,LONG64 x){InterlockedExchange64(&v,x);}
inline Error error(Shared& s){return static_cast<Error>(get(s.common.fault));}
inline void fail(Shared& s,Error why){InterlockedCompareExchange(&s.common.fault,static_cast<LONG>(why),0);put(s.common.cancel,1);}
inline void initialize(Shared& s,uint64_t domain,uint64_t epoch,uint64_t n0,uint64_t n1){
    // Only before publishing/inheriting mapping; never reset a live ring in place.
    std::memset(&s,0,sizeof(s));
    s.contract={magic,version,static_cast<uint32_t>(offsetof(Shared,slots)),sizeof(Shared),sizeof(Slot),capacity,sizeof(Packet),sizeof(Call),domain,epoch,{n0,n1},GetCurrentProcessId(),48000,480,0,hz()};
    put(s.writer.heartbeat,qpc());
}
inline Error validate(const Contract& c,uint64_t domain,uint64_t epoch,uint64_t n0,uint64_t n1,DWORD owner){
    if(c.magic!=magic||c.version!=version||c.headerBytes!=offsetof(Shared,slots)||c.mapBytes!=sizeof(Shared)||c.slotBytes!=sizeof(Slot)||c.capacity!=capacity||c.packetBytes!=sizeof(Packet)||c.callBytes!=sizeof(Call)||c.rate!=48000||c.frames!=480||c.reserved||c.qpcHz!=hz())return Error::badContract;
    if(!domain||!epoch||!owner||c.domain!=domain||c.epoch!=epoch||c.nonce[0]!=n0||c.nonce[1]!=n1||c.producerPid!=owner)return Error::identity;
    return Error::none;
}
inline uint32_t fnv(uint32_t h,const void* ptr,size_t n){auto* p=static_cast<const uint8_t*>(ptr);for(size_t i=0;i<n;++i){h^=p[i];h*=16777619u;}return h;}
inline uint32_t checksum(Message m,const void* calls){m.checksum=0;return fnv(fnv(2166136261u,&m,sizeof(m)),calls,m.calls*sizeof(WireCall));}
inline bool counts(Shared& s,int64_t& write,int64_t& read){
    write=get(s.writer.write);read=get(s.reader.read);
    if(read<0||write<0||read>write||write-read>capacity||write==INT64_MAX){fail(s,Error::counters);return false;}return true;
}
// Single producer. Payload is published only by the Interlocked write advance.
// No wait/retry/allocation. Events and liveness live in the control adapter.
inline bool send(Shared& s,const Packet& p,int64_t when){
    if(error(s)!=Error::none||get(s.common.cancel)||get(s.common.producerDone))return false;
    int64_t w,r;if(!counts(s,w,r))return false;
    if(w-r==capacity){fail(s,Error::overflow);return false;}
    if(p.domain!=s.contract.domain||p.header.calls>callCapacity||p.header.frames!=480||p.header.flags>1){fail(s,Error::badFrame);return false;}
    auto& slot=s.slots[w%capacity];
    Message m{frameMagic,static_cast<uint32_t>(sizeof(Message)+p.header.calls*sizeof(Call)),static_cast<uint64_t>(w),s.contract.domain,s.contract.epoch,
              p.header.index,480,p.header.calls,p.header.flags?2u:1u,p.header.firstQpc,p.header.lastQpc,when,0,0};
    if(m.calls)std::memcpy(slot.calls,p.calls.data(),m.calls*sizeof(Call));m.checksum=checksum(m,slot.calls);slot.header=m;
    put(s.writer.write,w+1);put(s.writer.heartbeat,when);
    if(m.kind==2)put(s.common.producerDone,1);
    return true;
}
enum class Poll {item,empty,failed};
// Single consumer copies before publishing read. Never trusts wire lengths for
// a memcpy until all fixed-size/domain/epoch/sequence bounds have been checked.
inline Poll receive(Shared& s,Packet& p,int64_t& enqueued){
    if(error(s)!=Error::none||get(s.common.cancel))return Poll::failed;
    int64_t w,r;if(!counts(s,w,r))return Poll::failed;if(w==r)return Poll::empty;
    auto& slot=s.slots[r%capacity];Message m=slot.header;
    auto reject=[&](Error why){fail(s,why);return Poll::failed;};
    if(m.magic!=frameMagic||m.calls>callCapacity||m.bytes!=sizeof(Message)+m.calls*sizeof(Call)||m.frames!=480||(m.kind!=1&&m.kind!=2)||m.reserved||m.enqueuedQpc<=0)return reject(Error::badFrame);
    if(m.domain!=s.contract.domain||m.epoch!=s.contract.epoch)return reject(Error::identity);
    if(m.sequence!=static_cast<uint64_t>(r))return reject(Error::sequence);
    if(checksum(m,slot.calls)!=m.checksum)return reject(Error::checksum);
    p.domain=m.domain;p.header={m.index,480,m.calls,m.kind==2?1u:0u,m.firstQpc,m.lastQpc};
    if(m.calls)std::memcpy(p.calls.data(),slot.calls,m.calls*sizeof(Call));enqueued=m.enqueuedQpc;
    put(s.reader.read,r+1);put(s.reader.heartbeat,qpc());return Poll::item;
}
} // namespace mat_ipc
