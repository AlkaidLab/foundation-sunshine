#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
#include <mutex>
#include <map>
#include <vector>
#include <cstring>
#include "capture_protocol.h"

using Microsoft::WRL::ComPtr;

// PoC: patch public COM vtable slots in this process only. No audiodg injection,
// global registration, codec bypass, or machine-wide settings are involved.
static TapRing* ring = nullptr;
static HANDLE mapping = nullptr;
static std::recursive_mutex lock;
static std::map<void**, void*> originals;

struct ObjectState {
    uint64_t id = 0, offset = 0;
    void* stream = nullptr;
    uint32_t rate = 0, type = 0;
    float x = 0, y = 0, z = 0, volume = 1;
    BYTE* buffer = nullptr;
    UINT32 bytes = 0;
};
static std::map<void*, ObjectState> objects;
static std::map<void*, uint32_t> rates;
static uint64_t nextId = 0, sequence = 0;

template <class F> static F Original(void* object, size_t slot) {
    auto vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<F>(originals.at(&vtable[slot]));
}

static bool Patch(void* object, size_t slot, void* hook) {
    auto cell = &(*reinterpret_cast<void***>(object))[slot];
    if (originals.count(cell)) return true;
    DWORD old = 0;
    if (!VirtualProtect(cell, sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedIncrement(&ring->patchFailures); return false;
    }
    originals[cell] = *cell;
    InterlockedExchangePointer(cell, hook);
    DWORD ignored = 0;
    VirtualProtect(cell, sizeof(void*), old, &ignored);
    return true;
}

using DeviceActivate = HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using ClientActivate = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioClient*, const PROPVARIANT*, REFIID, void**);
using ObjectActivate = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioObjectRenderStream*, AudioObjectType, ISpatialAudioObject**);
using GetBuffer = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioObject*, BYTE**, UINT32*);
using SetPosition = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioObject*, float, float, float);
using SetVolume = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioObject*, float);
using EndUpdating = HRESULT(STDMETHODCALLTYPE*)(ISpatialAudioObjectRenderStream*);
using ReleaseObject = ULONG(STDMETHODCALLTYPE*)(ISpatialAudioObject*);
using EnumEndpoints = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, DWORD, IMMDeviceCollection**);
using CollectionItem = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceCollection*, UINT, IMMDevice**);
using CreateInstance = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static CreateInstance originalCreate = nullptr;

static ULONG STDMETHODCALLTYPE HookRelease(ISpatialAudioObject* self) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    auto original = Original<ReleaseObject>(self, 2);
    ULONG count = original(self);
    if (!count) objects.erase(self);
    return count;
}

