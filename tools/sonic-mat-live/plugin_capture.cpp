#include "automatic_capture.h"
#include "capture_options.h"
#include "capture_runtime_hashes.h"
#include "components.h"
#include "native_session.h"
#include "plugin_bridge.h"
static std::wstring automaticUserSid;
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
  if (argc == 2 && std::wstring(argv[1]) == L"--wait-for-stop") {
    // Installers first stop Sunshine, which cancels owned capture. Wait only
    // for this installation's remaining publishers; never kill or unload them.
    const auto image = native_mat_guard::image(GetCurrentProcess());
    const auto deadline = GetTickCount64() + 30000;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 1;
    bool okay = true;
    PROCESSENTRY32W entry { sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"sunshine-plugin-mat-capture.exe")) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
          okay = false;
          continue;
        }
        if (_wcsicmp(native_mat_guard::image(process).c_str(), image.c_str()) == 0) {
          const auto now = GetTickCount64();
          if (WaitForSingleObject(process, now < deadline ? static_cast<DWORD>(deadline - now) : 0) != WAIT_OBJECT_0) okay = false;
        }
        CloseHandle(process);
      } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return okay ? 0 : 1;
  }
  if (argc == 6 && std::wstring(argv[1]) == L"--automatic") {
    try {
      const auto image = fs::path(native_mat_guard::image(GetCurrentProcess()));
      require(spatial_audio::mat_installed_image(image), "MAT capture must be installed in a protected Program Files directory");
      const auto pid = std::stoull(argv[2]), created = std::stoull(argv[3]);
      require(pid && pid <= MAXDWORD && created, "MAT owner identity");
      sonic_live::CaptureOwner owner(static_cast<DWORD>(pid), created, argv[4]);
      const auto host = fs::path(native_mat_guard::image(owner.process()));
      const auto expected = image.parent_path().parent_path().parent_path() / L"sunshine.exe";
      require(_wcsicmp(host.c_str(), expected.c_str()) == 0 && spatial_audio::mat_protected_entry(host), "MAT owner must be this installation's Sunshine");
      automaticUserSid = argv[5];
      if (!sonic_live::report_components(mat_compat::audio_engine(), sonic_live::find_dolby_access(automaticUserSid).component)) return 3;
      // A restart waits for its predecessor's cooperative cleanup. It never
      // force-kills a publisher or attaches over hooks that are still active.
      HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\SunshineMatCaptureLifecycle");
      require(mutex, "MAT lifecycle mutex");
      const auto acquired = WaitForSingleObject(mutex, 20000);
      struct Release {
        HANDLE handle;
        bool acquired;
        ~Release() {
          if (acquired) ReleaseMutex(handle);
          CloseHandle(handle);
        }
      } release { mutex, acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED };
      require(release.acquired, "Previous MAT capture is still stopping");
      sonic_live::EngineIdentity engine {};
      try {
        engine = sonic_live::automatic_engine();
      }
      catch (const std::exception &e) {
        printf("AUTOMATIC_UNAVAILABLE %s\n", e.what());
        return 4;
      }
      require(!owner.cancelled(), "MAT owner stopped before capture startup");
      const auto tap = image.parent_path() / capture_tap_name;
      const auto worker = image.parent_path() / capture_worker_name;
      require(spatial_audio::mat_protected_entry(tap) && spatial_audio::mat_protected_entry(worker), "Protected installed MAT libraries");
      std::vector<std::wstring> args { image.wstring(), L"--reattach", std::to_wstring(engine.pid), std::to_wstring(engine.created),
        tap.wstring(), capture_tap_sha256, worker.wstring(), capture_worker_sha256, L"\\\\.\\pipe\\SunshineMatLive-active",
        L"Local\\SunshineMatLiveStop-capture-" + std::to_wstring(GetCurrentProcessId()), L"--continuous" };
      std::vector<wchar_t *> pointers;
      for (auto &arg : args) pointers.push_back(arg.data());
      owner.monitor(interrupted);
      return wmain(static_cast<int>(pointers.size()), pointers.data());
    }
    catch (const std::exception &e) {
      printf("AUTOMATIC_FAILED %s win32=%lu\n", e.what(), GetLastError());
      return 1;
    }
  }
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
    const auto dolby = sonic_live::find_dolby_access(automaticUserSid);
    require(sonic_live::report_components(mat_compat::audio_engine(), dolby.component), "Unsupported system component; see COMPONENT lines");
    engine = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
    require(engine && native_mat_guard::created(engine) == creation && native_mat_guard::system_engine(engine), "Pinned plugin audiodg identity/hash");
    require(OriginalPluginBytes(engine), "Original six plugin entry points");
    require(!interrupted, "MAT capture startup cancelled");
    fs::path tap = fs::canonical(argv[4]), codec = fs::canonical(argv[6]);
    std::wstring th = argv[5], ch = argv[7];
    const auto ascii = [](const std::wstring &value) {
      std::string result;
      result.reserve(value.size());
      for (const auto character : value) {
        if (character > 0x7F) return std::string {};
        result.push_back(static_cast<char>(character));
      }
      return result;
    };
    const auto tapHash = ascii(th), codecHash = ascii(ch);
    HANDLE tapLock = CreateFileW(tap.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr), codecLock = CreateFileW(codec.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    struct Unlock {
      HANDLE a, b;
      ~Unlock() {
        if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
        if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
      }
    } unlock { tapLock, codecLock };
    require(tapLock != INVALID_HANDLE_VALUE && codecLock != INVALID_HANDLE_VALUE, "Lock reviewed plugin DLLs");
    require(native_mat_guard::sha256(tap) == tapHash && native_mat_guard::sha256(codec) == codecHash, "Both plugin DLL hashes");
    std::wstring pipe = argv[8], event = argv[9];
    require(pipe.starts_with(L"\\\\.\\pipe\\SunshineMatLive-") && pipe.find_first_of(L"\r\n") == std::wstring::npos && event.starts_with(L"Local\\SunshineMatLiveStop-"), "Private plugin pipe/stop");
    auto acl = PipeAcl();
    SECURITY_ATTRIBUTES sa { sizeof(sa), acl, FALSE };
    stop = CreateEventW(&sa, TRUE, FALSE, event.c_str());
    auto code = GetLastError();
    LocalFree(acl);
    require(stop && code != ERROR_ALREADY_EXISTS, "Fresh plugin stop event");
    bool reattach = std::wstring(argv[1]) == L"--reattach";
    require(!interrupted, "MAT capture startup cancelled");
    auto tapBase = LoadReviewed(engine, pid, tap, tapHash, reattach);
    auto codecBase = LoadReviewed(engine, pid, codec, codecHash, reattach);
    require(!interrupted, "MAT capture startup cancelled");
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
