#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include <filesystem>
#include <stdexcept>
#include <map>
#include "capture_protocol.h"

static void Require(bool condition, const char* what) {
    if (!condition) {
        printf("ERROR %s win32=%lu\n", what, GetLastError());
        throw std::runtime_error(what);
    }
}

// This launcher only starts/instruments our adjacent spatial_probe.exe.
// It deliberately has no interface for attaching to arbitrary PIDs.
static void LoadTap(HANDLE process, DWORD pid, const std::wstring& dll) {
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Require(remote != nullptr, "VirtualAllocEx");
    Require(WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr) != FALSE, "WriteProcessMemory");
    auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    Require(load != nullptr, "GetProcAddress LoadLibraryW");
    HMODULE owner = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(load), &owner) != FALSE, "GetModuleHandleEx");
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW(owner, modulePath, MAX_PATH);
    std::wstring moduleName = std::filesystem::path(modulePath).filename().wstring();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    Require(snapshot != INVALID_HANDLE_VALUE, "ModuleSnapshot");
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    BYTE* remoteModule = nullptr;
    if (Module32FirstW(snapshot, &me)) do {
        if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) { remoteModule = me.modBaseAddr; break; }
    } while (Module32NextW(snapshot, &me));
    CloseHandle(snapshot);
    Require(remoteModule != nullptr, "Remote loader module");
    auto entry = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteModule +
        (reinterpret_cast<BYTE*>(load) - reinterpret_cast<BYTE*>(owner)));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, remote, 0, nullptr);
    Require(thread != nullptr, "CreateRemoteThread LoadLibraryW");
    DWORD wait = WaitForSingleObject(thread, 10000);
    Require(wait == WAIT_OBJECT_0, "Wait LoadLibraryW");
    DWORD result = 0; GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    Require(result != 0, "LoadLibraryW result");
    printf("Injected capture-only DLL into controlled source PID=%lu\n", pid);
}

