#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include "protocol.h"
#include "vendor/minhook-1.3.4/include/MinHook.h"
#ifdef TRACE_PROPERTIES
#include "metadata_trace.h"
using Sanitize=HRESULT (__fastcall*)(const void*,uint32_t,uint32_t,void*);
static Sanitize originalSanitize;
static void* sanitizeTarget;
static BYTE savedSanitize[16];
static __declspec(thread) PropertyTrace threadTrace;
#endif
// Version-specific ABI derived offline from audioeng!MainPluginRenderer::Process.
// Bootstrap: IAudioProcessingObjectRT::APOProcess-style five-argument callback.
// Encoder vtable[2]: HRESULT(this, streamInfo, basicProperties, mono, frames, gain).
// Encoder vtable[3] takes extended metadata; counted but NOT decoded here.
static_assert(offsetof(Ring, write) == 64, "Preserve Sonic v2 queue ABI");
using Process = void (__fastcall*)(void*, uint32_t, void*, uint32_t, void*);
using Submit = HRESULT (__fastcall*)(void*, const void*, const void*, const float*, uint32_t, float);
using Extended = HRESULT (__fastcall*)(void*, const void*, const void*, const void*, const float*, uint32_t, float);
static Process originalProcess;
static Submit originalSubmit;
static Extended originalExtended;
static void* volatile currentRing;
static void* volatile candidate;
static volatile LONG active, running;
static void* bootstrap;
static void* submitTarget;
static void* extendedTarget;
static bool created;
static HMODULE self;
static BYTE savedSubmit[16], savedExtended[16];
#ifdef QUANTUM_CAPTURE
// Same matching offline PDB/PE as the object wrapper. This ABI is NOT a public API.
using Encode=HRESULT (__fastcall*)(void*,uint32_t,uint32_t,BYTE*,uint32_t*);
static Encode originalEncode;
static void* encodeTarget;
static BYTE savedEncode[16];
#endif
static volatile LONG64 lossEpoch;
static void Lost() {
#ifdef STATEFUL_CAPTURE
    InterlockedIncrement64(&lossEpoch);
#endif
}
static LONG64 Read64(volatile LONG64* p) { return InterlockedCompareExchange64(p, 0, 0); }
static void Maximum(volatile LONG64* p, LONG64 value) {
    LONG64 old = Read64(p); if (value > old) InterlockedCompareExchange64(p, value, old);
}
static void Capture(Ring* r, const void* info, const void* props, const float* samples, uint32_t frames, float gain, void* encoder=nullptr, HRESULT result=S_OK) {
#ifndef QUANTUM_CAPTURE
    (void)encoder;(void)result;
#endif
    LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin); InterlockedIncrement64(&r->calls);
    if (!frames || frames > kSamples) { InterlockedIncrement64(&r->invalid); Lost(); return; }
#ifdef TRACE_PROPERTIES
    if(frames>kTraceFloatOffset) { InterlockedIncrement64(&r->invalid);return; }
