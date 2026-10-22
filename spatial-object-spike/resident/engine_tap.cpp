#define NOMINMAX
#include <windows.h>
#include <cstring>
#include "protocol.h"
#include "vendor/minhook-1.3.4/include/MinHook.h"

// No debugger, waits, heap allocation, logging or IPC system calls in the hook.
// A single CAS attempt drops on concurrent producers instead of blocking them.
using Mono = HRESULT (__fastcall*)(void*, const float*, uint32_t, float);
static Mono original;
static void* volatile currentRing;
static volatile LONG active, running;
static bool created;
static void* target;
static HMODULE self;

static LONG64 Read64(volatile LONG64* p) { return InterlockedCompareExchange64(p, 0, 0); }
static void Maximum(volatile LONG64* p, LONG64 value) {
    LONG64 old = Read64(p);
    // Approximate maximum is telemetry only; never spin on the audio thread.
    if (value > old) InterlockedCompareExchange64(p, value, old);
}
static void Copy(Ring* r, void* host, const float* samples, uint32_t frames, float gain) {
    LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
    InterlockedIncrement64(&r->calls);
    if (!frames || frames > kSamples) { InterlockedIncrement64(&r->invalid); return; }
    if (InterlockedCompareExchange(&r->writer, 1, 0)) { InterlockedIncrement64(&r->droppedBusy); return; }
    LONG64 write = Read64(&r->write), read = Read64(&r->read);
    if (write - read >= kSlots) {
        InterlockedIncrement64(&r->droppedFull); InterlockedExchange(&r->writer, 0); return;
    }
    Slot& s = r->slots[write % kSlots];
    bool copied = false;
    __try {
        const BYTE* h = static_cast<const BYTE*>(host);
        s.record.magic = 0x474f424a; s.record.frames = frames;
        s.record.host = reinterpret_cast<uint64_t>(host); s.record.qpc = begin.QuadPart;
        memcpy(&s.record.volume, h + 0x64, 4);
        memcpy(&s.record.objectId, h + 0x68, 4);
        memcpy(&s.record.type, h + 0x6c, 4);
        memcpy(&s.record.x, h + 0x98, 12);
        s.record.callGain = gain; s.record.tid = GetCurrentThreadId();
        s.record.sequence = write;
        if (samples) memcpy(s.samples, samples, frames * sizeof(float));
        else { memset(s.samples, 0, frames * sizeof(float)); InterlockedIncrement64(&r->silent); }
        copied = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement64(&r->invalid); }
    if (copied) {
        InterlockedAdd64(&r->frames, frames);
        Maximum(&r->highWater, write - read + 1);
        InterlockedExchange64(&r->write, write + 1);
    }
    InterlockedExchange(&r->writer, 0);
    QueryPerformanceCounter(&end);
    LONG64 ticks = end.QuadPart - begin.QuadPart;
    InterlockedAdd64(&r->copyTicks, ticks); Maximum(&r->maxCopyTicks, ticks);
}
static HRESULT __fastcall Hook(void* host, const float* samples, uint32_t frames, float gain) {
    InterlockedIncrement(&active);
    auto* r = static_cast<Ring*>(InterlockedCompareExchangePointer(&currentRing, nullptr, nullptr));
    if (r && !r->stop) Copy(r, host, samples, frames, gain);
    HRESULT hr = original(host, samples, frames, gain);
    InterlockedDecrement(&active);
    return hr;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartCapture(void* data) {
    StartArgs args = *static_cast<StartArgs*>(data);
    if (InterlockedCompareExchange(&running, 1, 0)) { CloseHandle(args.mapping); CloseHandle(args.collector); return ERROR_BUSY; }
    auto* r = static_cast<Ring*>(MapViewOfFile(args.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    CloseHandle(args.mapping);
    if (!r) { CloseHandle(args.collector); InterlockedExchange(&running, 0); return GetLastError(); }
    DWORD error = 0;
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(L"audioeng.dll"));
    BYTE expected[] = {0x48,0x8b,0xc4,0x48,0x89,0x58,0x08};
    if (_wcsicmp(name, L"audiodg.exe") || !base || r->magic != kMagic || r->version != 1) error = ERROR_BAD_ENVIRONMENT;
    if (!error) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize ||
            (!created && memcmp(base + kMonoRva, expected, sizeof(expected)))) error = ERROR_REVISION_MISMATCH;
    }
    if (!error && !created) {
        HMODULE pinned{};
        // Deliberately pin code/trampoline until audiodg exits: delayed hook
        // return addresses cannot execute freed memory after stop/re-attach.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(self), &pinned)) error = GetLastError();
        if (!error) {
            auto status = MH_Initialize();
            if (status != MH_OK) error = 0x2000 + status;
        }
        if (!error) {
            target = base + kMonoRva;
            auto status = MH_CreateHook(target, reinterpret_cast<void*>(Hook), reinterpret_cast<void**>(&original));
            if (status != MH_OK) error = 0x2100 + status;
            else created = true;
        }
    }
    HANDLE collector = args.collector; // Duplicated SYNCHRONIZE handle, no cross-user OpenProcess.
    if (!error && !collector) error = ERROR_INVALID_HANDLE;
    if (!error) {
        r->enginePid = GetCurrentProcessId();
        InterlockedExchangePointer(&currentRing, r);
        auto status = MH_EnableHook(target);
        if (status != MH_OK) error = 0x2200 + status;
    }
    if (!error) {
        InterlockedExchange(&r->status, 1);
        for (;;) {
            // Watchdog runs on this management thread, never an audio thread.
            if (r->stop || WaitForSingleObject(collector, 100) != WAIT_TIMEOUT ||
                static_cast<LONG64>(GetTickCount64()) - Read64(&r->heartbeat) > 5000) break;
        }
        InterlockedExchange(&r->stop, 1);
        auto status = MH_DisableHook(target);
        if (status != MH_OK) error = 0x2300 + status;
    }
    // Exchange before waiting: late-entering hooks see null, existing ones
    // retain their mapping until they have completed their original call.
    InterlockedExchangePointer(&currentRing, nullptr);
    while (InterlockedCompareExchange(&active, 0, 0)) Sleep(1);
    if (collector) CloseHandle(collector);
    r->error = error;
    InterlockedExchange(&r->status, error ? -1 : 2);
    UnmapViewOfFile(r);
    InterlockedExchange(&running, 0);
    return error;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) { self = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