static void WriteWavHeader(FILE* f, uint32_t rate, uint32_t bytes) {
    uint32_t riff = bytes + 36, formatSize = 16, avg = rate * 4;
    uint16_t format = 3, channels = 1, align = 4, bits = 32;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&formatSize, 4, 1, f); fwrite(&format, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int wmain(int argc, wchar_t** argv) {
    int device = 1;
    int seconds = 5;
    unsigned objectCount = 2;
    bool control = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--dev" && i + 1 < argc) device = _wtoi(argv[++i]);
        else if (arg == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (arg == L"--objects" && i + 1 < argc) objectCount = _wtoi(argv[++i]);
        else if (arg == L"--control") control = true;
        else return 2;
    }
    if (device < 0 || seconds < 1 || seconds > 30 || objectCount < 1 || objectCount > 2) return 2;
    PROCESS_INFORMATION pi{};
    HANDLE mapping = nullptr, gate = nullptr;
    TapRing* ring = nullptr;
    struct Output { FILE* file = nullptr; uint64_t frames = 0; uint32_t rate = 0; };
    std::map<uint64_t, Output> outputs;
    FILE* csv = nullptr;
    int result = 1;
    try {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path folder = std::filesystem::path(exe).parent_path();
        std::wstring source = (folder / L"spatial_probe.exe").wstring();
        std::wstring dll = (folder / L"spatial_tap.dll").wstring();
        wchar_t gateName[128];
        swprintf_s(gateName, L"Local\\SpatialSourceGo_%lu_%llu", GetCurrentProcessId(), GetTickCount64());
        gate = CreateEventW(nullptr, TRUE, FALSE, gateName);
        Require(gate != nullptr, "Create gate");
        std::wstring cmd = L"\"" + source + L"\" --emit --dev " + std::to_wstring(device) +
            L" --seconds " + std::to_wstring(seconds) + L" --objects " + std::to_wstring(objectCount) + L" --gate " + gateName;
        STARTUPINFOW si{}; si.cb = sizeof(si);
        Require(CreateProcessW(source.c_str(), cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi) != FALSE, "Create source process");
        wchar_t ringName[128]; swprintf_s(ringName, L"Local\\SpatialTapRing_%lu", pi.dwProcessId);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TapRing), ringName);
        Require(mapping != nullptr, "Create ring");
        ring = reinterpret_cast<TapRing*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TapRing)));
        Require(ring != nullptr, "Map ring");
        ring->magic = kTapMagic;
        ring->version = 1;
        // Allow the child's normal loader to reach its bounded wait in main.
        Sleep(300);
        if (!control) {
            LoadTap(pi.hProcess, pi.dwProcessId, dll);
            ULONGLONG readyDeadline = GetTickCount64() + 10000;
            while (!InterlockedCompareExchange(&ring->ready, 0, 0) && GetTickCount64() < readyDeadline) Sleep(10);
            Require(ring->ready == 1, "Tap hook initialization");
        }
        printf("%s collectorPID=%lu sourcePID=%lu physicalEndpointIndex=%d\n", control ? "CONTROL NO DLL" : "TAP READY", GetCurrentProcessId(), pi.dwProcessId, device);
        _wfopen_s(&csv, control ? L"control-objects.csv" : L"objects.csv", L"wb");
        Require(csv != nullptr, "Open metadata file");
        fprintf(csv, "sequence,pid,object_id,object_type,frame_offset,qpc,sample_rate,frames,x,y,z,volume,wav_offset\n");
        SetEvent(gate);
        uint64_t frames = 0;
        uint32_t rate = 0, blocks = 0;
        ULONGLONG deadline = GetTickCount64() + (seconds + 10) * 1000;
        bool ended = false;
        while (GetTickCount64() < deadline) {
            LONG read = InterlockedCompareExchange(&ring->read, 0, 0);
            LONG write = InterlockedCompareExchange(&ring->write, 0, 0);
            while (read < write) {
                const TapBlock& b = ring->blocks[read % kTapSlots];
                Require(b.frames <= kTapMaxSamples && b.objectId <= objectCount, "Supported capture block");
                Output& o = outputs[b.objectId];
                if (!o.file) {
                    std::wstring name = L"object-" + std::to_wstring(b.objectId) + L".wav";
                    _wfopen_s(&o.file, name.c_str(), L"wb");
                    Require(o.file != nullptr, "Open object WAV");
                    WriteWavHeader(o.file, b.sampleRate, 0);
                }
                Require(fwrite(b.samples, sizeof(float), b.frames, o.file) == b.frames, "Write object WAV");
                fprintf(csv, "%llu,%u,%llu,%u,%llu,%lld,%u,%u,%.9g,%.9g,%.9g,%.9g,%llu\n",
                    b.sequence, b.pid, b.objectId, b.objectType, b.frameOffset, b.qpc, b.sampleRate,
                    b.frames, b.x, b.y, b.z, b.volume, o.frames * sizeof(float));
                o.frames += b.frames; o.rate = b.sampleRate;
                frames += b.frames; rate = b.sampleRate; ++blocks;
                InterlockedExchange(&ring->read, ++read);
            }
            ended = WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0;
            if (ended && read == InterlockedCompareExchange(&ring->write, 0, 0)) break;
            Sleep(2);
        }
        DWORD childExit = STILL_ACTIVE; GetExitCodeProcess(pi.hProcess, &childExit);
        printf("CAPTURE objects=%zu blocks=%u totalObjectFrames=%llu rate=%u dropped=%ld childExit=%lu\n",
            outputs.size(), blocks, frames, rate, ring->dropped, childExit);
        printf("HOOKS device=%ld spatial=%ld stream=%ld object=%ld buffer=%ld end=%ld patchFailures=%ld\n",
            ring->deviceActivations, ring->spatialActivations, ring->streamActivations, ring->objectActivations,
            ring->bufferCalls, ring->endCalls, ring->patchFailures);
        for (auto& entry : outputs) {
            auto& o = entry.second;
            WriteWavHeader(o.file, o.rate, static_cast<uint32_t>(o.frames * sizeof(float)));
            printf("OBJECT id=%llu frames=%llu duration=%.3f\n", entry.first, o.frames, static_cast<double>(o.frames) / o.rate);
        }
        result = ended && childExit == 0 && ring->dropped == 0 &&
            (control ? (outputs.empty() && blocks == 0) : (outputs.size() == objectCount && blocks > 0)) ? 0 : 1;
    } catch (const std::exception& e) {
        printf("FAILED: %s\n", e.what());
    }
    for (auto& entry : outputs) if (entry.second.file) fclose(entry.second.file);
    if (csv) fclose(csv);
    if (pi.hProcess) {
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (ring) UnmapViewOfFile(ring);
    if (mapping) CloseHandle(mapping);
    if (gate) CloseHandle(gate);
    return result;
}
