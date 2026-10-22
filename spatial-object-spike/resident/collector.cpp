#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <share.h>
#include <psapi.h>
#include "protocol.h"
#include "metadata_trace.h"
#include "position_cache.h"
namespace fs = std::filesystem;
static volatile LONG interrupted;
static bool dolbyBackend=true;
static bool metadataBackend;
static bool statefulBackend=true;
static bool quantumBackend;
static uint64_t PrivateBytes(HANDLE p) {
    PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof(m);
    return GetProcessMemoryInfo(p,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m)) ? m.PrivateUsage : 0;
}
static BOOL WINAPI Ctrl(DWORD) { InterlockedExchange(&interrupted, 1); return TRUE; }
static void Require(bool ok, const char* what) {
    if (!ok) { printf("ERROR %s win32=%lu\n", what, GetLastError()); throw std::runtime_error(what); }
}
static LONG64 Read64(volatile LONG64* p) { return InterlockedCompareExchange64(p, 0, 0); }
static DWORD EnginePid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W p{}; p.dwSize = sizeof(p); DWORD pid = 0, count = 0;
    if (Process32FirstW(s, &p)) do {
        if (!_wcsicmp(p.szExeFile, L"audiodg.exe")) { pid = p.th32ProcessID; ++count; }
    } while (Process32NextW(s, &p));
    CloseHandle(s); return count == 1 ? pid : 0;
}
static uintptr_t Module(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32W m{}; m.dwSize = sizeof(m); uintptr_t base = 0;
    if (Module32FirstW(s, &m)) do {
        if (!_wcsicmp(m.szModule, name)) { base = reinterpret_cast<uintptr_t>(m.modBaseAddr); break; }
    } while (Module32NextW(s, &m));
    CloseHandle(s); return base;
}
static HANDLE RemoteThread(HANDLE process, uintptr_t entry, void* arg) {
    HANDLE t = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), arg, 0, nullptr);
    Require(t != nullptr, "CreateRemoteThread"); return t;
}
static void* RemoteCopy(HANDLE process, const void* data, size_t bytes) {
    void* p = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Require(p != nullptr, "VirtualAllocEx");
    Require(WriteProcessMemory(process, p, data, bytes, nullptr) != FALSE, "WriteProcessMemory"); return p;
}
static uintptr_t LoadTap(HANDLE process, DWORD pid, const fs::path& path) {
    uintptr_t base = Module(pid, path.filename().c_str());
    if (base) return base;
    auto loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner{};
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(loader), &owner) != FALSE, "Loader owner");
    wchar_t ownerPath[MAX_PATH]{}; GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    uintptr_t remoteOwner = Module(pid, fs::path(ownerPath).filename().c_str());
    Require(remoteOwner != 0, "Remote loader module");
    std::wstring value = path.wstring();
    void* arg = RemoteCopy(process, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
    HANDLE t = RemoteThread(process, remoteOwner + reinterpret_cast<uintptr_t>(loader) - reinterpret_cast<uintptr_t>(owner), arg);
    DWORD wait = WaitForSingleObject(t, 10000); DWORD loaderExit{}; GetExitCodeThread(t, &loaderExit); CloseHandle(t);
    printf("LoadLibrary remoteThreadExit=0x%08lX\n", loaderExit);
    // Never free memory a timed-out loader might still be reading.
    Require(wait == WAIT_OBJECT_0, "LoadLibrary timeout (argument retained)");
    VirtualFreeEx(process, arg, 0, MEM_RELEASE);
    base = Module(pid, path.filename().c_str()); Require(base != 0, "Loaded engine_tap module");
    return base;
}
static uint64_t CpuTime(HANDLE p) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(p, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a{}, b{}; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime; return a.QuadPart + b.QuadPart;
}
static bool OriginalBytes(HANDLE process, DWORD pid) {
    BYTE actual[7]{}; BYTE expected[] = {0x48,0x8b,0xc4,0x48,0x89,0x58,0x08}; SIZE_T got{};
    if (dolbyBackend) expected[6] = 0x10;
    auto base = Module(pid, L"audioeng.dll");
    return base && ReadProcessMemory(process, reinterpret_cast<void*>(base + (dolbyBackend ? 0xfa3b0 : kMonoRva)), actual, sizeof(actual), &got) &&
        got == sizeof(actual) && !memcmp(actual, expected, sizeof(actual));
}
#ifndef SPATIAL_COLLECTOR_HELPERS_ONLY
int wmain(int argc, wchar_t** argv) {
    int seconds = 0, maxMb = 512, chunkMb=128; fs::path out; DWORD stopPid = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (a == L"--output" && i + 1 < argc) out = argv[++i];
        else if (a == L"--max-mb" && i + 1 < argc) maxMb = _wtoi(argv[++i]);
        else if (a == L"--chunk-mb" && i + 1 < argc) chunkMb = _wtoi(argv[++i]);
        else if (a == L"--backend" && i + 1 < argc) { std::wstring b = argv[++i]; if (b != L"sonic" && b != L"dolby" && b != L"metadata" && b!=L"dolby-raw" && b!=L"dolby-quantum") return 2; dolbyBackend = b != L"sonic"; metadataBackend = b == L"metadata"; statefulBackend=b==L"dolby";quantumBackend=b==L"dolby-quantum"; }
        else if (a == L"--stop" && i + 1 < argc) stopPid = _wtol(argv[++i]);
        else { puts("Usage: collector --output NEW_DIRECTORY [--seconds N] [--max-mb 512] | --stop COLLECTOR_PID"); return 2; }
    }
    wchar_t eventName[96]{};
    swprintf_s(eventName, L"Local\\SpatialResidentStop_%lu", stopPid ? stopPid : GetCurrentProcessId());
    if (stopPid) {
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (!e) return 1; BOOL ok = SetEvent(e); CloseHandle(e); return ok ? 0 : 1;
    }
    if (out.empty() || seconds < 0 || maxMb < 1 || chunkMb<1) return 2;
    SetConsoleCtrlHandler(Ctrl, TRUE); setvbuf(stdout, nullptr, _IONBF, 0);
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES p{}; p.PrivilegeCount = 1; LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &p.Privileges[0].Luid);
        p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; AdjustTokenPrivileges(token, FALSE, &p, 0, nullptr, nullptr); CloseHandle(token);
    }
    HANDLE process{}, mapping{}, thread{}; Ring* ring{}; void* remoteArgs{};
    FILE* binary{}; FILE* metrics{}; FILE* metadata{}; DWORD pid{}; int result = 1;
    try {
        Require(!fs::exists(out) && fs::create_directories(out), "Create NEW output directory");
        pid = EnginePid(); Require(pid != 0, "Expected exactly one audiodg instance");
        process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
            PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
        Require(process != nullptr, "Open audiodg (run elevated)");
        Require(OriginalBytes(process, pid), "Original version-specific function prologue");
        size_t ringBytes=quantumBackend?sizeof(QuantumRing):statefulBackend?sizeof(StatefulRing):sizeof(Ring);
        PositionCache cache;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(ringBytes), nullptr);
        Require(mapping != nullptr, "Create shared ring");
        ring = static_cast<Ring*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, ringBytes));
        Require(ring != nullptr, "Map shared ring");
        memset(ring, 0, ringBytes); // Pre-fault every queue page outside audio process.
        ring->magic = kMagic; ring->version = quantumBackend?3:statefulBackend?2:1; ring->collectorPid = GetCurrentProcessId();
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); ring->qpcFrequency = frequency.QuadPart;
        InterlockedExchange64(&ring->heartbeat, GetTickCount64());
        HANDLE remoteMapping{};
        Require(DuplicateHandle(GetCurrentProcess(), mapping, process, &remoteMapping, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE, "Duplicate ring handle");
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // audiodg runs as LOCAL SERVICE, which cannot read a private user profile.
        wchar_t programData[MAX_PATH]{}; GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
        fs::path dll = fs::path(programData) / L"SpatialObjectSpike" / (dolbyBackend ? L"engine_tap_dolby_v3.dll" : L"engine_tap_v2.dll");
        if(metadataBackend) dll=fs::path(programData)/L"SpatialObjectSpike"/L"engine_tap_metadata_v1.dll";
        if(statefulBackend) dll=fs::path(programData)/L"SpatialObjectSpike"/L"engine_tap_stateful_v1.dll";
        if(quantumBackend) dll=fs::path(programData)/L"SpatialObjectSpike"/L"engine_tap_quantum_v1.dll";
        Require(fs::exists(dll), "Deploy engine_tap.dll to ProgramData/SpatialObjectSpike first");
        uintptr_t remoteDll = LoadTap(process, pid, dll);
        HMODULE localDll = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        Require(localDll != nullptr, "Read local export RVA");
        auto start = GetProcAddress(localDll, "StartCapture"); Require(start != nullptr, "StartCapture export");
        uintptr_t entry = remoteDll + reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(localDll);
        FreeLibrary(localDll);
        HANDLE remoteCollector{};
        Require(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), process, &remoteCollector, SYNCHRONIZE, FALSE, 0) != FALSE, "Duplicate watchdog process handle");
        StartArgs args{remoteMapping, remoteCollector}; remoteArgs = RemoteCopy(process, &args, sizeof(args));
        _wfopen_s(&binary, (out / L"global-objects.bin").c_str(), L"wb"); Require(binary != nullptr, "Open capture binary");
        setvbuf(binary, nullptr, _IOFBF, 1024 * 1024);
        if(metadataBackend) {
            _wfopen_s(&metadata,(out/L"property-trace.bin").c_str(),L"wb");Require(metadata!=nullptr,"Open property trace");
            setvbuf(metadata,nullptr,_IOFBF,1024*1024);
        }
        metrics = _wfsopen((out / L"metrics.csv").c_str(), L"wb", _SH_DENYWR); Require(metrics != nullptr, "Open metrics");
        fprintf(metrics, "elapsed_s,calls,blocks,frames,drop_full,drop_busy,invalid,queue_depth,copy_us_total,copy_us_max,audiodg_cpu_s,collector_cpu_s,audiodg_private_bytes,collector_private_bytes,position_updates,position_cached,position_unknown\n");
        FILE* format{};_wfopen_s(&format,(out/L"format.json").c_str(),L"wb");
        Require(format!=nullptr,"Open format description");
        if(quantumBackend)fprintf(format,"{\"version\":3,\"header_bytes\":112,\"sample_format\":\"mono-float32-le\",\"source_kind\":\"native-process-marker-probe\",\"scope\":\"one selected encoder observation domain\",\"event_kinds\":{\"1\":\"raw-object\",\"2\":\"encoder-process-end\"},\"object_offset\":\"not supplied; shorter-than-pass remains unknown\",\"sample_rate_verified\":false,\"position_cache_applied\":false}\n");
        else fprintf(format,"{\"version\":%d,\"header_bytes\":%d,\"sample_format\":\"mono-float32-le\",\"files\":\"global-objects.bin, global-objects-0001.bin, ... (one continuous sequence/cache)\",\"position_states\":{\"0\":\"unknown\",\"1\":\"updated\",\"2\":\"cached\",\"3\":\"create-default-origin\",\"4\":\"static-channel-implicit\"},\"unknown_coordinates\":\"NaN\",\"coordinate_space\":\"listener-relative-not-world\"}\n",statefulBackend?2:1,statefulBackend?112:64);
        fclose(format);
        uint64_t startTime = GetTickCount64(), nextReport = startTime, bytes = 0;
        uint64_t chunkBytes=0;unsigned chunks=1;
        uint64_t engineCpu = CpuTime(process), selfCpu = CpuTime(GetCurrentProcess());
        thread = RemoteThread(process, entry, remoteArgs);
        uint64_t readyDeadline = startTime + 10000;
        while (!ring->status && GetTickCount64() < readyDeadline && WaitForSingleObject(thread, 10) == WAIT_TIMEOUT) {}
        if (ring->status != 1) printf("INITIALIZE status=%ld error=%ld\n", ring->status, ring->error);
        Require(ring->status == 1, "Hook READY");
        printf("READY collectorPID=%lu audiodgPID=%lu queueBytes=%zu seconds=%d qpcHz=%lld stateful=%d\n", GetCurrentProcessId(), pid, ringBytes, seconds, frequency.QuadPart,statefulBackend);
        FILE* ready{}; _wfopen_s(&ready, (out / L"ready.txt").c_str(), L"wb"); if (ready) { fprintf(ready, "%lu %lu\n", GetCurrentProcessId(), pid); fclose(ready); }
        for (;;) {
            uint64_t now = GetTickCount64();
            InterlockedExchange64(&ring->heartbeat, now);
            if (interrupted || WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
                (seconds && now - startTime >= uint64_t(seconds) * 1000) || bytes >= uint64_t(maxMb) * 1024 * 1024 ||
                ((statefulBackend || quantumBackend) && Read64(&ring->extendedCalls))) InterlockedExchange(&ring->stop, 1);
            LONG64 read = Read64(&ring->read), write = Read64(&ring->write);
            while (read < write) {
                const StatefulSlot* ss=statefulBackend?&reinterpret_cast<StatefulRing*>(ring)->slots[read%kSlots]:nullptr;
                const QuantumSlot* qs=quantumBackend?&reinterpret_cast<QuantumRing*>(ring)->slots[read%kSlots]:nullptr;
                const Record& record=qs?qs->record:ss?ss->record:ring->slots[read%kSlots].record;
                const float* samples=qs?qs->samples:ss?ss->samples:ring->slots[read%kSlots].samples;
                Require(record.magic == 0x474f424a && (record.frames || (qs && qs->kind==kQuantumProcess)) && record.frames <= kSamples && record.sequence == uint64_t(read), "Ring integrity");
                if(chunkBytes>=uint64_t(chunkMb)*1024*1024) {
                    int closeResult=fclose(binary);binary=nullptr;Require(closeResult==0,"Flush capture chunk");
                    wchar_t filename[64]{};swprintf_s(filename,L"global-objects-%04u.bin",chunks++);
                    _wfopen_s(&binary,(out/filename).c_str(),L"wb");Require(binary!=nullptr,"Open next capture chunk");
                    setvbuf(binary,nullptr,_IOFBF,1024*1024);chunkBytes=0;
                }
                if(qs) {
                    Require(ring->backend==4 && (qs->kind==kQuantumObject || qs->kind==kQuantumProcess),"Quantum queue ABI");
                    QuantumRecord event{record,qs->propertyFlags,qs->kind,qs->lossEpoch,qs->encoder,qs->inputFrames,qs->result,qs->producedBytes,qs->capacity,0};
                    Require(fwrite(&event,sizeof(event),1,binary)==1,"Write quantum event header");
                } else if(ss) {
                    Require(ring->backend==3,"Stateful queue ABI");
                    StateRecord state=cache.Apply(record,ss->propertyFlags,ss->lossEpoch);
                    Require(fwrite(&state,sizeof(state),1,binary)==1,"Write state header");
                } else Require(fwrite(&record,sizeof(record),1,binary)==1,"Write legacy header");
                Require(fwrite(samples, 4, record.frames, binary) == record.frames, "Disk write");
                if(metadataBackend) {
                    const auto* t=reinterpret_cast<const PropertyTrace*>(samples+kTraceFloatOffset);
                    Require(ring->backend==2 && t->magic==kTraceMagic && record.frames<=kTraceFloatOffset,"Property trace ABI");
                    Require(fwrite(&record,sizeof(Record),1,metadata)==1 && fwrite(t,sizeof(PropertyTrace),1,metadata)==1,"Write property trace");
                }
                bytes += (qs?sizeof(QuantumRecord):ss?sizeof(StateRecord):sizeof(Record)) + record.frames * 4;
                chunkBytes += (qs?sizeof(QuantumRecord):ss?sizeof(StateRecord):sizeof(Record)) + record.frames * 4;
                if(metadataBackend) bytes+=sizeof(Record)+sizeof(PropertyTrace);
                InterlockedExchange64(&ring->read, ++read);
            }
            if (now >= nextReport) {
                double us = 1e6 / frequency.QuadPart;
                fprintf(metrics, "%.3f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%.3f,%.3f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu\n", (now - startTime) / 1000.,
                    Read64(&ring->calls), Read64(&ring->write), Read64(&ring->frames), Read64(&ring->droppedFull), Read64(&ring->droppedBusy), Read64(&ring->invalid),
                    Read64(&ring->write) - Read64(&ring->read), Read64(&ring->copyTicks) * us, Read64(&ring->maxCopyTicks) * us,
                    (CpuTime(process) - engineCpu) / 1e7, (CpuTime(GetCurrentProcess()) - selfCpu) / 1e7,
                    PrivateBytes(process),PrivateBytes(GetCurrentProcess()),cache.updates,cache.cached,cache.unknown);
                fflush(metrics); nextReport = now + 1000;
            }
            if (WaitForSingleObject(thread, 0) != WAIT_TIMEOUT && Read64(&ring->read) == Read64(&ring->write)) break;
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) { puts("ENGINE_EXIT: capture stopped; restart collector for new audiodg"); break; }
            Sleep(5);
        }
        Require(fflush(binary)==0 && (!metadata || fflush(metadata)==0),"Flush final audio/metadata");
        double elapsed = (GetTickCount64() - startTime) / 1000., us = 1e6 / frequency.QuadPart;
        bool restored = WaitForSingleObject(process, 0) == WAIT_OBJECT_0 || OriginalBytes(process, pid);
        printf("STOP status=%ld error=%ld blocks=%lld frames=%lld droppedFull=%lld droppedBusy=%lld invalid=%lld originalBytesRestored=%d\n",
            ring->status, ring->error, Read64(&ring->write), Read64(&ring->frames), Read64(&ring->droppedFull), Read64(&ring->droppedBusy), Read64(&ring->invalid), restored);
        printf("BACKEND dolby=%d renderCalls=%lld encoderReady=%ld encoderRva=0x%llX extendedCalls=%lld\n", dolbyBackend, Read64(&ring->renderCalls),ring->encoderReady,ring->encoderRva,Read64(&ring->extendedCalls));
        if (dolbyBackend && ring->error == ERROR_NOT_SUPPORTED && !Read64(&ring->write)) printf("ENCODER_DIAGNOSTIC %ls\n",reinterpret_cast<wchar_t*>(ring->slots[0].samples));
        FILE* backendReport{}; _wfopen_s(&backendReport, (out / L"backend.json").c_str(), L"wb");
        if (backendReport) { fprintf(backendReport,"{\"dolby\":%s,\"render_calls\":%lld,\"encoder_ready\":%ld,\"encoder_rva\":%llu,\"extended_calls\":%lld}\n",dolbyBackend?"true":"false",Read64(&ring->renderCalls),ring->encoderReady,ring->encoderRva,Read64(&ring->extendedCalls)); fclose(backendReport); }
        FILE* positions{};_wfopen_s(&positions,(out/L"position-summary.json").c_str(),L"wb");
        if(positions) {fprintf(positions,"{\"stateful\":%s,\"updated\":%llu,\"cached\":%llu,\"unknown\":%llu,\"create_default_origin\":%llu,\"static_implicit\":%llu,\"creates\":%llu,\"ends\":%llu,\"recovered_after_unknown\":%llu,\"loss_invalidations\":%llu,\"evictions\":%llu,\"cache_peak\":%llu,\"cache_limit\":4096}\n",statefulBackend?"true":"false",cache.updates,cache.cached,cache.unknown,cache.defaults,cache.statics,cache.creates,cache.ends,cache.recovered,cache.gapInvalidations,cache.evictions,cache.peak);fclose(positions);}
        FILE* storage{};_wfopen_s(&storage,(out/L"storage.json").c_str(),L"wb");
        if(storage) {fprintf(storage,"{\"chunks\":%u,\"chunk_mb\":%d,\"cache_continuous_across_chunks\":true,\"bytes\":%llu}\n",chunks,chunkMb,bytes);fclose(storage);}
        FILE* report{}; _wfopen_s(&report, (out / L"summary.json").c_str(), L"wb");
        if (report) {
            fprintf(report, "{\"audiodg_pid\":%lu,\"collector_pid\":%lu,\"elapsed_seconds\":%.3f,\"blocks\":%lld,\"frames\":%lld,\"calls\":%lld,\"dropped_full\":%lld,\"dropped_busy\":%lld,\"invalid\":%lld,\"silent_blocks\":%lld,\"queue_high_water\":%lld,\"copy_us_total\":%.3f,\"copy_us_max\":%.3f,\"audiodg_cpu_seconds\":%.6f,\"collector_cpu_seconds\":%.6f,\"status\":%ld,\"error\":%ld,\"original_bytes_restored\":%s,\"dll_pinned_until_engine_exit\":true,\"qpc_frequency\":%lld,\"bytes\":%llu}\n",
                pid, GetCurrentProcessId(), elapsed, Read64(&ring->write), Read64(&ring->frames), Read64(&ring->calls), Read64(&ring->droppedFull), Read64(&ring->droppedBusy),
                Read64(&ring->invalid), Read64(&ring->silent), Read64(&ring->highWater), Read64(&ring->copyTicks) * us, Read64(&ring->maxCopyTicks) * us,
                (CpuTime(process) - engineCpu) / 1e7, (CpuTime(GetCurrentProcess()) - selfCpu) / 1e7, ring->status, ring->error, restored ? "true" : "false", frequency.QuadPart, bytes);
            fclose(report);
        }
        result = ring->status == 2 && restored && !ring->error && !ring->invalid && !ring->droppedFull && !ring->droppedBusy && (!(statefulBackend || quantumBackend) || !ring->extendedCalls) ? 0 : 1;
    } catch (const std::exception& e) { printf("FAILED %s\n", e.what()); }
    if (ring) InterlockedExchange(&ring->stop, 1);
    bool finished = !thread || WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0;
    if (remoteArgs && finished) VirtualFreeEx(process, remoteArgs, 0, MEM_RELEASE);
    if (thread) CloseHandle(thread);
    if (metrics) fclose(metrics); if (binary) fclose(binary); if(metadata) fclose(metadata);
    if (ring) UnmapViewOfFile(ring); if (mapping) CloseHandle(mapping);
    if (process) CloseHandle(process); if (stopEvent) CloseHandle(stopEvent);
    return result;
}
#endif
