// Version-locked, worker-thread-only native encoder experiment. Never touches a render endpoint.
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
#include "mat_encode_protocol.h"
using Microsoft::WRL::ComPtr;
static HMODULE self;
static volatile LONG running;
static GUID Guid(const wchar_t* value){GUID g{};CLSIDFromString(value,&g);return g;}
static void Event(MatEncodeState* s,const char* label,HRESULT hr,uint32_t index=0){
    LONG n=s->eventCount;if(n>=64)return;
    strcpy_s(s->events[n].label,label);s->events[n].hr=hr;s->events[n].caseIndex=index;
    InterlockedExchange(&s->eventCount,n+1);
}
static bool Stop(MatEncodeState* s,HANDLE collector,ULONGLONG began){
    return s->stop || GetTickCount64()-began>10000 || WaitForSingleObject(collector,0)!=WAIT_TIMEOUT ||
        static_cast<LONG64>(GetTickCount64())-InterlockedCompareExchange64(&s->heartbeat,0,0)>5000;
}
static bool InputsValid(MatEncodeState* s){
    for(auto& c:s->cases){
        if(c.blocks!=kEncodeBlocks)return false;
        for(uint32_t b=0;b<c.blocks;++b){
            auto& block=c.input[b];
            if(!block.objects[0].id || !block.objects[1].id || block.objects[0].id==block.objects[1].id)return false;
            for(unsigned o=0;o<2;++o){
                auto& object=block.objects[o];
                if(object.id!=c.input[0].objects[o].id)return false;
                for(float p:object.xyz)if(!std::isfinite(p) || fabsf(p)>10000)return false;
                for(float p:object.pcm)if(!std::isfinite(p) || fabsf(p)>1)return false;
            }
        }
    }
    return true;
}
// Signatures/slots checked against this audioeng PDB and the installed Dolby binary.
using MakeMapperFactory=HRESULT(__fastcall*)(IUnknown**);
using CreateMapper=HRESULT(__fastcall*)(IUnknown*,REFGUID,IUnknown**);
using InitializeEncoder=HRESULT(__fastcall*)(IUnknown*,UINT32,UINT32,UINT32,UINT32,ULONG,IUnknown*,REFGUID,IUnknown*);
using SubmitObject=HRESULT(__fastcall*)(IUnknown*,const void*,const void*,const float*,UINT32,float);
using ProcessEncoder=HRESULT(__fastcall*)(IUnknown*,UINT32,BYTE*,UINT32*);
static HRESULT NewMapper(BYTE* audio,IUnknown** output){
    const BYTE expected[]={0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56};
    if(memcmp(audio+0xdf298,expected,sizeof(expected)))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    ComPtr<IUnknown> factory;
    HRESULT hr=reinterpret_cast<MakeMapperFactory>(audio+0xdf298)(&factory);
    if(FAILED(hr))return hr;
    auto** table=*reinterpret_cast<void***>(factory.Get());
    if(table[3]!=audio+0xe00e0)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    const GUID format=Guid(L"{a289735d-fa3e-4e35-9d7d-b6f896acb2e7}");
    return reinterpret_cast<CreateMapper>(table[3])(factory.Get(),format,output);
}
static bool EncoderTable(IUnknown* encoder,HMODULE module){
    const uintptr_t rvas[]={0x7780,0x7540,0x77e0,0x41a0,0x5ee0,0x6270,0x5760,0x4e00};
    auto** table=*reinterpret_cast<void***>(encoder);
    for(unsigned i=0;i<8;++i)if(table[i]!=reinterpret_cast<BYTE*>(module)+rvas[i])return false;
    return true;
}
struct ObjectProperties {
    uint32_t id,offset,type,flags;float x,y,z,volume;BYTE extra[0xb4-32];
};
static_assert(sizeof(ObjectProperties)==0xb4);
static bool EncodeCase(MatEncodeState* s,uint32_t index,IActivationFactory* factory,HMODULE module,BYTE* audio,HANDLE collector,ULONGLONG began){
    auto& c=s->cases[index];
    struct Timer {MatEncodeCase& c;LARGE_INTEGER start{};Timer(MatEncodeCase& value):c(value){QueryPerformanceCounter(&start);}~Timer(){LARGE_INTEGER finish{};QueryPerformanceCounter(&finish);c.elapsedTicks=finish.QuadPart-start.QuadPart;}} timer(c);
    ComPtr<IInspectable> object;ComPtr<IUnknown> encoder,mapper;
    c.createHr=factory->ActivateInstance(&object);Event(s,"Activate fresh encoder",c.createHr,index);
    if(FAILED(c.createHr))return false;
    c.createHr=object->QueryInterface(Guid(L"{deff1192-f581-4d77-9c1b-3e596b0ca989}"),reinterpret_cast<void**>(encoder.GetAddressOf()));
    Event(s,"QI IAsarEncoder2",c.createHr,index);
    if(FAILED(c.createHr))return false;
    if(!EncoderTable(encoder.Get(),module)){c.createHr=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);Event(s,"Encoder ABI guard",c.createHr,index);return false;}
    c.mapperHr=NewMapper(audio,&mapper);Event(s,"Create native Windows MAT position mapper",c.mapperHr,index);
    if(FAILED(c.mapperHr) || Stop(s,collector,began))return false;
    auto** table=*reinterpret_cast<void***>(encoder.Get());
    c.initializationAttempted=1;
    // 480 frames/object, 48kHz, 7.1.4 static mask, 8-channel carrier. Session GUID/context default/null.
    c.initializeHr=reinterpret_cast<InitializeEncoder>(table[3])(encoder.Get(),480,48000,0x1ffe,8,0,mapper.Get(),GUID_NULL,nullptr);
    Event(s,"Initialize native encoder",c.initializeHr,index);
    if(FAILED(c.initializeHr))return false; // Never retry with a forged context or patched check.
    c.initialized=1;
    auto submit=reinterpret_cast<SubmitObject>(table[4]);auto process=reinterpret_cast<ProcessEncoder>(table[7]);
    BYTE streamInfo[24]{};float silence[kEncodeFrames]{};
    // Create the same objects during silent preroll. Actual source PCM starts at frame zero,
    // with no re-create and no source truncation. Retain all carrier output for A/B analysis.
    const uint32_t sourceEnd=kEncodePrerollBlocks+c.blocks;
    for(uint32_t b=0;b<sourceEnd+kEncodeTailBlocks;++b){
        if(Stop(s,collector,began)){c.processHr=HRESULT_FROM_WIN32(ERROR_CANCELLED);Event(s,"Stopped between calls",c.processHr,index);return false;}
        const bool preroll=b<kEncodePrerollBlocks;
        const bool source=b>=kEncodePrerollBlocks && b<sourceEnd;
        if(b<=sourceEnd){
            for(unsigned o=0;o<2;++o){
                const uint32_t sourceBlock=preroll?0:(source?b-kEncodePrerollBlocks:c.blocks-1);
                auto& input=c.input[sourceBlock].objects[o];
                ObjectProperties properties{};properties.id=input.id;properties.type=1;
                properties.flags=(b==sourceEnd?8u:((b==0?1u:0u)|2u|4u));
                properties.x=input.xyz[0];properties.y=input.xyz[1];properties.z=input.xyz[2];properties.volume=1;
                c.submitHr=submit(encoder.Get(),streamInfo,&properties,source?input.pcm:silence,kEncodeFrames,1.f);
                if(FAILED(c.submitHr)){Event(s,"Object submit failed",c.submitHr,index);return false;}
                ++c.submissions;
            }
        }
        if(c.records>=kEncodeRecords || kEncodeBytes-c.bytes<kEncodeRequestBytes){c.processHr=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);Event(s,"Own output capacity guard reached",c.processHr,index);return false;}
        UINT32 produced{};BYTE* output=c.output+c.bytes;
        c.processHr=process(encoder.Get(),kEncodeRequestBytes,output,&produced);
        if(FAILED(c.processHr) || produced>kEncodeRequestBytes){Event(s,"Process failed or invalid length",c.processHr,index);return false;}
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        c.outputRecords[c.records]={0x42595445,produced,c.records,now.QuadPart,reinterpret_cast<uint64_t>(encoder.Get()),480,kEncodeRequestBytes,produced,GetCurrentThreadId(),c.processHr,0,0};
        ++c.records;c.bytes+=produced;
        if(source)++c.processedBlocks;else if(preroll)++c.processedPrerollBlocks;else ++c.processedTailBlocks;
    }
    Event(s,"Case processing complete; offline validation required",S_OK,index);
    return true;
}
extern "C" __declspec(dllexport) DWORD WINAPI StartMatEncodeProbe(void* data){
    auto args=*static_cast<MatEncodeArgs*>(data);
    auto* s=static_cast<MatEncodeState*>(MapViewOfFile(args.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(MatEncodeState)));CloseHandle(args.mapping);
    if(!s){CloseHandle(args.collector);return ERROR_INVALID_HANDLE;}
    DWORD error=0;if(InterlockedCompareExchange(&running,1,0))error=ERROR_BUSY;bool owns=!error;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);auto* name=wcsrchr(path,L'\\');name=name?name+1:path;
    auto* audio=reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    if(!error && (_wcsicmp(name,L"audiodg.exe") || !audio || s->magic!=kEncodeMagic || s->version!=kEncodeVersion))error=ERROR_BAD_ENVIRONMENT;
    if(!error){
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(audio+reinterpret_cast<IMAGE_DOS_HEADER*>(audio)->e_lfanew);
        if(nt->FileHeader.TimeDateStamp!=0x0627570d || nt->OptionalHeader.SizeOfImage!=0x1ef000)error=ERROR_REVISION_MISMATCH;
        if(!wmemchr(s->vendorDirectory,L'\0',MAX_PATH) || !InputsValid(s))error=ERROR_INVALID_DATA;
    }
    s->hostPid=GetCurrentProcessId();
    if(!error){
        HMODULE pin{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pin);
        s->guardPassed=1;InterlockedExchange(&s->status,1);
        HRESULT init=RoInitialize(RO_INIT_MULTITHREADED);Event(s,"RoInitialize worker",init);
        if(SUCCEEDED(init)){
            wchar_t dll[MAX_PATH]{};swprintf_s(dll,L"%ls\\DolbyMatEnc.dll",s->vendorDirectory);
            HMODULE module=LoadLibraryExW(dll,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            HRESULT hr=module?S_OK:HRESULT_FROM_WIN32(GetLastError());Event(s,"Load installed encoder",hr);
            if(module){
                auto get=reinterpret_cast<HRESULT(WINAPI*)(HSTRING,IActivationFactory**)>(GetProcAddress(module,"DllGetActivationFactory"));
                HSTRING runtime{};ComPtr<IActivationFactory> factory;
                hr=WindowsCreateString(L"Dolby.DolbyMatEncoder",21,&runtime);
                if(SUCCEEDED(hr) && get)hr=get(runtime,&factory);else if(!get)hr=E_NOINTERFACE;
                if(runtime)WindowsDeleteString(runtime);Event(s,"Get encoder factory",hr);
                if(SUCCEEDED(hr)){
                    auto began=GetTickCount64();
                    for(uint32_t i=0;i<kEncodeCases;++i)if(!EncodeCase(s,i,factory.Get(),module,audio,args.collector,began))break;
                }
                factory.Reset(); // Loaded module refs intentionally retained until audiodg exits.
            }
            RoUninitialize();
        }
        Event(s,"Probe diagnostic finished; no endpoint operations",S_OK);
    }
    s->error=error;InterlockedExchange(&s->status,error?-1:2);
    UnmapViewOfFile(s);CloseHandle(args.collector);if(owns)InterlockedExchange(&running,0);return error;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){
    if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;
}
