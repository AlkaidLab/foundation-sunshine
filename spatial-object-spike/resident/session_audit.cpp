#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;
int wmain() {
    SetConsoleOutputCP(CP_UTF8); CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return 1;
    ComPtr<IMMDevice> def; LPWSTR defaultId{};
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&def))) def->GetId(&defaultId);
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&devices))) return 1;
    UINT count{}; devices->GetCount(&count);
    for (UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> d; devices->Item(i,&d); LPWSTR id{}; d->GetId(&id);
        ComPtr<IPropertyStore> store; d->OpenPropertyStore(STGM_READ,&store);
        PROPVARIANT name{}; store->GetValue(PKEY_Device_FriendlyName,&name);
        printf("ENDPOINT index=%u default=%d name=%ls id=%ls\n",i,defaultId&&id&&!wcscmp(defaultId,id),name.pwszVal,id);
        PropVariantClear(&name); CoTaskMemFree(id);
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(d->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,&manager))) continue;
        ComPtr<IAudioSessionEnumerator> sessions; manager->GetSessionEnumerator(&sessions);
        int n{}; sessions->GetCount(&n);
        for(int j=0;j<n;++j) {
            ComPtr<IAudioSessionControl> c; sessions->GetSession(j,&c);
            ComPtr<IAudioSessionControl2> c2; c.As(&c2);
            DWORD pid{}; AudioSessionState state{}; c2->GetProcessId(&pid); c->GetState(&state);
            ComPtr<IAudioMeterInformation> meter; float peak=-1;
            if(SUCCEEDED(c.As(&meter))) meter->GetPeakValue(&peak);
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
            wchar_t path[1024]{}; DWORD length=1024;
            if(process) { QueryFullProcessImageNameW(process,0,path,&length); CloseHandle(process); }
            printf("  SESSION pid=%lu state=%d peak=%.8g path=%ls\n",pid,int(state),peak,path);
        }
    }
    CoTaskMemFree(defaultId); return 0;
}