#endif
    if (InterlockedCompareExchange(&r->writer, 1, 0)) { InterlockedIncrement64(&r->droppedBusy); Lost(); return; }
    LONG64 write=Read64(&r->write), read=Read64(&r->read);
    if (write-read >= kSlots) { InterlockedIncrement64(&r->droppedFull); Lost(); InterlockedExchange(&r->writer, 0); return; }
    auto& s=r->slots[write % kSlots]; bool copied=false;
    __try {
        const BYTE* p=static_cast<const BYTE*>(props);
        s.record.magic=0x474f424a; s.record.frames=frames;
        memcpy(&s.record.host, static_cast<const BYTE*>(info)+4, 8);
        s.record.qpc=begin.QuadPart;
        memcpy(&s.record.objectId,p,4); memcpy(&s.record.type,p+8,4);
        memcpy(&s.record.x,p+0x10,16);
#ifdef STATEFUL_CAPTURE
        memcpy(&s.propertyFlags,p+0xc,4);
#ifndef QUANTUM_CAPTURE
        s.reserved=0;
#endif
        s.lossEpoch=Read64(&lossEpoch);
#endif
#ifdef QUANTUM_CAPTURE
        s.kind=kQuantumObject;s.encoder=reinterpret_cast<uint64_t>(encoder);
        s.inputFrames=0;s.result=result;s.producedBytes=0;s.capacity=0;
#endif
        s.record.callGain=gain; s.record.tid=GetCurrentThreadId(); s.record.sequence=write;
        if (samples) memcpy(s.samples,samples,frames*4);
        else { memset(s.samples,0,frames*4); InterlockedIncrement64(&r->silent); }
#ifdef TRACE_PROPERTIES
        PropertyTrace trace{}; TraceSubmit(trace,threadTrace,info,props);
        memcpy(s.samples+kTraceFloatOffset,&trace,sizeof(trace));
#endif
        copied=true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement64(&r->invalid); Lost(); }
    if (copied) { InterlockedAdd64(&r->frames,frames); Maximum(&r->highWater,write-read+1); InterlockedExchange64(&r->write,write+1); }
    InterlockedExchange(&r->writer,0); QueryPerformanceCounter(&end);
    LONG64 ticks=end.QuadPart-begin.QuadPart; InterlockedAdd64(&r->copyTicks,ticks); Maximum(&r->maxCopyTicks,ticks);
}
#ifdef QUANTUM_CAPTURE
static void CaptureBoundary(Ring* r,void* encoder,uint32_t frames,uint32_t capacity,uint32_t* produced,HRESULT result) {
    if(InterlockedCompareExchange(&r->writer,1,0)) {InterlockedIncrement64(&r->droppedBusy);Lost();return;}
    LONG64 write=Read64(&r->write),read=Read64(&r->read);bool copied=false;
    if(write-read>=kSlots) {InterlockedIncrement64(&r->droppedFull);Lost();InterlockedExchange(&r->writer,0);return;}
    __try {
        uint32_t bytes=produced?*produced:0;
        if(!produced || frames>kSamples || bytes>capacity) {InterlockedIncrement64(&r->invalid);Lost();}
        else {
            auto& s=r->slots[write%kSlots];LARGE_INTEGER time{};QueryPerformanceCounter(&time);
            s.record={};s.record.magic=0x474f424a;s.record.qpc=time.QuadPart;
            s.record.tid=GetCurrentThreadId();s.record.sequence=write;
            s.propertyFlags=0;s.kind=kQuantumProcess;s.lossEpoch=Read64(&lossEpoch);
            s.encoder=reinterpret_cast<uint64_t>(encoder);s.inputFrames=frames;
            s.result=result;s.producedBytes=bytes;s.capacity=capacity;copied=true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {InterlockedIncrement64(&r->invalid);Lost();}
    if(copied){Maximum(&r->highWater,write-read+1);InterlockedExchange64(&r->write,write+1);}
    InterlockedExchange(&r->writer,0);
}
static HRESULT __fastcall HookEncode(void* encoder,uint32_t frames,uint32_t capacity,BYTE* output,uint32_t* produced) {
    InterlockedIncrement(&active);
    HRESULT result=originalEncode(encoder,frames,capacity,output,produced);
    auto* r=static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing,nullptr,nullptr));
    if(r && !r->stop && encoder==InterlockedCompareExchangePointer(&candidate,nullptr,nullptr))
        CaptureBoundary(r,encoder,frames,capacity,produced,result);
    InterlockedDecrement(&active);return result;
}
#endif
#ifdef TRACE_PROPERTIES
static HRESULT __fastcall HookSanitize(const void* input,uint32_t bytes,uint32_t maxFrames,void* output) {
    InterlockedIncrement(&active);
    auto* r=static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing,nullptr,nullptr));
    bool trace=r && !r->stop;
    if(trace) {
        __try { TraceBefore(threadTrace,input,bytes,maxFrames,output); }
        __except(EXCEPTION_EXECUTE_HANDLER) { threadTrace.flags=0;InterlockedIncrement64(&r->invalid); }
    }
    HRESULT hr=originalSanitize(input,bytes,maxFrames,output);
    if(trace) {
        __try { TraceAfter(threadTrace,hr,output); }
        __except(EXCEPTION_EXECUTE_HANDLER) { threadTrace.flags=0;InterlockedIncrement64(&r->invalid); }
    }
    InterlockedDecrement(&active);return hr;
}
#endif
static void __fastcall HookProcess(void* renderer, uint32_t nIn, void* in, uint32_t nOut, void* out) {
    InterlockedIncrement(&active);
    auto* r=static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing,nullptr,nullptr));
    if(r && !r->stop) {
        InterlockedIncrement64(&r->renderCalls);
        if(!InterlockedCompareExchangePointer(&candidate,nullptr,nullptr)) {
            __try { InterlockedCompareExchangePointer(&candidate,*reinterpret_cast<void**>(static_cast<BYTE*>(renderer)+8),nullptr); }
            __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement64(&r->invalid); }
        }
    }
    originalProcess(renderer,nIn,in,nOut,out); InterlockedDecrement(&active);
}
static HRESULT __fastcall HookSubmit(void* encoder,const void* info,const void* props,const float* samples,uint32_t frames,float gain) {
    InterlockedIncrement(&active);
    auto* r=static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing,nullptr,nullptr));
