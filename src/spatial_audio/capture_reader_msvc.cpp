#include "capture_reader_api.h"
#include "../../spatial-object-spike/resident/quantum_v4_protocol.h"
#include <new>
namespace sc=spatial_capture;
struct Reader {HANDLE map{},owner{};q4::Ring* ring{};uint64_t next=0;bool poisoned=false;~Reader(){if(ring)UnmapViewOfFile(ring);if(map)CloseHandle(map);if(owner)CloseHandle(owner);}};
static uint64_t born(HANDLE p){FILETIME a{},b{},c{},d{};if(!GetProcessTimes(p,&a,&b,&c,&d))return 0;return uint64_t(a.dwHighDateTime)<<32|a.dwLowDateTime;}
extern "C" __declspec(dllexport) uint32_t WINAPI SpatialCaptureCreate(const sc::Args* a,void** out){
    if(out)*out=nullptr;if(!a||!out||a->bytes!=sizeof(*a)||a->version!=sc::version||!a->mapping||!a->owner||!a->owner_pid||a->collector_pid!=GetCurrentProcessId()||!a->owner_created)return ERROR_INVALID_PARAMETER;
    auto r=new(std::nothrow) Reader;if(!r)return ERROR_OUTOFMEMORY;
    if(!DuplicateHandle(GetCurrentProcess(),a->mapping,GetCurrentProcess(),&r->map,0,FALSE,DUPLICATE_SAME_ACCESS)||!DuplicateHandle(GetCurrentProcess(),a->owner,GetCurrentProcess(),&r->owner,0,FALSE,DUPLICATE_SAME_ACCESS)){delete r;return ERROR_INVALID_HANDLE;}
    r->ring=static_cast<q4::Ring*>(MapViewOfFile(r->map,FILE_MAP_ALL_ACCESS,0,0,sizeof(q4::Ring)));
    if(!r->ring||GetProcessId(r->owner)!=a->owner_pid||born(r->owner)!=a->owner_created){delete r;return ERROR_INVALID_DATA;}
    auto& q=*r->ring;LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);
    if(q.magic!=kMagic||q.version!=4||q.collectorPid!=a->collector_pid||q.qpcFrequency!=hz.QuadPart||InterlockedCompareExchange64(&q.read,0,0)!=0){delete r;return ERROR_INVALID_DATA;}
    *out=r;return 0;
}
extern "C" __declspec(dllexport) uint32_t WINAPI SpatialCaptureNext(void* context,sc::Event* out){
    auto r=static_cast<Reader*>(context);if(!r||!out||out->bytes!=sizeof(*out)||out->version!=sc::version)return sc::failed;out->size=0;
    if(r->poisoned)return sc::failed;auto& q=*r->ring;InterlockedExchange64(&q.heartbeat,GetTickCount64());
    auto bad=[&](){r->poisoned=true;return uint32_t(sc::failed);};
    // A completed producer may exit only AFTER the consumer drains its ring.
    if(q.magic!=kMagic||q.version!=4||q.error||q.invalid||q.droppedFull||q.droppedBusy||q.unboundAfterArming)return bad();
    auto read=InterlockedCompareExchange64(&q.read,0,0),write=InterlockedCompareExchange64(&q.write,0,0);
    if(read<0||write<read||write-read>kSlots||uint64_t(read)!=r->next)return bad();
    if(read==write){if(q.status==2)return sc::ended;if(q.status<0||WaitForSingleObject(r->owner,0)!=WAIT_TIMEOUT)return bad();return sc::pending;}
    const auto& slot=q.slots[read%kSlots];const auto& h=slot.header;
    if(h.version!=4||h.audio.magic!=q4::magic||h.audio.sequence!=r->next||h.audio.frames>kSamples||h.lossEpoch||h.kind<1||h.kind>7||(h.kind!=q4::ObjectBefore&&h.audio.frames))return bad();
    out->size=sizeof(h)+h.audio.frames*4;memcpy(out->data,&h,sizeof(h));if(h.audio.frames)memcpy(out->data+sizeof(h),slot.pcm,h.audio.frames*4);
    // Release only AFTER copying to caller-owned storage.
    InterlockedExchange64(&q.read,read+1);++r->next;InterlockedExchange64(&q.heartbeat,GetTickCount64());return sc::event;
}
extern "C" __declspec(dllexport) void WINAPI SpatialCaptureDestroy(void* context){delete static_cast<Reader*>(context);}
BOOL WINAPI DllMain(HINSTANCE h,DWORD why,void*){if(why==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(h);return TRUE;}
