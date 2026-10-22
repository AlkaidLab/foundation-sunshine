// Isolated in-process activation diagnostic. No endpoints, drivers, hooks or registry writes.
#define NOMINMAX
#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;

static void Result(const wchar_t* label, HRESULT hr) {
    wprintf(L"%ls hr=0x%08lX\n", label, static_cast<unsigned long>(hr)); fflush(stdout);
}
static GUID Guid(const wchar_t* text) {
    GUID g{}; if (FAILED(CLSIDFromString(text, &g))) ExitProcess(2); return g;
}
static void Vtable(IUnknown* p, unsigned count) {
    auto** v = *reinterpret_cast<void***>(p);
    for (unsigned i=0; i<count; ++i) {
        HMODULE owner{}; wchar_t path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(v[i]), &owner);
        GetModuleFileNameW(owner, path, MAX_PATH);
        wprintf(L"  vt[%u] %ls +0x%llX\n", i, path,
            reinterpret_cast<ULONG_PTR>(v[i])-reinterpret_cast<ULONG_PTR>(owner));
    }
    fflush(stdout);
}
static void Inspect(IUnknown* p) {
    const wchar_t* ids[] = {
        L"{deff1192-f581-4d77-9c1b-3e596b0ca989}", // IAsarEncoder2, matching audioeng PDB/callsite
        L"{fa1d346f-c678-4005-a719-4ca918cc4c45}", // configuration3 observed in wrapper
        L"{dc57ddb4-e086-49ec-b13d-eccdd512990c}",
        L"{54151d15-066e-441c-81e7-d894d8a0abc7}"
    };
    for (auto text : ids) {
        ComPtr<IUnknown> q;
        HRESULT hr=p->QueryInterface(Guid(text), reinterpret_cast<void**>(q.GetAddressOf()));
        Result(text,hr);
        if(SUCCEEDED(hr)) Vtable(q.Get(), text==ids[0]?8:3);
    }
    ComPtr<IInspectable> inspect;
    HRESULT hr=p->QueryInterface(IID_PPV_ARGS(&inspect)); Result(L"QI IInspectable",hr);
    if(SUCCEEDED(hr)) {
        ULONG count{}; IID* idsOut{};
        hr=inspect->GetIids(&count,&idsOut);Result(L"GetIids",hr);
        if(SUCCEEDED(hr)) {
            for(ULONG i=0;i<count && i<64;++i) {
                wchar_t text[40]{}; StringFromGUID2(idsOut[i],text,40); wprintf(L"  IID %ls\n",text);
            }
            CoTaskMemFree(idsOut);
        }
    }
}
static void Activate(const std::wstring& dir, const wchar_t* dll, const wchar_t* name, const wchar_t* clsid) {
    wprintf(L"\nCOMPONENT %ls %ls\n",dll,name); fflush(stdout);
    const auto path=dir+L"\\"+dll;
    for(DWORD desired : {DWORD(GENERIC_READ), DWORD(GENERIC_READ|GENERIC_EXECUTE)}) {
        HANDLE file=CreateFileW(path.c_str(),desired,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        HRESULT access=file==INVALID_HANDLE_VALUE?HRESULT_FROM_WIN32(GetLastError()):S_OK;
        Result(desired==GENERIC_READ?L"CreateFile READ":L"CreateFile READ_EXECUTE",access);
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    ComPtr<IUnknown> com;
    HRESULT hr=CoCreateInstance(Guid(clsid),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&com));
    Result(L"CoCreateInstance",hr); if(SUCCEEDED(hr)) Inspect(com.Get());
    HSTRING className{}; hr=WindowsCreateString(name,static_cast<UINT32>(wcslen(name)),&className);
    if(FAILED(hr)) {Result(L"WindowsCreateString",hr);return;}
    ComPtr<IActivationFactory> registered;
    hr=RoGetActivationFactory(className,IID_PPV_ARGS(&registered)); Result(L"RoGetActivationFactory",hr);
    // Use the installed DLL in place. Never copy/extract or register vendor components.
    HMODULE mod=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!mod) {Result(L"LoadLibraryExW",HRESULT_FROM_WIN32(GetLastError()));WindowsDeleteString(className);return;}
    auto get=reinterpret_cast<HRESULT(WINAPI*)(HSTRING,IActivationFactory**)>(GetProcAddress(mod,"DllGetActivationFactory"));
    if(!get) {Result(L"GetProcAddress",HRESULT_FROM_WIN32(GetLastError()));WindowsDeleteString(className);return;}
    ComPtr<IActivationFactory> factory;
    hr=get(className,&factory);Result(L"Direct DllGetActivationFactory",hr);
    if(SUCCEEDED(hr)) {
        ComPtr<IInspectable> obj;
        hr=factory->ActivateInstance(&obj);Result(L"ActivateInstance",hr);
        if(SUCCEEDED(hr)) Inspect(obj.Get());
    }
    WindowsDeleteString(className);
    // Keep the module mapped until process exit; no unload races with vendor globals.
}
static void EnumerateMfts() {
    IMFActivate** items{}; UINT32 count{};
    HRESULT hr=MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,MFT_ENUM_FLAG_ALL,nullptr,nullptr,&items,&count);
    Result(L"MFTEnumEx AUDIO_ENCODER ALL",hr);
    if(FAILED(hr)) return;
    wprintf(L"encoder_count=%u\n",count);
    for(UINT32 i=0;i<count;++i) {
        wchar_t* name{}; UINT32 n{}; GUID clsid{};wchar_t id[40]{};
        items[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,&name,&n);
        items[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&clsid);StringFromGUID2(clsid,id,40);
        wprintf(L"MFT %ls %ls\n",name?name:L"<unnamed>",id);
        UINT8* types{};UINT32 bytes{};
        if(SUCCEEDED(items[i]->GetAllocatedBlob(MFT_OUTPUT_TYPES_Attributes,&types,&bytes))) {
            auto* t=reinterpret_cast<MFT_REGISTER_TYPE_INFO*>(types);
            for(UINT32 j=0;j<bytes/sizeof(*t);++j) {
                wchar_t sub[40]{};StringFromGUID2(t[j].guidSubtype,sub,40);wprintf(L"  output=%ls\n",sub);
            }
            CoTaskMemFree(types);
        }
        CoTaskMemFree(name);items[i]->Release();
    }
    CoTaskMemFree(items);fflush(stdout);
    for(auto text : {L"{0000010c-0cea-0010-8000-00aa00389b71}",
                     L"{0000030c-0cea-0010-8000-00aa00389b71}"}) {
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Audio,Guid(text)};
        items=nullptr;count=0;
        hr=MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,MFT_ENUM_FLAG_ALL,nullptr,&output,&items,&count);
        Result(text,hr);wprintf(L"MAT_FILTER %ls count=%u\n",text,count);
        for(UINT32 i=0;i<count;++i) items[i]->Release();
        CoTaskMemFree(items);
    }
    // A supported public encoder is a control, not an Atmos/MAT substitute.
    ComPtr<IMFTransform> ac3;
    hr=CoCreateInstance(Guid(L"{ac3315c9-f481-45d7-826c-0b406c1f64b8}"),nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&ac3));
    Result(L"Control CoCreateInstance AC3 IMFTransform",hr);
    if(SUCCEEDED(hr)) {
        ComPtr<IMFMediaType> type;hr=ac3->GetOutputAvailableType(0,0,&type);
        Result(L"Control AC3 GetOutputAvailableType",hr);
        if(SUCCEEDED(hr)) {
            GUID subtype{};wchar_t id[40]{};UINT32 channels{};
            type->GetGUID(MF_MT_SUBTYPE,&subtype);StringFromGUID2(subtype,id,40);
            type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&channels);
            wprintf(L"Control AC3 subtype=%ls channels=%u\n",id,channels);
        }
    }
    fflush(stdout);
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc!=2) {fwprintf(stderr,L"Usage: direct-encoder-probe INSTALLED_DOLBY_DLL_DIRECTORY\n");return 2;}
    Result(L"RoInitialize",RoInitialize(RO_INIT_MULTITHREADED));
    HMODULE control=LoadLibraryExW(L"propsys.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    Result(L"Control LoadLibraryExW propsys.dll",control?S_OK:HRESULT_FROM_WIN32(GetLastError()));
    if(control) FreeLibrary(control);
    Result(L"MFStartup",MFStartup(MF_VERSION,MFSTARTUP_NOSOCKET));
    EnumerateMfts();
    Activate(argv[1],L"DolbyMatEnc.dll",L"Dolby.DolbyMatEncoder",L"{aaca92be-029d-4006-ada0-725b506cf9b2}");
    Activate(argv[1],L"DolbyMatEngine.dll",L"Dolby.DolbyMATEngine",L"{5b3c2c7f-5957-4892-bcc5-022dee554ae1}");
    wprintf(L"DONE activation_only=1 encoded_bytes=0 endpoint_calls=0\n");fflush(stdout);
    MFShutdown();RoUninitialize();return 0;
}