static HRESULT STDMETHODCALLTYPE HookGetBuffer(ISpatialAudioObject* self, BYTE** data, UINT32* bytes) {
    InterlockedIncrement(&ring->bufferCalls);
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<GetBuffer>(self, 3)(self, data, bytes);
    auto it = objects.find(self);
    if (SUCCEEDED(hr) && it != objects.end()) {
        it->second.buffer = *data;
        it->second.bytes = *bytes;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetPosition(ISpatialAudioObject* self, float x, float y, float z) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<SetPosition>(self, 7)(self, x, y, z);
    auto it = objects.find(self);
    if (SUCCEEDED(hr) && it != objects.end()) {
        it->second.x = x; it->second.y = y; it->second.z = z;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetVolume(ISpatialAudioObject* self, float volume) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<SetVolume>(self, 8)(self, volume);
    auto it = objects.find(self);
    if (SUCCEEDED(hr) && it != objects.end()) it->second.volume = volume;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookEnd(ISpatialAudioObjectRenderStream* self) {
    InterlockedIncrement(&ring->endCalls);
    std::lock_guard<std::recursive_mutex> guard(lock);
    // Copy before the real EndUpdating invalidates these buffer pointers.
    for (auto& entry : objects) {
        auto& o = entry.second;
        if (o.stream != self || !o.buffer || !o.bytes) continue;
        LONG w = InterlockedCompareExchange(&ring->write, 0, 0);
        LONG r = InterlockedCompareExchange(&ring->read, 0, 0);
        uint32_t frames = o.bytes / sizeof(float);
        if (w - r >= static_cast<LONG>(kTapSlots) || frames > kTapMaxSamples) {
            InterlockedIncrement(&ring->dropped);
        } else {
            TapBlock& b = ring->blocks[w % kTapSlots];
            b.sequence = sequence++;
            b.objectId = o.id;
            b.frameOffset = o.offset;
            LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
            b.qpc = qpc.QuadPart;
            b.pid = GetCurrentProcessId();
            b.sampleRate = o.rate;
            b.frames = frames;
            b.objectType = o.type;
            b.x = o.x; b.y = o.y; b.z = o.z; b.volume = o.volume;
            memcpy(b.samples, o.buffer, o.bytes);
            InterlockedExchange(&ring->write, w + 1);
        }
        o.offset += frames;
        o.buffer = nullptr;
        o.bytes = 0;
    }
    return Original<EndUpdating>(self, 9)(self);
}

static HRESULT STDMETHODCALLTYPE HookObjectActivate(ISpatialAudioObjectRenderStream* self, AudioObjectType type, ISpatialAudioObject** object) {
    InterlockedIncrement(&ring->objectActivations);
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<ObjectActivate>(self, 10)(self, type, object);
    if (SUCCEEDED(hr) && *object) {
        ObjectState s;
        s.id = ++nextId;
        s.stream = self;
        s.rate = rates[self];
        s.type = static_cast<uint32_t>(type);
        objects[*object] = s;
        Patch(*object, 2, reinterpret_cast<void*>(HookRelease));
        Patch(*object, 3, reinterpret_cast<void*>(HookGetBuffer));
        Patch(*object, 7, reinterpret_cast<void*>(HookSetPosition));
        Patch(*object, 8, reinterpret_cast<void*>(HookSetVolume));
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookClientActivate(ISpatialAudioClient* self, const PROPVARIANT* params, REFIID iid, void** result) {
    InterlockedIncrement(&ring->streamActivations);
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<ClientActivate>(self, 10)(self, params, iid, result);
    if (SUCCEEDED(hr) && iid == __uuidof(ISpatialAudioObjectRenderStream) && params && params->vt == VT_BLOB &&
        params->blob.cbSize >= sizeof(SpatialAudioObjectRenderStreamActivationParams)) {
        auto p = reinterpret_cast<const SpatialAudioObjectRenderStreamActivationParams*>(params->blob.pBlobData);
        if (p->ObjectFormat && p->ObjectFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
            p->ObjectFormat->wBitsPerSample == 32 && p->ObjectFormat->nChannels == 1) {
            rates[*result] = p->ObjectFormat->nSamplesPerSec;
            Patch(*result, 9, reinterpret_cast<void*>(HookEnd));
            Patch(*result, 10, reinterpret_cast<void*>(HookObjectActivate));
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookDeviceActivate(IMMDevice* self, REFIID iid, DWORD cls, PROPVARIANT* params, void** result) {
    InterlockedIncrement(&ring->deviceActivations);
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<DeviceActivate>(self, 3)(self, iid, cls, params, result);
    if (SUCCEEDED(hr) && iid == __uuidof(ISpatialAudioClient)) {
        InterlockedIncrement(&ring->spatialActivations);
        Patch(*result, 10, reinterpret_cast<void*>(HookClientActivate));
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookCollectionItem(IMMDeviceCollection* self, UINT index, IMMDevice** device) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<CollectionItem>(self, 4)(self, index, device);
    if (SUCCEEDED(hr)) Patch(*device, 3, reinterpret_cast<void*>(HookDeviceActivate));
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookEnumEndpoints(IMMDeviceEnumerator* self, EDataFlow flow, DWORD state, IMMDeviceCollection** collection) {
    std::lock_guard<std::recursive_mutex> guard(lock);
    HRESULT hr = Original<EnumEndpoints>(self, 3)(self, flow, state, collection);
    if (SUCCEEDED(hr)) Patch(*collection, 4, reinterpret_cast<void*>(HookCollectionItem));
    return hr;
}

static HRESULT WINAPI HookCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* object) {
    HRESULT hr = originalCreate(clsid, outer, context, iid, object);
    if (SUCCEEDED(hr) && iid == __uuidof(IMMDeviceEnumerator)) {
        std::lock_guard<std::recursive_mutex> guard(lock);
        Patch(*object, 3, reinterpret_cast<void*>(HookEnumEndpoints));
    }
    return hr;
}

static bool HookSourceImport() {
    // Patch only the controlled EXE import. Bind each newly returned COM object
    // rather than assuming dummy instances share an implementation vtable.
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (auto d = imports; d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto values = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(name->Name), "CoCreateInstance") != 0) continue;
            void** cell = reinterpret_cast<void**>(&values[i].u1.Function);
            DWORD protection = 0;
            if (!VirtualProtect(cell, sizeof(void*), PAGE_READWRITE, &protection)) return false;
            originalCreate = reinterpret_cast<CreateInstance>(*cell);
            InterlockedExchangePointer(cell, reinterpret_cast<void*>(HookCreateInstance));
            DWORD ignored = 0; VirtualProtect(cell, sizeof(void*), protection, &ignored);
            return true;
        }
    }
    return false;
}

static DWORD WINAPI Initialize(void*) {
    wchar_t name[128];
    swprintf_s(name, L"Local\\SpatialTapRing_%lu", GetCurrentProcessId());
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 1;
    ring = reinterpret_cast<TapRing*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TapRing)));
    if (!ring || ring->magic != kTapMagic) return 2;
    bool ok = HookSourceImport();
    InterlockedExchange(&ring->ready, ok ? 1 : -1);
    return ok ? 0 : 4;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