#ifndef QUANTUM_CAPTURE
    if(r && !r->stop) Capture(r,info,props,samples,frames,gain,encoder);
#endif
    HRESULT hr=originalSubmit(encoder,info,props,samples,frames,gain);
#ifdef QUANTUM_CAPTURE
    // The known wrapper has const input buffers. Capture after return so a
    // rejected object cannot be mislabeled as an accepted S_OK submission.
    if(r && !r->stop && encoder==InterlockedCompareExchangePointer(&candidate,nullptr,nullptr))
        Capture(r,info,props,samples,frames,gain,encoder,hr);
#endif
    InterlockedDecrement(&active); return hr;
}
static HRESULT __fastcall HookExtended(void* encoder,const void* info,const void* props,const void* metadata,const float* samples,uint32_t frames,float gain) {
    InterlockedIncrement(&active);
    auto* r=static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing,nullptr,nullptr));
    if(r && !r->stop
#ifdef QUANTUM_CAPTURE
        && encoder==InterlockedCompareExchangePointer(&candidate,nullptr,nullptr)
#endif
        ) { InterlockedIncrement64(&r->extendedCalls); Lost(); }
    HRESULT hr=originalExtended(encoder,info,props,metadata,samples,frames,gain);
    InterlockedDecrement(&active); return hr;
}
static bool EncoderTargets(void* encoder, Ring* r) {
    void* a{}; void* b{};
    __try { auto** v=*static_cast<void***>(encoder); a=v[2]; b=v[3]; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    HMODULE ownerA{}, ownerB{};
    BOOL foundA=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(a),&ownerA);
    BOOL foundB=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(b),&ownerB);
    wchar_t otherPath[MAX_PATH]{}; GetModuleFileNameW(ownerB,otherPath,MAX_PATH);
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(ownerA,path,MAX_PATH); wchar_t* name=wcsrchr(path,L'\\'); name=name?name+1:path;
    // Before installing producer hooks, an empty slot can carry init diagnostics.
    // Collector reads this overlay only on failed initialization with write==0.
    swprintf_s(reinterpret_cast<wchar_t*>(r->slots[0].samples),1024,L"A=%s+0x%llX found=%d; B=%s+0x%llX found=%d",path,reinterpret_cast<uint64_t>(a)-reinterpret_cast<uint64_t>(ownerA),foundA,otherPath,reinterpret_cast<uint64_t>(b)-reinterpret_cast<uint64_t>(ownerB),foundB);
    if(!foundA || !foundB || ownerA!=ownerB) return false;
    bool directDolby=!_wcsicmp(name,L"DolbyMatEnc.dll") || !_wcsicmp(name,L"DolbyMatEngine.dll");
    const BYTE wrapperPrologue[]={0x48,0x83,0xec,0x48,0x48,0x8b,0x49,0x08};
    // Matching Microsoft PDB identifies these as AsarEncoderWrapper<IAsarEncoder2>
    // SetAudioObject / SetAudioObjectWithMetadataReader, with the same ABI.
    bool verifiedWrapper=!_wcsicmp(name,L"audioeng.dll") &&
        reinterpret_cast<BYTE*>(a)-reinterpret_cast<BYTE*>(ownerA)==0x2fd90 &&
        reinterpret_cast<BYTE*>(b)-reinterpret_cast<BYTE*>(ownerA)==0x45be0 &&
        !memcmp(a,wrapperPrologue,8) && !memcmp(b,wrapperPrologue,8);
    if(!directDolby && !verifiedWrapper) return false;
