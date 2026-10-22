#define NOMINMAX
#include <windows.h>
#include <cstring>
#include "protocol.h"
#include "encoded_protocol.h"
#if defined(MAT_LIVE_TAP)
#include <MinHook.h>
#else
#include "vendor/minhook-1.3.4/include/MinHook.h"
#endif
#if defined(MAT_LIVE_TAP)
#include "../../src/spatial_audio/native_mat_guard.h"
#endif
// OFFLINE verified wrapper ABI: Process(this, inputFrames, capacity, output, bytes*).
// This wrapper strips inputFrames before invoking IAsarEncoder2::Process.
using Encode=HRESULT (__fastcall*)(void*,uint32_t,uint32_t,BYTE*,uint32_t*);
static Encode original;
static void* target;
static BYTE saved[16];
static HMODULE self;
static void* volatile current;
static volatile LONG active,running;
static bool created;
static LONG64 AtomicRead(volatile LONG64* p){return InterlockedCompareExchange64(p,0,0);}
static void Capture(EncodedRing* r,void* encoder,uint32_t frames,uint32_t capacity,BYTE* output,uint32_t* produced,HRESULT hr) {
    InterlockedIncrement64(&r->calls);
    if(FAILED(hr)) InterlockedIncrement64(&r->failures);
    if(InterlockedCompareExchange(&r->writer,1,0)) {InterlockedIncrement64(&r->dropped);return;}
    auto write=AtomicRead(&r->write),read=AtomicRead(&r->read);
    if(write-read>=kByteSlots) {InterlockedIncrement64(&r->dropped);InterlockedExchange(&r->writer,0);return;}
    bool copied=false;
    __try {
        uint32_t bytes=*produced;
        if(bytes>capacity || bytes>kByteCapacity || (bytes && !output)) InterlockedIncrement64(&r->invalid);
        else {
            auto& s=r->slots[write%kByteSlots];LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
            s.record={0x42595445,bytes,static_cast<uint64_t>(write),qpc.QuadPart,reinterpret_cast<uint64_t>(encoder),frames,capacity,bytes,GetCurrentThreadId(),hr,0,0};
            if(bytes) memcpy(s.data,output,bytes);
            InterlockedAdd64(&r->bytes,bytes);copied=true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {InterlockedIncrement64(&r->invalid);}
    if(copied) InterlockedExchange64(&r->write,write+1);
    InterlockedExchange(&r->writer,0);
}
static HRESULT __fastcall Hook(void* encoder,uint32_t frames,uint32_t capacity,BYTE* output,uint32_t* produced) {
    InterlockedIncrement(&active);
    HRESULT hr=original(encoder,frames,capacity,output,produced);
    auto* r=static_cast<EncodedRing*>(InterlockedCompareExchangePointer(&current,nullptr,nullptr));
    if(r && !r->stop) Capture(r,encoder,frames,capacity,output,produced,hr);
    InterlockedDecrement(&active);return hr;
}
extern "C" __declspec(dllexport) DWORD WINAPI StartCapture(void* data) {
    StartArgs args=*static_cast<StartArgs*>(data);
    if(InterlockedCompareExchange(&running,1,0)) {CloseHandle(args.mapping);CloseHandle(args.collector);return ERROR_BUSY;}
    auto* r=static_cast<EncodedRing*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(EncodedRing)));CloseHandle(args.mapping);
    if(!r) {CloseHandle(args.collector);InterlockedExchange(&running,0);return ERROR_INVALID_HANDLE;}
    DWORD error=0;auto* base=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto* name=wcsrchr(path,L'\\');name=name?name+1:path;
    const BYTE expected[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x49,0x08};
    if(_wcsicmp(name,L"audiodg.exe") || !base || r->magic!=kByteMagic || r->version!=1) error=ERROR_BAD_ENVIRONMENT;
#if defined(MAT_LIVE_TAP)
    if(!error && (!native_mat_guard::system_engine(GetCurrentProcess()) ||
        !native_mat_guard::loaded_audioeng(reinterpret_cast<HMODULE>(base)))) error=ERROR_REVISION_MISMATCH;
#endif
    if(!error) {
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->FileHeader.TimeDateStamp!=kTimestamp || nt->OptionalHeader.SizeOfImage!=kImageSize || memcmp(base+0x31e00,expected,8)) error=ERROR_REVISION_MISMATCH;
    }
    if(!error && !created) {
        HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pinned);
        auto s=MH_Initialize();if(s!=MH_OK) error=0x4000+s;
        if(!error) {
            target=base+0x31e00;memcpy(saved,target,16);
            s=MH_CreateHook(target,reinterpret_cast<void*>(Hook),reinterpret_cast<void**>(&original));
            if(s!=MH_OK) error=0x4100+s;else created=true;
        }
    }
    if(!error) {
        InterlockedExchangePointer(&current,r);
        auto s=MH_EnableHook(target);if(s!=MH_OK) error=0x4200+s;
    }
    if(!error) {
        InterlockedExchange(&r->status,1);
        while(!r->stop && WaitForSingleObject(args.collector,20)==WAIT_TIMEOUT && static_cast<LONG64>(GetTickCount64())-AtomicRead(&r->heartbeat)<5000) {}
    }
    InterlockedExchange(&r->stop,1);
    if(created) {auto s=MH_DisableHook(target);if(s!=MH_OK && !error) error=0x4300+s;}
    InterlockedExchangePointer(&current,nullptr);
    while(InterlockedCompareExchange(&active,0,0)) Sleep(1);
    if(original && memcmp(target,saved,16)) error=ERROR_WRITE_FAULT;
    r->error=error;InterlockedExchange(&r->status,error?-1:2);
    UnmapViewOfFile(r);CloseHandle(args.collector);InterlockedExchange(&running,0);return error;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void*) {
    if(reason==DLL_PROCESS_ATTACH) {self=instance;DisableThreadLibraryCalls(instance);}return TRUE;
}
