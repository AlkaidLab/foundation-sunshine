// Bounded, paced private-ABI experiment on an OWN worker. No endpoint/hook/audio-thread waits.
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cmath>
#include "mat_replay_v4_input.h"
using Microsoft::WRL::ComPtr;
static HMODULE self;
static volatile LONG running;
static GUID Guid(const wchar_t* value){GUID g{};CLSIDFromString(value,&g);return g;}
static void Event(ReplayState* s,const char* label,HRESULT hr,uint32_t index=0){
    LONG n=s->eventCount;if(n>=64)return;
    strcpy_s(s->events[n].label,label);s->events[n].hr=hr;s->events[n].caseIndex=index;
    InterlockedExchange(&s->eventCount,n+1);
}
static bool Stop(ReplayState* s,HANDLE collector,ULONGLONG began){
    return s->stop || GetTickCount64()-began>10000 || WaitForSingleObject(collector,0)!=WAIT_TIMEOUT ||
        static_cast<LONG64>(GetTickCount64())-InterlockedCompareExchange64(&s->heartbeat,0,0)>5000;
}
static bool InputsValid(ReplayState* s){return ReplayInputsValid(s);}
// Same guarded ABI already exercised by v3; no reverse-engineered checks are patched out.
using MakeMapperFactory=HRESULT(__fastcall*)(IUnknown**);
using CreateMapper=HRESULT(__fastcall*)(IUnknown*,REFGUID,IUnknown**);
using InitializeEncoder=HRESULT(__fastcall*)(IUnknown*,UINT32,UINT32,UINT32,UINT32,ULONG,IUnknown*,REFGUID,IUnknown*);
using SubmitObject=HRESULT(__fastcall*)(IUnknown*,const void*,const void*,const float*,UINT32,float);
using ProcessEncoder=HRESULT(__fastcall*)(IUnknown*,UINT32,BYTE*,UINT32*);
static HRESULT NewMapper(BYTE* audio,IUnknown** output){
    const BYTE expected[]={0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56};
    if(memcmp(audio+0xdf298,expected,sizeof(expected)))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    ComPtr<IUnknown> factory;HRESULT hr=reinterpret_cast<MakeMapperFactory>(audio+0xdf298)(&factory);
    if(FAILED(hr))return hr;
    auto** table=*reinterpret_cast<void***>(factory.Get());
    if(table[3]!=audio+0xe00e0)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    return reinterpret_cast<CreateMapper>(table[3])(factory.Get(),Guid(L"{a289735d-fa3e-4e35-9d7d-b6f896acb2e7}"),output);
}
static bool EncoderTable(IUnknown* encoder,HMODULE module){
    const uintptr_t rvas[]={0x7780,0x7540,0x77e0,0x41a0,0x5ee0,0x6270,0x5760,0x4e00};
    auto** table=*reinterpret_cast<void***>(encoder);
    for(unsigned i=0;i<8;++i)if(table[i]!=reinterpret_cast<BYTE*>(module)+rvas[i])return false;
    return true;
}
struct ObjectProperties {uint32_t id,offset,type,flags;float x,y,z,volume;BYTE extra[0xb4-32];};
static_assert(sizeof(ObjectProperties)==0xb4);
struct TimerHandle {HANDLE value{};~TimerHandle(){if(value)CloseHandle(value);}};
static bool WaitUntil(HANDLE timer,int64_t target,int64_t hz,ReplayState* s,HANDLE collector,ULONGLONG began){
    for(;;){
        if(Stop(s,collector,began))return false;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);if(now.QuadPart>=target)return true;
        LARGE_INTEGER due{};due.QuadPart=-((target-now.QuadPart)*10000000+hz-1)/hz;
        if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))return false;
        // No spinning, no global timer resolution/priority changes. Only our worker sleeps.
        if(WaitForSingleObject(timer,20)==WAIT_FAILED)return false;
    }
}
static bool EncodeCase(ReplayState* s,uint32_t index,IActivationFactory* factory,HMODULE module,BYTE* audio,HANDLE collector,ULONGLONG began){
    auto& c=s->cases[index];
    struct Timer {ReplayCase& c;LARGE_INTEGER start{};Timer(ReplayCase& v):c(v){QueryPerformanceCounter(&start);}~Timer(){LARGE_INTEGER end{};QueryPerformanceCounter(&end);c.elapsedTicks=end.QuadPart-start.QuadPart;}} elapsed(c);
    ComPtr<IInspectable> object;ComPtr<IUnknown> encoder,mapper;
    c.createHr=factory->ActivateInstance(&object);Event(s,"Activate fresh encoder",c.createHr,index);
    if(FAILED(c.createHr))return false;
    c.createHr=object->QueryInterface(Guid(L"{deff1192-f581-4d77-9c1b-3e596b0ca989}"),reinterpret_cast<void**>(encoder.GetAddressOf()));
    if(FAILED(c.createHr) || !EncoderTable(encoder.Get(),module)){c.createHr=E_NOINTERFACE;Event(s,"Encoder ABI/QI guard failed",c.createHr,index);return false;}
    c.mapperHr=NewMapper(audio,&mapper);Event(s,"Create native Windows MAT mapper",c.mapperHr,index);
    if(FAILED(c.mapperHr) || Stop(s,collector,began))return false;
    auto** table=*reinterpret_cast<void***>(encoder.Get());c.initializationAttempted=1;
    c.initializeHr=reinterpret_cast<InitializeEncoder>(table[3])(encoder.Get(),480,48000,0x1ffe,8,0,mapper.Get(),GUID_NULL,nullptr);
    Event(s,"Initialize native encoder",c.initializeHr,index);if(c.initializeHr!=S_OK)return false;c.initialized=1;
    TimerHandle timer;timer.value=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    if(!timer.value){c.processHr=HRESULT_FROM_WIN32(GetLastError());Event(s,"High resolution timer creation failed; no fallback",c.processHr,index);return false;}
    auto submit=reinterpret_cast<SubmitObject>(table[4]);auto process=reinterpret_cast<ProcessEncoder>(table[7]);
    BYTE streamInfo[24]{};LARGE_INTEGER hz{},origin{};QueryPerformanceFrequency(&hz);QueryPerformanceCounter(&origin);
    uint32_t active=0;
    for(uint32_t b=0;b<c.blocks;++b){
        auto& timing=c.timing[b];timing.target=origin.QuadPart+static_cast<int64_t>(b)*hz.QuadPart/100;
        if(!WaitUntil(timer.value,timing.target,hz.QuadPart,s,collector,began)){c.processHr=HRESULT_FROM_WIN32(ERROR_CANCELLED);Event(s,"Wait/watchdog stopped",c.processHr,index);return false;}
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);timing.start=now.QuadPart;
        auto& block=c.input[b];timing.submissions=block.count;
        c.submitHr=SubmitReplayBlock(block,encoder.Get(),streamInfo,reinterpret_cast<ReplaySubmit>(submit));
        if(c.submitHr!=S_OK){Event(s,"Object submit failed or non-S_OK",c.submitHr,index);return false;}
        for(unsigned o=0;o<block.count;++o){++c.submissions;auto flags=block.objects[o].flags;if(flags&1)++active;if(flags&8)--active;}
        timing.active=active;
        if(c.records>=kReplayRecords || kReplayBytes-c.bytes<kReplayRequest){c.processHr=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);Event(s,"Own output guard",c.processHr,index);return false;}
        QueryPerformanceCounter(&now);timing.processStart=now.QuadPart;UINT32 produced{};
        c.processHr=process(encoder.Get(),kReplayRequest,c.output+c.bytes,&produced);
        QueryPerformanceCounter(&now);timing.finish=now.QuadPart;
        if(c.processHr!=S_OK || produced>kReplayRequest){Event(s,"Process failed or invalid length",c.processHr,index);return false;}
        c.outputRecords[c.records]={0x42595445,produced,c.records,now.QuadPart,reinterpret_cast<uint64_t>(encoder.Get()),480,kReplayRequest,produced,GetCurrentThreadId(),c.processHr,0,0};
        ++c.records;c.bytes+=produced;++c.processedBlocks;
    }
    Event(s,"Paced case complete; offline verification required",S_OK,index);return true;
}
// Pure fixture validator for ordinary-process tests; it cannot activate any native codec.
extern "C" __declspec(dllexport) BOOL WINAPI ValidateMatReplayV4Input(ReplayState* s){return InputsValid(s);}
extern "C" __declspec(dllexport) DWORD WINAPI StartMatReplayV4Probe(void* data){
    auto args=*static_cast<ReplayArgs*>(data);
    auto* s=static_cast<ReplayState*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(ReplayState)));CloseHandle(args.mapping);
    if(!s){CloseHandle(args.collector);return ERROR_INVALID_HANDLE;}
    DWORD error=0;if(InterlockedCompareExchange(&running,1,0))error=ERROR_BUSY;bool owns=!error;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto* name=wcsrchr(path,L'\\');name=name?name+1:path;
    auto* audio=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    if(!error && (_wcsicmp(name,L"audiodg.exe") || !audio || s->magic!=kReplayMagic || s->version!=kReplayVersion))error=ERROR_BAD_ENVIRONMENT;
    if(!error){
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(audio+reinterpret_cast<IMAGE_DOS_HEADER*>(audio)->e_lfanew);
        if(nt->FileHeader.TimeDateStamp!=0x0627570d || nt->OptionalHeader.SizeOfImage!=0x1ef000)error=ERROR_REVISION_MISMATCH;
        if(!wmemchr(s->vendorDirectory,L'\0',MAX_PATH) || !InputsValid(s))error=ERROR_INVALID_DATA;
    }
    s->hostPid=GetCurrentProcessId();
    if(!error){
        HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pin))error=GetLastError();
        s->guardPassed=error?0:1;InterlockedExchange(&s->status,error?-1:1);
        HRESULT init=error?HRESULT_FROM_WIN32(error):RoInitialize(RO_INIT_MULTITHREADED);Event(s,"RoInitialize worker",init);
        if(SUCCEEDED(init)){
            wchar_t dll[MAX_PATH]{};swprintf_s(dll,L"%ls\\DolbyMatEnc.dll",s->vendorDirectory);
            HMODULE module=LoadLibraryExW(dll,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            HRESULT hr=module?S_OK:HRESULT_FROM_WIN32(GetLastError());Event(s,"Load installed encoder",hr);
            if(module){
                auto get=reinterpret_cast<HRESULT(WINAPI*)(HSTRING,IActivationFactory**)>(GetProcAddress(module,"DllGetActivationFactory"));
                HSTRING runtime{};ComPtr<IActivationFactory> factory;hr=WindowsCreateString(L"Dolby.DolbyMatEncoder",21,&runtime);
                if(SUCCEEDED(hr) && get)hr=get(runtime,&factory);else if(!get)hr=E_NOINTERFACE;
                if(runtime)WindowsDeleteString(runtime);Event(s,"Get encoder factory",hr);
                if(SUCCEEDED(hr)){auto began=GetTickCount64();for(uint32_t i=0;i<kReplayCases;++i)if(!EncodeCase(s,i,factory.Get(),module,audio,args.collector,began)){error=ERROR_FUNCTION_FAILED;break;}}else error=ERROR_FUNCTION_FAILED;
                factory.Reset(); // Installed module refs and pinned own helper retained until audiodg exits.
            }else error=ERROR_MOD_NOT_FOUND;
            RoUninitialize();
        }else if(!error)error=ERROR_FUNCTION_FAILED;
        Event(s,"Replay diagnostic finished; no endpoint calls",error?E_FAIL:S_OK);
    }
    s->error=error;InterlockedExchange(&s->status,error?-1:2);
    UnmapViewOfFile(s);CloseHandle(args.collector);if(owns)InterlockedExchange(&running,0);return error;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;}