#ifdef STATEFUL_CAPTURE
    // Flags/stream identity are verified only for this Microsoft wrapper ABI.
    if(!verifiedWrapper) return false;
#endif
#ifdef QUANTUM_CAPTURE
    void* process{};
    __try {process=(*static_cast<void***>(encoder))[5];}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
    const BYTE processPrologue[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x49,0x08};
    if(static_cast<BYTE*>(process)-reinterpret_cast<BYTE*>(ownerA)!=0x31e00 || memcmp(process,processPrologue,8))return false;
    encodeTarget=process;memcpy(savedEncode,process,16);
#endif
    submitTarget=a; extendedTarget=b;
    memcpy(savedSubmit,a,16); memcpy(savedExtended,b,16); return true;
}
extern "C" __declspec(dllexport) DWORD WINAPI StartCapture(void* data) {
    StartArgs args=*static_cast<StartArgs*>(data);
    if(InterlockedCompareExchange(&running,1,0)) { CloseHandle(args.mapping); CloseHandle(args.collector); return ERROR_BUSY; }
    auto* r=static_cast<Ring*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Ring))); CloseHandle(args.mapping);
    if(!r) { CloseHandle(args.collector); InterlockedExchange(&running,0); return ERROR_INVALID_HANDLE; }
    DWORD error=0; wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH); wchar_t* name=wcsrchr(path,L'\\'); name=name?name+1:path;
    BYTE* base=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    const BYTE expected[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x10};
    const uint32_t version =
#ifdef QUANTUM_CAPTURE
        3;
#elif defined(STATEFUL_CAPTURE)
        2;
#else
        1;
