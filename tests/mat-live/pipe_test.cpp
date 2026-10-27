// Own-process byte-pipe test. Generated carrier only; no audiodg or vendor use.
// The test-source descriptor is intentionally rejected by native preparation.
#define NOMINMAX
#include <windows.h>
static void *lastAllocation;
static void *
ObservedAllocation(HANDLE process, void *address, SIZE_T bytes, DWORD type, DWORD protection) {
  return lastAllocation = VirtualAllocEx(process, address, bytes, type, protection);
}
static HANDLE
DeniedThread(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, void *, DWORD, LPDWORD) {
  SetLastError(ERROR_ACCESS_DENIED);
  return nullptr;
}
#define VirtualAllocEx ObservedAllocation
#define CreateRemoteThread DeniedThread
#include "../../tools/mat-live/publisher.h"
#undef CreateRemoteThread
#undef VirtualAllocEx
#include "../../src/spatial_audio/mat_capture_reader.h"
#include "../../src/spatial_audio/native_mat_pipe_source.h"
#include <atomic>
#include <iostream>
static void
CheckPipe(bool okay, const char *why) {
  if (!okay) throw std::runtime_error(why);
}
static void
StartupFailureCleanup() {
  // Real own-process allocation; the OS denies writes on the first handle.
  // The second case injects thread-creation failure before any entry can run.
  for (bool copyFailure : { true, false }) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | (copyFailure ? 0 : PROCESS_VM_WRITE), FALSE, GetCurrentProcessId());
    CheckPipe(process != nullptr, "Open restricted own-process handle");
    lastAllocation = nullptr;
    bool rejected = false;
    try {
      if (copyFailure) {
        const int value = 42;
        RemoteCopy(process, &value, sizeof(value));
      }
      else
        LoadTap(process, GetCurrentProcessId(), L"mat-cleanup-test-not-loaded.dll");
    }
    catch (const std::runtime_error &error) {
      rejected = std::string(error.what()) == (copyFailure ? "WriteProcessMemory" : "CreateRemoteThread");
    }
    MEMORY_BASIC_INFORMATION memory {};
    const bool released = lastAllocation && VirtualQueryEx(process, lastAllocation, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_FREE;
    CloseHandle(process);
    CheckPipe(rejected && released, "Startup failure retained remote argument memory");
  }
}
static void
Run(unsigned mode) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-eos-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::string serverError;
  std::atomic<bool> finished = false;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {try{Serve(shared,name,descriptor);}catch(const std::exception& e){serverError=e.what();}finished=true; });
  std::exception_ptr failure;
  try {
    auto until = GetTickCount64() + 2000;
    while (client == INVALID_HANDLE_VALUE && GetTickCount64() < until) {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client == INVALID_HANDLE_VALUE) Sleep(1);
    }
    CheckPipe(client != INVALID_HANDLE_VALUE, "Connect own-process pipe");
    char command[8];
    memcpy(command, "MCAPOPEN", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "OPEN");
    uint8_t header[mat_capture::descriptor_bytes];
    CheckPipe(Io(client, header, sizeof(header), false, 2000), "Descriptor");
    mat_capture::Descriptor decoded;
    CheckPipe(!mat_capture::decode_descriptor(header, decoded), "Generated fixture must stay forbidden as native input");
    memcpy(command, "MCAPPLAY", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "PLAY");
    until = GetTickCount64() + 2000;
    for (;;) {
      bool playing = false;
      {
        std::lock_guard lock(shared.mutex);
        playing = shared.playing;
      }
      if (playing) break;
      CheckPipe(GetTickCount64() < until, "PLAY admission");
      Sleep(1);
    }
    Cycle cycle {};
    memcpy(cycle.bytes.data(), "\x72\xf8\x1f\x4e\x16\x00\x02\x00", 8);
    cycle.qpc = Qpc();
    {
      std::lock_guard lock(shared.mutex);
      shared.queue.push_back(cycle);
      shared.last_cycle = GetTickCount64();
      shared.ended = true;
      shared.changed.notify_all();
    }
    // Make the producer finish writing before consuming the tail. A write
    // completing into a pipe buffer does not mean the client consumed it.
    Sleep(100);
    uint64_t next = 0, last = 0;
    spatial_audio::Frame frame;
    bool ackAttempted = false, ended = false;
    auto io = [&](void *data, size_t n, bool writing) {
      if (writing) {
        ackAttempted = true;
        if (mode == 2) throw std::runtime_error("Simulated acknowledgement cancellation");
        if (mode == 1) mat_put64(static_cast<uint8_t *>(data) + 8, 999);
      }
      CheckPipe(Io(client, data, (DWORD) n, writing, 2000), "Tail pipe I/O");
    };
    CheckPipe(mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::frame && next == 1 && frame.epoch == 7 && frame.domain == 42 && std::equal(frame.bytes.begin(), frame.bytes.end(), cycle.bytes.begin()), "Production reader final cycle");
    try {
      ended = mat_capture::read_record(io, next, last, 7, 42, frame) == spatial_audio::Read::ended;
    }
    catch (...) {
      if (mode != 2) throw;
    }
    CheckPipe(ackAttempted && (ended == (mode != 2)), "Unacknowledged EOS must not become clean end");
    if (mode == 2) {
      auto began = GetTickCount64();
      while (!finished && GetTickCount64() - began < 3000) Sleep(1);
      CheckPipe(finished, "Missing acknowledgement must be bounded");
    }
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    server.join();
    CheckPipe(serverError.empty(), "Server exception");
    CheckPipe(shared.acknowledged_leases == (mode == 0 ? 1u : 0u), "Wrong/missing end acknowledgement accepted");
    CheckPipe(mode == 0 ? shared.delivered_sha256 == "b51894f456731ba3171663abe5305090f366ee778eb56cece1009e90005f4898" : shared.delivered_sha256.empty(), "Producer stream SHA oracle / failed lease publication");
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (server.joinable()) {
    Broken(shared, "Test cleanup");
    server.join();
  }
  if (failure) std::rethrow_exception(failure);
}
static void
Reconnect(bool sourceFailure) {
  Shared shared;
  shared.ready = true;
  shared.encoder = 42;
  shared.last_cycle = GetTickCount64();
  const auto name = L"\\\\.\\pipe\\SunshineMatLive-reconnect-" + std::to_wstring(GetCurrentProcessId()) + (sourceFailure ? L"-fault" : L"-resume");
  mat_capture::Descriptor descriptor { mat_capture::test_source, GetCurrentProcessId(), GetCurrentProcessId(), 1, 1, 7, 42, 10000000 };
  std::exception_ptr failure;
  std::string serverError;
  HANDLE client = INVALID_HANDLE_VALUE;
  std::thread server([&] {try{Serve(shared,name,descriptor);}catch(const std::exception& e){serverError=e.what();} });
  const auto open = [&] {
    auto until = GetTickCount64() + 2000;
    do {
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (client != INVALID_HANDLE_VALUE) break;
      Sleep(1);
    } while (GetTickCount64() < until);
    CheckPipe(client != INVALID_HANDLE_VALUE, "Reconnect exclusive lease");
    char command[8];
    memcpy(command, "MCAPOPEN", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "Reconnect OPEN");
    uint8_t header[mat_capture::descriptor_bytes];
    CheckPipe(Io(client, header, sizeof(header), false, 2000), "Reconnect descriptor");
    mat_capture::Descriptor decoded;
    CheckPipe(!mat_capture::decode_descriptor(header, decoded), "Reconnect fixture must not authenticate as native");
  };
  const auto play = [&] {
    char command[8];
    memcpy(command, "MCAPPLAY", 8);
    CheckPipe(Io(client, command, 8, true, 2000), "Reconnect PLAY");
    auto until = GetTickCount64() + 2000;
    for (;;) {
      {
        std::lock_guard lock(shared.mutex);
        if (shared.playing) break;
      }
      CheckPipe(GetTickCount64() < until, "Reconnect PLAY admission");
      Sleep(1);
    }
  };
  const auto cycle = [](uint8_t tag) {Cycle c{};memcpy(c.bytes.data(),"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);c.bytes[8]=tag;c.qpc=Qpc();return c; };
  const auto enqueue = [&](const Cycle &c, bool end) {std::lock_guard lock(shared.mutex);shared.queue.push_back(c);shared.last_cycle=GetTickCount64();shared.ended=end;shared.changed.notify_all(); };
  const auto read = [&](uint64_t &next, uint64_t &last, spatial_audio::Frame &frame) { return mat_capture::read_record([&](void *p, size_t n, bool writing) { CheckPipe(Io(client, p, (DWORD) n, writing, 2000), "Reconnect record I/O"); }, next, last, 7, 42, frame); };
  try {
    // DESCRIBE/prepare cancellation must release the single lease before PLAY.
    open();
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    open();
    play();
    if (sourceFailure) {
      Broken(shared, "Controlled source failure, no clean EOS");
      uint64_t next = 0, last = 0;
      spatial_audio::Frame frame;
      bool rejected = false;
      try {
        read(next, last, frame);
      }
      catch (...) {
        rejected = true;
      }
      CheckPipe(rejected, "Source failure fabricated EOS");
    }
    else {
      enqueue(cycle(0x11), false);
      uint64_t next = 0, last = 0;
      spatial_audio::Frame frame;
      CheckPipe(read(next, last, frame) == spatial_audio::Read::frame && next == 1 && frame.bytes[8] == 0x11, "First lease data");
      CloseHandle(client);
      client = INVALID_HANDLE_VALUE;
      // A subsequent real cycle makes the writer observe the disconnect.
      enqueue(cycle(0x22), false);
      open();
      {
        std::lock_guard lock(shared.mutex);
        CheckPipe(!shared.playing && !shared.failed && shared.queue.empty(), "Disconnected lease retained data/admission");
        shared.queue.push_back(cycle(0x33));
      }
      play();
      {
        std::lock_guard lock(shared.mutex);
        CheckPipe(shared.queue.empty(), "New PLAY retained pre-PLAY data");
      }
      enqueue(cycle(0x44), true);
      next = 0;
      last = 0;
      CheckPipe(read(next, last, frame) == spatial_audio::Read::frame && next == 1 && frame.bytes[8] == 0x44, "Reconnect sequence/cutoff mixed old cycle");
      CheckPipe(read(next, last, frame) == spatial_audio::Read::ended, "Reconnect clean EOS");
    }
    CloseHandle(client);
    client = INVALID_HANDLE_VALUE;
    server.join();
    CheckPipe(serverError.empty() && shared.acknowledged_leases == (sourceFailure ? 0u : 1u), "Reconnect lease/EOS accounting");
    CheckPipe(!shared.playing && shared.queue.empty(), "Reconnect final lease release");
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (server.joinable()) {
    Broken(shared, "Reconnect test cleanup");
    server.join();
  }
  if (failure) std::rethrow_exception(failure);
}
static void
RejectNativePeer(unsigned mode) {
  // A mock publisher may exercise rejection/cleanup, but can never pass the
  // production audiodg identity check or supply a native stream to the host.
  const auto name = mode == 3 ? fs::path(spatial_audio::mat_auto_pipe).wstring() : L"\\\\.\\pipe\\SunshineMatLive-reject-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mode);
  HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  CheckPipe(pipe != INVALID_HANDLE_VALUE, "Mock publisher pipe");
  std::string peerError;
  std::atomic<bool> disconnected = false;
  std::thread peer([&] {try{
        OVERLAPPED connection{};connection.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);CheckPipe(connection.hEvent!=nullptr,"Mock connect event");
        BOOL okay=ConnectNamedPipe(pipe,&connection);DWORD code=okay?0:GetLastError();
        if(code==ERROR_PIPE_CONNECTED)okay=TRUE;
        if(!okay&&code==ERROR_IO_PENDING){
            DWORD n=0;auto waited=WaitForSingleObject(connection.hEvent,2000);
            if(waited!=WAIT_OBJECT_0){CancelIoEx(pipe,&connection);GetOverlappedResult(pipe,&connection,&n,TRUE);CloseHandle(connection.hEvent);throw std::runtime_error("Mock connect timeout");}
            okay=GetOverlappedResult(pipe,&connection,&n,FALSE);
        }
        const auto connectError=okay?0:GetLastError();CloseHandle(connection.hEvent);
        if(mode==3&&!okay&&(connectError==ERROR_NO_DATA||connectError==ERROR_PIPE_NOT_CONNECTED||connectError==ERROR_BROKEN_PIPE)){
            disconnected=true;return; // Rejection can finish before the server observes connect.
        }
        CheckPipe(okay,"Mock source connected");
        if(mode==3){
            char next;auto began=GetTickCount64();disconnected=!Io(pipe,&next,1,false,1000)&&GetTickCount64()-began<1000;
            return; // Automatic discovery must reject us before sending OPEN.
        }
        char open[8];CheckPipe(Io(pipe,open,8,false,2000)&&!memcmp(open,"MCAPOPEN",8),"Mock OPEN");
        mat_capture::Descriptor d{mode==0?mat_capture::test_source:mat_capture::native_object_source,GetCurrentProcessId(),GetCurrentProcessId(),1,1,7,42,10000000};
        if(mode){const auto created=native_mat_guard::created(GetCurrentProcess());d.publisher_created=d.engine_created=created;}
        if(mode==2)++d.publisher_pid;
        uint8_t bytes[mat_capture::descriptor_bytes];mat_capture::encode_descriptor(bytes,d);CheckPipe(Io(pipe,bytes,sizeof(bytes),true,2000),"Mock descriptor");
        // prepare() must close the rejected lease while its source object is
        // still alive, instead of retaining it until launch-ticket destruction.
        char next;auto began=GetTickCount64();disconnected=!Io(pipe,&next,1,false,1000)&&GetTickCount64()-began<1000;
    }catch(const std::exception& e){peerError=e.what();} });
  spatial_audio::NativeMatPipeSource source;
  bool rejected = false;
  try {
    source.prepare(fs::path(name).string(), mode == 3 ? "" : fs::path(native_mat_guard::image(GetCurrentProcess())).string());
  }
  catch (...) {
    rejected = true;
  }
  peer.join();
  CloseHandle(pipe);
  if (!(rejected && peerError.empty() && disconnected)) std::cerr << "REJECT_MODE " << mode << " source=" << source.error() << " peer=" << peerError << " closed=" << disconnected << '\n';
  CheckPipe(rejected && peerError.empty() && disconnected, "Rejected native preparation retained the lease");
  CheckPipe(source.state() == spatial_audio::State::fault && !source.error().empty() && !source.start(), "Rejected native source state/diagnostic");
  const auto error = source.error();
  source.stop();
  source.stop();
  CheckPipe(source.state() == spatial_audio::State::fault && source.error() == error, "Stop erased source failure");
  const auto observed = spatial_audio::mat_live_status.snapshot();
  CheckPipe(observed.leases == 0 && observed.running == 0 && observed.last_error == error && std::string(observed.state()) == "error", "Rejected source status retained a lease/lost its error");
}
int
main() {
  try {
    StartupFailureCleanup();
    const fs::path programs = L"C:\\Program Files";
    const std::string hash(64, 'a');
    const auto installed = programs / L"SunshineSonicMatLive" / hash / L"sunshine-plugin-mat-capture.exe";
    CheckPipe(spatial_audio::mat_auto_image(installed, programs, hash), "Automatic installed capture identity");
    CheckPipe(!spatial_audio::mat_auto_image(installed, programs, std::string(64, 'b')) &&
                !spatial_audio::mat_auto_image(installed.parent_path() / L"other.exe", programs, hash) &&
                !spatial_audio::mat_auto_image(installed, programs / L"other", hash) &&
                !spatial_audio::trusted_mat_auto_publisher(GetCurrentProcess()),
      "Automatic source accepted wrong executable/root/hash/process");
    spatial_audio::MatLiveStatus status;
    CheckPipe(std::string(status.snapshot().state()) == "unobserved", "Configuration is not readiness");
    status.acquired();
    status.started();
    status.failed("Competing lease failed");
    CheckPipe(std::string(status.snapshot().state()) == "streaming", "Failed contender hid active stream");
    std::thread observer([&] {for(unsigned i=0;i<10000;++i){const auto s=status.snapshot();CheckPipe(s.leases==1&&s.running==1,"Status observation changed ownership");} });
    for (unsigned i = 0; i < 10000; ++i) status.failed("Competing lease failed");
    observer.join();
    status.released(true);
    CheckPipe(std::string(status.snapshot().state()) == "error", "Release lost latest failure");
    status.acquired();
    CheckPipe(std::string(status.snapshot().state()) == "ready" && status.snapshot().last_error.empty(), "Reconnect did not clear stale error");
    status.released(false);
    CheckPipe(std::string(status.snapshot().state()) == "idle", "Release falsely advertises readiness");
    for (unsigned mode = 0; mode < 3; ++mode) Run(mode);
    Reconnect(false);
    Reconnect(true);
    for (unsigned mode = 0; mode < 4; ++mode) RejectNativePeer(mode);
    for (unsigned mode = 0; mode < 3; ++mode) {
      uint8_t record[mat_capture::record_bytes];
      uint64_t next = mode == 0 ? 0 : 1, last = 0;
      spatial_audio::Frame frame;
      mat_capture::encode_record(record, mode == 2 ? mat_capture::error_kind : mat_capture::end_kind, next, 1, nullptr, 0);
      if (mode == 1) record[28] ^= 1;
      bool wrote = false, failed = false;
      try {
        mat_capture::read_record([&](void *p, size_t n, bool writing) {if(writing)wrote=true;else {CheckPipe(n==sizeof(record),"Control read size");memcpy(p,record,n);} }, next, last, 7, 42, frame);
      }
      catch (...) {
        failed = true;
      }
      CheckPipe(failed && !wrote, "Invalid/empty/error EOF acknowledged");
    }
    std::cout << "PASS production pipe/reader delayed tail, exact EOS acknowledgement, wrong/missing/cancelled acknowledgement, prepare cancellation, streaming disconnect/reconnect, fresh PLAY cutoff/sequence, source failure without EOS, native peer rejection/release and invalid/empty/error EOF; nativeCapture=0\n";
    return 0;
  }
  catch (const std::exception &e) {
    std::cerr << "FAIL_PIPE_TAIL " << e.what() << " nativeCapture=0\n";
    return 1;
  }
}
