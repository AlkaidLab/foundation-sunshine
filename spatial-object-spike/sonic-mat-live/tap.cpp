#define NOMINMAX
#include "protocol.h"
#include "../../src/spatial_audio/native_mat_guard.h"
#include <MinHook.h>
#include <cstring>
using Render=void(__fastcall*)(void*,uint32_t,void*,uint32_t,void*);
using Mono=HRESULT(__fastcall*)(void*,uint32_t,const float*,uint32_t,float);
using EndObject=HRESULT(__fastcall*)(void*);
static Render originalRender;static Mono originalMono;static EndObject originalEnd;
static void* targets[3]{};static BYTE saved[3][16]{};static bool created;
static HMODULE self;static void* volatile current;static volatile LONG active,running;
static __declspec(thread) sonic_live::Pass* pass;
static LONG64 Read(volatile LONG64* p){return InterlockedCompareExchange64(p,0,0);}
static sonic_live::Ring* Current(){return static_cast<sonic_live::Ring*>(InterlockedCompareExchangePointer(&current,nullptr,nullptr));}
static void Fault(sonic_live::Ring* r){if(r)InterlockedIncrement64(&r->invalid);}
static bool Metadata(sonic_live::Pass* p,void* renderer,uint32_t inputs,void* input){
    bool valid=false;__try {
        if(inputs!=1||!input)return false;
        auto* connection=*static_cast<BYTE**>(input);if(!connection)return false;
        memcpy(&p->deviceFrames,connection+8,4);
        auto* bytes=static_cast<BYTE*>(renderer);memcpy(&p->rate,bytes+0x48,4);memcpy(&p->requestedFrames,bytes+0x22c,4);
        valid=true;
    }__except(EXCEPTION_EXECUTE_HANDLER){}return valid;
}
static void Copy(sonic_live::Object& o,void* host,uint32_t offset,const float* pcm,uint32_t frames,float gain,sonic_live::Ring* ring){
    __try {
        o={};o.host=reinterpret_cast<uint64_t>(host);o.offset=offset;o.frames=frames;o.gain=gain;o.result=E_UNEXPECTED;
        const auto* h=static_cast<BYTE*>(host);memcpy(&o.volume,h+0x64,4);memcpy(&o.id,h+0x68,4);memcpy(&o.type,h+0x6c,4);memcpy(o.xyz,h+0x98,12);
        o.nullPcm=pcm==nullptr;if(frames>480){Fault(ring);return;}if(pcm)memcpy(o.pcm,pcm,frames*4);
    }__except(EXCEPTION_EXECUTE_HANDLER){Fault(ring);}
}
static HRESULT __fastcall HookMono(void* host,uint32_t offset,const float* pcm,uint32_t frames,float gain){
    InterlockedIncrement(&active);auto* ring=Current();sonic_live::Object* object=nullptr;
    if(ring){InterlockedIncrement64(&ring->monoCalls);
        if(pass){if(pass->count<sonic_live::objects){object=&pass->objects[pass->count++];Copy(*object,host,offset,pcm,frames,gain,ring);}else Fault(ring);}
        else if(!ring->stop)InterlockedIncrement64(&ring->unbound);
    }
    HRESULT result=E_UNEXPECTED;
    __try{result=originalMono(host,offset,pcm,frames,gain);}
    __finally{if(AbnormalTermination())Fault(ring);else if(object)object->result=result;InterlockedDecrement(&active);}
    return result;
}
static HRESULT __fastcall HookEnd(void* host){
    InterlockedIncrement(&active);auto* ring=Current();HRESULT result=E_UNEXPECTED;uint32_t id=UINT32_MAX;
    __try{memcpy(&id,static_cast<BYTE*>(host)+0x68,4);}__except(EXCEPTION_EXECUTE_HANDLER){Fault(ring);}
    __try{result=originalEnd(host);}
    __finally{
        if(AbnormalTermination())Fault(ring);
        else if(ring&&result==S_OK){
            if(pass){
                if(pass->endCount<sonic_live::objects)pass->ends[pass->endCount++]={reinterpret_cast<uint64_t>(host),id,0};else Fault(ring);
                for(uint32_t n=0;n<pass->count;++n)if(pass->objects[n].host==reinterpret_cast<uint64_t>(host))pass->objects[n].ended=1;
            }else if(!ring->stop)InterlockedIncrement64(&ring->unbound);
        }
        InterlockedDecrement(&active);
    }return result;
}
static void __fastcall HookRender(void* renderer,uint32_t inputs,void* input,uint32_t outputs,void* output){
    InterlockedIncrement(&active);auto* ring=Current();auto* previous=pass;bool owned=false;LONG64 w=0;
    if(ring&&!ring->stop){
        if(previous)Fault(ring);
        else if(InterlockedCompareExchange(&ring->writer,1,0))InterlockedIncrement64(&ring->dropped);
        else {
            owned=true;w=Read(&ring->write);auto r=Read(&ring->read);
            if(w<r||w-r>=sonic_live::capacity)InterlockedIncrement64(&ring->dropped);
            else {auto& p=ring->slots[w%sonic_live::capacity];p.sequence=w;p.renderer=reinterpret_cast<uint64_t>(renderer);p.count=p.endCount=0;
                LARGE_INTEGER now{};QueryPerformanceCounter(&now);p.firstQpc=now.QuadPart;
                if(Metadata(&p,renderer,inputs,input))pass=&p;else Fault(ring);
            }
        }
    }
    __try{originalRender(renderer,inputs,input,outputs,output);}
    __finally{
        if(AbnormalTermination())Fault(ring);
        else if(pass&&pass!=previous){LARGE_INTEGER now{};QueryPerformanceCounter(&now);pass->lastQpc=now.QuadPart;
            InterlockedIncrement64(&ring->passes);InterlockedExchange64(&ring->write,w+1);}
        pass=previous;if(owned)InterlockedExchange(&ring->writer,0);InterlockedDecrement(&active);
    }
}
extern "C" __declspec(dllexport) DWORD WINAPI StartCapture(void* data){
    StartArgs args=*static_cast<StartArgs*>(data);
    if(InterlockedCompareExchange(&running,1,0)){CloseHandle(args.mapping);CloseHandle(args.collector);return ERROR_BUSY;}
    auto* ring=static_cast<sonic_live::Ring*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(sonic_live::Ring)));CloseHandle(args.mapping);
    if(!ring){CloseHandle(args.collector);InterlockedExchange(&running,0);return ERROR_INVALID_HANDLE;}
    DWORD error=0;auto* base=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    constexpr DWORD rvas[]={0xf7880,0x433f0,0x79e20};
    const BYTE expected[][8]={{0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x44},{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48},{0x40,0x53,0x48,0x83,0xec,0x40,0x80,0x79}};
    if(!base||!args.collector||ring->magic!=sonic_live::magic||ring->version!=sonic_live::version||
        !native_mat_guard::system_engine(GetCurrentProcess())||!native_mat_guard::loaded_audioeng(reinterpret_cast<HMODULE>(base)))error=ERROR_REVISION_MISMATCH;
    for(unsigned n=0;n<3&&!error;++n)if(memcmp(base+rvas[n],expected[n],8))error=ERROR_REVISION_MISMATCH;
    if(!error&&!created){
        HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pinned))error=GetLastError();
        auto status=error?MH_ERROR_NOT_INITIALIZED:MH_Initialize();if(!error&&status!=MH_OK)error=0x6000+status;
        void* hooks[]={reinterpret_cast<void*>(HookRender),reinterpret_cast<void*>(HookMono),reinterpret_cast<void*>(HookEnd)};
        void** originals[]={reinterpret_cast<void**>(&originalRender),reinterpret_cast<void**>(&originalMono),reinterpret_cast<void**>(&originalEnd)};
        for(unsigned n=0;n<3&&!error;++n){targets[n]=base+rvas[n];memcpy(saved[n],targets[n],16);status=MH_CreateHook(targets[n],hooks[n],originals[n]);if(status!=MH_OK)error=0x6100+n*0x100+status;}
        if(!error)created=true;
    }
    if(!error){InterlockedExchangePointer(&current,ring);
        for(auto target:targets){auto status=MH_QueueEnableHook(target);if(status!=MH_OK&&!error)error=0x6500+status;}
        if(!error){auto status=MH_ApplyQueued();if(status!=MH_OK)error=0x6600+status;}
        if(!error){InterlockedExchange(&ring->status,1);
            while(!ring->stop&&WaitForSingleObject(args.collector,20)==WAIT_TIMEOUT&&static_cast<LONG64>(GetTickCount64())-Read(&ring->heartbeat)<5000){}
        }
    }
    InterlockedExchange(&ring->stop,1);
    // Drain admitted Process passes before disabling their nested Mono/EOS hooks.
    while(InterlockedCompareExchange(&ring->writer,0,0))Sleep(1);
    if(created){auto status=MH_DisableHook(MH_ALL_HOOKS);if(status!=MH_OK&&!error)error=0x6700+status;}
    InterlockedExchangePointer(&current,nullptr);while(InterlockedCompareExchange(&active,0,0))Sleep(1);
    for(unsigned n=0;n<3;++n)if(targets[n]&&memcmp(targets[n],saved[n],16))error=ERROR_WRITE_FAULT;
    if(ring->invalid||ring->unbound||ring->dropped)error=ERROR_INVALID_DATA;
    ring->error=error;InterlockedExchange(&ring->status,error?-1:2);UnmapViewOfFile(ring);CloseHandle(args.collector);InterlockedExchange(&running,0);return error;
}
#ifndef SONIC_TAP_TEST
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;}
#endif
