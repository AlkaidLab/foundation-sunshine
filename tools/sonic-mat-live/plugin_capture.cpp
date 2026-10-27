#include "native_session.h"
#include "plugin_bridge.h"
#include "capture_options.h"
#include "components.h"
static bool
OriginalPluginBytes(HANDLE engine) {
  constexpr DWORD rvas[] = { 0xfa3b0, 0x2fd90, 0x45be0, 0x31e00, 0x74b50, 0x733f8 };
  constexpr BYTE expected[][8] = { { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55 }, { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x49, 0x08 }, { 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x49, 0x08 }, { 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x49, 0x08 }, { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x49, 0x08 }, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b } };
  auto base = Module(GetProcessId(engine), L"audioeng.dll");
  if (!base) return false;
  for (unsigned n = 0; n < 6; ++n) {
    BYTE bytes[8];
    SIZE_T got = 0;
    if (!ReadProcessMemory(engine, (void *) (base + rvas[n]), bytes, 8, &got) || got != 8 || memcmp(bytes, expected[n], 8)) return false;
  }
  return true;
}
static uint32_t
PluginRate(HANDLE engine, uint64_t renderer) {
  // Matching Microsoft PE/PDB: MainPluginRenderer::Initialize reads the
  // status entry at +0x18, sample rate F32 at status+0x108 for Initialize.
  uintptr_t status = 0;
  float rate = 0;
  SIZE_T got = 0;
  require(ReadProcessMemory(engine, (void *) (renderer + 0x18), &status, sizeof(status), &got) && got == sizeof(status) && status, "Plugin status identity");
  require(ReadProcessMemory(engine, (void *) (status + 0x108), &rate, sizeof(rate), &got) && got == sizeof(rate) && rate == 48000.f, "Plugin native input rate must be observed 48kHz");
  return 48000;
}
int
wmain(int argc, wchar_t **argv) {
  if (argc == 4 && std::wstring(argv[1]) == L"--preflight") {
    HANDLE process = nullptr;
    int code = 1;
    try {
      if (!sonic_live::report_components(mat_compat::audio_engine(), sonic_live::find_dolby_access().component)) {
        puts("PREFLIGHT_UNSUPPORTED system audio engine or Dolby Access build has not been verified; attach=0");
        return 3;
      }
      auto pid = std::stoull(argv[2]), created = std::stoull(argv[3]);
      require(pid && pid <= MAXDWORD && created, "Explicit plugin engine identity");
      process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD) pid);
      require(process && native_mat_guard::created(process) == created && native_mat_guard::system_engine(process), "Read-only pinned plugin engine identity/hash");
      require(OriginalPluginBytes(process), "Original six plugin entry points");
      puts("PREFLIGHT plugin identity/hash/entries=6 passed; nativeReady=unverified attach=0");
      code = 0;
    }
    catch (const std::exception &e) {
      printf("PREFLIGHT_FAILED %s win32=%lu\n", e.what(), GetLastError());
    }
    if (process) CloseHandle(process);
    return code;
  }
  if ((argc != 11 && argc != 12) || (std::wstring(argv[1]) != L"--attach" && std::wstring(argv[1]) != L"--reattach")) {
    puts("plugin-mat-capture --preflight PID CREATED | --attach/--reattach PID CREATED TAP TAP_SHA CODEC CODEC_SHA LIVE_PIPE STOP_EVENT (--continuous | --max-seconds 1..60)");
    return 2;
  }
  HANDLE engine {}, map {}, thread {}, stop {}, remoteMap {}, remoteOwner {};
  void *arguments {};
  q4::Ring *ring {};
  std::thread controller;
  Shared shared;
  std::unique_ptr<NativeSession> native;
  bool handed = false;
  int result = 1;
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleCtrlHandler(Ctrl, TRUE);
  try {
    auto limit = sonic_live::capture_limit(argc - 10, argv[10], argc == 12 ? argv[11] : nullptr);
    auto pid64 = std::stoull(argv[2]), creation = std::stoull(argv[3]);
    require(pid64 && pid64 <= MAXDWORD && creation, "Explicit audiodg identity");
    DWORD pid = (DWORD) pid64;
    const auto dolby = sonic_live::find_dolby_access();
    require(sonic_live::report_components(mat_compat::audio_engine(), dolby.component), "Unsupported system component; see COMPONENT lines");
    engine = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
    require(engine && native_mat_guard::created(engine) == creation && native_mat_guard::system_engine(engine), "Pinned plugin audiodg identity/hash");
    require(OriginalPluginBytes(engine), "Original six plugin entry points");
    fs::path tap = fs::canonical(argv[4]), codec = fs::canonical(argv[6]);
    std::wstring th = argv[5], ch = argv[7];
    HANDLE tapLock = CreateFileW(tap.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr), codecLock = CreateFileW(codec.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    struct Unlock {
      HANDLE a, b;
      ~Unlock() {
        if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
        if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
      }
    } unlock { tapLock, codecLock };
    require(tapLock != INVALID_HANDLE_VALUE && codecLock != INVALID_HANDLE_VALUE, "Lock reviewed plugin DLLs");
    require(native_mat_guard::sha256(tap) == std::string(th.begin(), th.end()) && native_mat_guard::sha256(codec) == std::string(ch.begin(), ch.end()), "Both plugin DLL hashes");
    std::wstring pipe = argv[8], event = argv[9];
    require(pipe.starts_with(L"\\\\.\\pipe\\SunshineMatLive-") && pipe.find_first_of(L"\r\n") == std::wstring::npos && event.starts_with(L"Local\\SunshineMatLiveStop-"), "Private plugin pipe/stop");
    auto acl = PipeAcl();
    SECURITY_ATTRIBUTES sa { sizeof(sa), acl, FALSE };
    stop = CreateEventW(&sa, TRUE, FALSE, event.c_str());
    auto code = GetLastError();
    LocalFree(acl);
    require(stop && code != ERROR_ALREADY_EXISTS, "Fresh plugin stop event");
    bool reattach = std::wstring(argv[1]) == L"--reattach";
    auto tapBase = LoadReviewed(engine, pid, tap, std::string(th.begin(), th.end()), reattach);
    auto codecBase = LoadReviewed(engine, pid, codec, std::string(ch.begin(), ch.end()), reattach);
    map = Map(sizeof(q4::Ring));
    ring = (q4::Ring *) MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*ring));
    require(ring, "Map paired plugin ring");
    memset(ring, 0, sizeof(*ring));
    ring->magic = kMagic;
    ring->version = q4::version;
    ring->collectorPid = GetCurrentProcessId();
    ring->qpcFrequency = receiver::hz();
    InterlockedExchange64(&ring->heartbeat, GetTickCount64());
    remoteMap = DuplicateTo(engine, map);
    remoteOwner = DuplicateTo(engine, GetCurrentProcess(), SYNCHRONIZE);
    StartArgs args { remoteMap, remoteOwner };
    arguments = RemoteCopy(engine, &args, sizeof(args));
    thread = RemoteThread(engine, Export(tap, tapBase, "StartCapture"), arguments);
    handed = true;
    auto began = GetTickCount64();
    while (!ring->status && GetTickCount64() - began < 2000 && WaitForSingleObject(thread, 10) == WAIT_TIMEOUT) InterlockedExchange64(&ring->heartbeat, GetTickCount64());
    require(ring->status == 1, "Plugin paired hooks ready");
    auto epoch = (Qpc() & INT64_MAX) | 1;
    controller = std::thread([&, codec, codecBase, vendor = dolby.directory, epoch, began, limit] {try {
            q4::Assembler assembler;sonic_live::PluginBridge bridge;std::map<uint64_t,uint32_t> rates;std::map<uint64_t,int64_t> starts;uint64_t selected=0;
            for(;;){auto now=GetTickCount64();InterlockedExchange64(&ring->heartbeat,now);if(native)native->heartbeat();
                require(!Read64(&ring->droppedFull)&&!Read64(&ring->droppedBusy)&&!Read64(&ring->invalid)&&!Read64(&ring->unboundAfterArming)&&!ring->error,"Plugin callback loss/ABI violation");require(WaitForSingleObject(engine,0)==WAIT_TIMEOUT,"Pinned plugin engine exited");
                auto r=Read64(&ring->read),w=Read64(&ring->write);require(w>=r&&w-r<=kSlots,"Plugin ring bounds");
                while(r<w){if(!q4::recordReady(*ring,r))break;const auto& s=ring->slots[r%kSlots];const auto& h=s.header;
                    if(h.kind==q4::DomainBegin){rates[h.domain]=PluginRate(engine,h.renderer);printf("PLUGIN_DOMAIN domain=%llu renderer=0x%llx rate=%u\n",h.domain,h.renderer,rates[h.domain]);}
                    if(h.kind==q4::PassBegin)starts[h.domain]=h.audio.qpc;
                    q4::Quantum quantum;if(assembler.push(h,s.pcm,quantum)){
                        if(!selected&&!quantum.objects.empty()){selected=quantum.domain;uint32_t mask=0,dynamic=0;
                            for(const auto& o:quantum.objects){if(o.source.audio.type==1)++dynamic;else mask|=o.source.audio.type;}
                            printf("PLUGIN_SOURCE domain=%llu renderer=0x%llx objects=%zu frames=%u rate=%u staticMask=0x%x dynamicObjects=%u\n",selected,quantum.renderer,quantum.objects.size(),quantum.frames,rates.at(selected),mask,dynamic);}
                        if(selected&&quantum.domain!=selected)require(quantum.objects.empty(),"Multiple object-bearing plugin domains");
                        else if(selected)bridge.push(quantum,starts.at(h.domain),h.audio.qpc,rates.at(h.domain),[&](const receiver::Slot& packet){if(!native)native=std::make_unique<NativeSession>(engine,codec,codecBase,vendor,bridge.renderer(),epoch,shared,"plugin-reencode");return native->send(packet);});
                    }
                    if(h.kind==q4::DomainEnd){
                        rates.erase(h.domain);starts.erase(h.domain);
                        if(h.domain==selected)require(h.reason==q4::CaptureStop,"Selected plugin domain retired; end epoch");
                    }
                    InterlockedExchange64(&ring->read,++r);
                }
                {std::lock_guard lock(shared.mutex);require(!shared.failed,"Native plugin MAT output/consumer failed");}
                if(limit&&GetTickCount64()-began>=limit*1000)SetEvent(stop);if(interrupted||WaitForSingleObject(stop,0)==WAIT_OBJECT_0)InterlockedExchange(&ring->stop,1);
                if(WaitForSingleObject(thread,0)==WAIT_OBJECT_0){if(Read64(&ring->read)!=Read64(&ring->write))continue;
                    require(ring->status==2&&!ring->error&&(interrupted||WaitForSingleObject(stop,0)==WAIT_OBJECT_0),"Unclean plugin capture end");assembler.finish();
                    if(native){bridge.finish([&](const receiver::Slot& packet){return native->send(packet);});native->finish();}
                    else {std::lock_guard lock(shared.mutex);shared.ended=true;shared.changed.notify_all();puts("NO_NATIVE_SOURCE observedCycles=0 noEOS=1");}
                    printf("PLUGIN_STOP passes=%llu inputSamples=%llu outputOnlyPasses=%llu restored=1\n",bridge.passes(),bridge.samples(),bridge.outputOnlyPasses());break;}
                Sleep(1);
            }
        }catch(const std::exception& e){Broken(shared,e.what());InterlockedExchange(&ring->stop,1);} });
    mat_capture::Descriptor descriptor;
    descriptor.kind = mat_capture::native_object_source;
    descriptor.publisher_pid = GetCurrentProcessId();
    descriptor.publisher_created = native_mat_guard::created(GetCurrentProcess());
    descriptor.engine_pid = pid;
    descriptor.engine_created = creation;
    descriptor.epoch = epoch;
    descriptor.qpc_hz = receiver::hz();
    printf("ATTACHED source=plugin-objects-to-native-MAT engine=%lu fileInput=0 endpointChange=0\n", pid);
    Serve(shared, pipe, descriptor);
    InterlockedExchange(&ring->stop, 1);
    controller.join();
    require(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && ring->status == 2 && !ring->error, "Plugin hooks did not restore cleanly");
    printf("STOP cycles=%llu failed=%d error=%s restored=1 renderCalls=%lld calls=%lld invalid=%lld\n", shared.cycles, shared.failed, shared.error.c_str(), Read64(&ring->renderCalls), Read64(&ring->calls), Read64(&ring->invalid));
    result = shared.failed ? 1 : 0;
  }
  catch (const std::exception &e) {
    printf("FAILED %s win32=%lu\n", e.what(), GetLastError());
    if (ring) InterlockedExchange(&ring->stop, 1);
    InterlockedExchange(&interrupted, 1);
    if (controller.joinable()) controller.join();
  }
  native.reset();
  bool done = !thread || WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0;
  if (engine && ring) {
    bool restored = done && OriginalPluginBytes(engine);
    printf("HOOK_STOP restored=%d nativeStatus=%ld nativeError=%ld entries=6\n", restored, ring->status, ring->error);
    if (!restored) result = 1;
  }
  if (!handed && engine) {
    CloseRemote(engine, remoteMap);
    CloseRemote(engine, remoteOwner);
  }
  if (done && arguments) VirtualFreeEx(engine, arguments, 0, MEM_RELEASE);
  if (ring) UnmapViewOfFile(ring);
  if (thread) CloseHandle(thread);
  if (map) CloseHandle(map);
  if (stop) CloseHandle(stop);
  if (engine) CloseHandle(engine);
  return result;
}
