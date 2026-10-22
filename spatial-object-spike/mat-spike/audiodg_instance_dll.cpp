// Stage A ONLY: create independent instances, inspect interfaces, release them.
// No encoder Initialize/SetAudioObject/Process calls, no hooks or audio endpoint APIs.
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "audiodg_instance_protocol.h"
using Microsoft::WRL::ComPtr;
static HMODULE self;
static volatile LONG running;
static void Event(InstanceState* s,const char* text,HRESULT hr,uint32_t component=0,uint64_t a=0,uint64_t b=0) {
    LONG index=s->eventCount;
    if(index>=static_cast<LONG>(kInstanceEvents)){++s->eventsDropped;return;}
    auto& e=s->events[index];strcpy_s(e.label,text);e.hr=hr;e.component=component;e.a=a;e.b=b;
    InterlockedExchange(&s->eventCount,index+1);
}
static bool Stop(InstanceState* s,HANDLE collector,ULONGLONG started) {
    return s->stop || WaitForSingleObject(collector,0)!=WAIT_TIMEOUT || GetTickCount64()-started>10000 ||
        static_cast<LONG64>(GetTickCount64())-InterlockedCompareExchange64(&s->heartbeat,0,0)>5000;
}
static GUID EncoderIID() {GUID g{};CLSIDFromString(L"{deff1192-f581-4d77-9c1b-3e596b0ca989}",&g);return g;}
static void Component(InstanceState* s,uint32_t index,HANDLE collector,ULONGLONG started) {
    auto& c=s->components[index];
    const wchar_t* dll=index?L"DolbyMatEngine.dll":L"DolbyMatEnc.dll";
    const wchar_t* runtime=index?L"Dolby.DolbyMATEngine":L"Dolby.DolbyMatEncoder";
    c.preloaded=GetModuleHandleW(dll)!=nullptr;
    wchar_t path[MAX_PATH]{};
    if(swprintf_s(path,L"%ls\\%ls",s->vendorDirectory,dll)<0){c.loadHr=E_INVALIDARG;return;}
    HMODULE module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    c.loadHr=module?S_OK:HRESULT_FROM_WIN32(GetLastError());Event(s,"LoadLibrary installed component",c.loadHr,index,c.preloaded);
    if(!module || Stop(s,collector,started))return;
    // Keep this reference until audiodg exits; unloading vendor global worker state is not assumed safe.
    auto get=reinterpret_cast<HRESULT(WINAPI*)(HSTRING,IActivationFactory**)>(GetProcAddress(module,"DllGetActivationFactory"));
    if(!get){c.factoryHr=HRESULT_FROM_WIN32(GetLastError());Event(s,"GetProcAddress factory",c.factoryHr,index);return;}
    HSTRING className{};HRESULT hr=WindowsCreateString(runtime,static_cast<UINT32>(wcslen(runtime)),&className);
    if(FAILED(hr)){c.factoryHr=hr;Event(s,"WindowsCreateString",hr,index);return;}
    ComPtr<IActivationFactory> factory;
    c.factoryHr=get(className,&factory);WindowsDeleteString(className);
    Event(s,"DllGetActivationFactory",c.factoryHr,index);
    if(FAILED(c.factoryHr))return;
    ComPtr<IInspectable> objects[2];ComPtr<IUnknown> identities[2],encoders[2];
    for(uint32_t i=0;i<2;++i) {
        if(Stop(s,collector,started)){Event(s,"Stop before activation",HRESULT_FROM_WIN32(ERROR_CANCELLED),index,i);break;}
        c.activateHr[i]=factory->ActivateInstance(&objects[i]);Event(s,"ActivateInstance new object",c.activateHr[i],index,i);
        if(FAILED(c.activateHr[i]))continue;
        hr=objects[i]->QueryInterface(IID_PPV_ARGS(&identities[i]));Event(s,"Canonical IUnknown",hr,index,i);
        c.encoderQiHr[i]=objects[i]->QueryInterface(EncoderIID(),reinterpret_cast<void**>(encoders[i].GetAddressOf()));
        Event(s,"QI IAsarEncoder2",c.encoderQiHr[i],index,i);
    }
    c.distinctCanonicalIdentities=identities[0] && identities[1] && identities[0].Get()!=identities[1].Get();
    Event(s,"Distinct canonical identities",S_OK,index,c.distinctCanonicalIdentities);
    if(encoders[0]) {
        auto** table=*reinterpret_cast<void***>(encoders[0].Get());
        for(unsigned i=0;i<8;++i) {
            HMODULE owner{};DWORD ownerTag=0;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(table[i]),&owner)) {
                if(owner==GetModuleHandleW(L"DolbyMatEnc.dll"))ownerTag=1;
                else if(owner==GetModuleHandleW(L"DolbyMatEngine.dll"))ownerTag=2;
                else if(owner==GetModuleHandleW(L"audioeng.dll"))ownerTag=3;
                else ownerTag=4;
                c.encoderVtableRvas[i]=reinterpret_cast<uintptr_t>(table[i])-reinterpret_cast<uintptr_t>(owner);
            }
            c.encoderVtableOwners[i]=ownerTag;Event(s,"IAsarEncoder2 vtable RVA",S_OK,index,i,c.encoderVtableRvas[i]);
        }
    }
    encoders[0].Reset();encoders[1].Reset();identities[0].Reset();identities[1].Reset();objects[0].Reset();objects[1].Reset();factory.Reset();
    Event(s,"Owned COM references released",S_OK,index);
}
extern "C" __declspec(dllexport) DWORD WINAPI StartMatInstanceProbe(void* data) {
    const auto args=*static_cast<InstanceArgs*>(data);
    auto* s=static_cast<InstanceState*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(InstanceState)));
    CloseHandle(args.mapping);
    if(!s){CloseHandle(args.collector);return ERROR_INVALID_HANDLE;}
    DWORD error=0;
    if(InterlockedCompareExchange(&running,1,0))error=ERROR_BUSY;
    bool owns=!error;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
    auto* audio=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    if(!error && (s->magic!=kInstanceMagic || s->version!=kInstanceVersion || _wcsicmp(name,L"audiodg.exe") || !audio))error=ERROR_BAD_ENVIRONMENT;
    if(!error) {
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(audio+reinterpret_cast<IMAGE_DOS_HEADER*>(audio)->e_lfanew);
        if(nt->FileHeader.TimeDateStamp!=0x0627570d || nt->OptionalHeader.SizeOfImage!=0x1ef000)error=ERROR_REVISION_MISMATCH;
        if(!wmemchr(s->vendorDirectory,L'\0',MAX_PATH) || !s->vendorDirectory[0])error=ERROR_INVALID_PARAMETER;
    }
    s->hostPid=GetCurrentProcessId();s->hostTid=GetCurrentThreadId();
    if(!error) {
        HMODULE pin{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pin);
        s->guardPassed=1;InterlockedExchange(&s->status,1);
        auto start=GetTickCount64();HRESULT init=RoInitialize(RO_INIT_MULTITHREADED);Event(s,"RoInitialize worker thread",init);
        if(SUCCEEDED(init)) {
            Component(s,0,args.collector,start);
            if(!Stop(s,args.collector,start))Component(s,1,args.collector,start);
            RoUninitialize();
        }
        Event(s,"Stage A complete; no encoder initialization",S_OK);
    }
    s->error=error;InterlockedExchange(&s->status,error?-1:2);
    UnmapViewOfFile(s);CloseHandle(args.collector);if(owns)InterlockedExchange(&running,0);return error;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*) {
    if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;
}