#endif
    if(_wcsicmp(name,L"audiodg.exe") || !base || r->magic!=kMagic || r->version!=version) error=ERROR_BAD_ENVIRONMENT;
    if(!error) {
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base); auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->FileHeader.TimeDateStamp!=kTimestamp || nt->OptionalHeader.SizeOfImage!=kImageSize || memcmp(base+0xfa3b0,expected,7)) error=ERROR_REVISION_MISMATCH;
    }
    if(!error && !created) {
        HMODULE pinned{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pinned);
        auto status=MH_Initialize(); if(status!=MH_OK) error=0x3000+status;
        if(!error) {
            bootstrap=base+0xfa3b0;
            status=MH_CreateHook(bootstrap,reinterpret_cast<void*>(HookProcess),reinterpret_cast<void**>(&originalProcess));
            if(status!=MH_OK) error=0x3100+status; else created=true;
#ifdef TRACE_PROPERTIES
            if(!error) {
                const BYTE expectedSanitize[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c};
                sanitizeTarget=base+0x2a2bc;
                if(memcmp(sanitizeTarget,expectedSanitize,8)) error=ERROR_REVISION_MISMATCH;
                else {
                    memcpy(savedSanitize,sanitizeTarget,16);
                    status=MH_CreateHook(sanitizeTarget,reinterpret_cast<void*>(HookSanitize),reinterpret_cast<void**>(&originalSanitize));
                    if(status!=MH_OK) error=0x3700+status;
                }
            }
#endif
        }
    }
    if(!error) {
        InterlockedExchange64(&lossEpoch,0);
        r->backend=1; InterlockedExchangePointer(&candidate,nullptr); InterlockedExchangePointer(&currentRing,r);
#ifdef STATEFUL_CAPTURE
        r->backend=3;
#endif
#ifdef QUANTUM_CAPTURE
        r->backend=4;
#endif
#ifdef TRACE_PROPERTIES
        r->backend=2;
        MH_QueueEnableHook(bootstrap);MH_QueueEnableHook(sanitizeTarget);
        auto status=MH_ApplyQueued(); if(status!=MH_OK) error=0x3200+status;
#else
        auto status=MH_EnableHook(bootstrap); if(status!=MH_OK) error=0x3200+status;
#endif
    }
    if(!error) {
        InterlockedExchange(&r->status,1); bool installed=false;
        for(;;) {
            if(r->stop || WaitForSingleObject(args.collector,20)!=WAIT_TIMEOUT || static_cast<LONG64>(GetTickCount64())-Read64(&r->heartbeat)>5000) break;
            if(!installed) {
                void* encoder=InterlockedCompareExchangePointer(&candidate,nullptr,nullptr);
                if(encoder) {
                    if(!submitTarget) {
                        if(!EncoderTargets(encoder,r)) { error=ERROR_NOT_SUPPORTED; break; }
                        auto s=MH_CreateHook(submitTarget,reinterpret_cast<void*>(HookSubmit),reinterpret_cast<void**>(&originalSubmit));
                        if(s!=MH_OK) { error=0x3300+s; break; }
                        s=MH_CreateHook(extendedTarget,reinterpret_cast<void*>(HookExtended),reinterpret_cast<void**>(&originalExtended));
                        if(s!=MH_OK) { error=0x3400+s; break; }
#ifdef QUANTUM_CAPTURE
                        s=MH_CreateHook(encodeTarget,reinterpret_cast<void*>(HookEncode),reinterpret_cast<void**>(&originalEncode));
                        if(s!=MH_OK) {error=0x3800+s;break;}
#endif
                    }
                    MH_QueueEnableHook(submitTarget); MH_QueueEnableHook(extendedTarget);
#ifdef QUANTUM_CAPTURE
                    MH_QueueEnableHook(encodeTarget);
#endif
                    auto s=MH_ApplyQueued(); if(s!=MH_OK) { error=0x3500+s; break; }
                    HMODULE owner{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(submitTarget),&owner);
                    r->encoderRva=reinterpret_cast<uint64_t>(submitTarget)-reinterpret_cast<uint64_t>(owner);
                    InterlockedExchange(&r->encoderReady,1); installed=true;
                }
            }
        }
    }
    InterlockedExchange(&r->stop,1);
    if(created) { auto s=MH_DisableHook(MH_ALL_HOOKS); if(s!=MH_OK && !error) error=0x3600+s; }
    InterlockedExchangePointer(&currentRing,nullptr);
    while(InterlockedCompareExchange(&active,0,0)) Sleep(1);
    if(submitTarget && (memcmp(submitTarget,savedSubmit,16) || memcmp(extendedTarget,savedExtended,16))) error=ERROR_WRITE_FAULT;
#ifdef QUANTUM_CAPTURE
    if(encodeTarget && originalEncode && memcmp(encodeTarget,savedEncode,16))error=ERROR_WRITE_FAULT;
#endif
#ifdef TRACE_PROPERTIES
    if(sanitizeTarget && originalSanitize && memcmp(sanitizeTarget,savedSanitize,16)) error=ERROR_WRITE_FAULT;
#endif
    CloseHandle(args.collector); r->error=error; InterlockedExchange(&r->status,error?-1:2);
    UnmapViewOfFile(r); InterlockedExchange(&running,0); return error;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void*) {
    if(reason==DLL_PROCESS_ATTACH) { self=instance; DisableThreadLibraryCalls(instance); } return TRUE;
}
