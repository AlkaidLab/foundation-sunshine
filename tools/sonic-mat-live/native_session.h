#pragma once
// Shared native object -> dedicated MAT worker -> private live pipe.
// No recorded input, source synthesis, default endpoint change or elevation.
#define NOMINMAX
#include "bridge.h"
#include "native_queue.h"
#include "../../src/spatial_audio/stream_ring_source.h"
#include "../mat-live/publisher.h"
#include <memory>
static uintptr_t
LoadReviewed(HANDLE engine, DWORD pid, const fs::path &path, const std::string &hash, bool reattach) {
  require(path.is_absolute() && hash.size() == 64 && native_mat_guard::sha256(path) == hash, "Reviewed native DLL hash");
  auto prior = LoadedTap(pid, path);
  require(reattach || !prior, "Native DLL already loaded; explicit reattach required");
  if (prior) return prior;
  auto loaded = LoadTap(engine, pid, path);
  require(LoadedTap(pid, path) == loaded, "Native loaded DLL path");
  return loaded;
}
static uintptr_t
Export(const fs::path &path, uintptr_t loaded, const char *name) {
  auto local = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  require(local, "Read native exports");
  auto entry = GetProcAddress(local, name);
  auto address = entry ? loaded + (uintptr_t) entry - (uintptr_t) local : 0;
  FreeLibrary(local);
  require(address != 0, "Native export");
  return address;
}
static HANDLE
DuplicateTo(HANDLE engine, HANDLE source, DWORD access = 0) {
  HANDLE remote {};
  require(DuplicateHandle(GetCurrentProcess(), source, engine, &remote, access, FALSE, access ? 0 : DUPLICATE_SAME_ACCESS) != FALSE, "Duplicate native session handle");
  return remote;
}
static void
CloseRemote(HANDLE engine, HANDLE remote) {
  if (!remote) return;
  HANDLE copy {};
  if (DuplicateHandle(engine, remote, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) CloseHandle(copy);
}
static HANDLE
Map(size_t size) {
  auto h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) size, nullptr);
  require(h, "Create bounded native session mapping");
  return h;
}

class NativeSession {
  HANDLE engine_, input_map_ {}, output_map_ {}, data_ {}, cancel_ {}, thread_ {};
  receiver::Shared *input_ {};
  spatial_stream::Output *output_ {};
  void *arguments_ {};
  std::vector<HANDLE> remote_;
  std::thread consumer_;
  Shared &pipe_;
  uint64_t domain_, epoch_;
  const char *sourceKind_;
  bool finished_ = false, reported_ = false;
  void
  report() {
    if (!output_ || reported_) return;
    reported_ = true;
    const auto &s = output_->header;
    printf("NATIVE_WORKER sourceRounds=%llu sourceSamples=%llu processes=%llu outputBytes=%llu warm=%llu drain=%llu status=%ld error=%ld vendorLoads=%llu activations=%llu initialize=%llu createHR=0x%08x mapperHR=0x%08x initializeHR=0x%08x submitHR=0x%08x processHR=0x%08x\n", s.sourceRounds, s.sourceSamples, s.processes, s.outputBytes, s.warmRounds, s.drainRounds, s.status, s.error, s.vendorLoads, s.activations, s.initializations, (unsigned) s.createHr, (unsigned) s.mapperHr, (unsigned) s.initializeHr, (unsigned) s.submitHr, (unsigned) s.processHr);
  }
  void
  consume() {
    try {
      spatial_audio::StreamRingSource source(*output_, epoch_, domain_);
      require(source.start(), "Native MAT output contract");
      auto progress = GetTickCount64();
      spatial_audio::Frame frame;
      for (;;) {
        auto status = source.read(frame);
        if (status == spatial_audio::Read::pending) {
          require(GetTickCount64() - progress < 2000, "Native MAT worker output stalled");
          Sleep(1);
          continue;
        }
        require(status != spatial_audio::Read::failed, source.error().c_str());
        if (status == spatial_audio::Read::ended) {
          require(WaitForSingleObject(thread_, 2000) == WAIT_OBJECT_0, "Native MAT final status timeout");
          require(output_->header.eofSeen && output_->header.workerFinished && !output_->header.error && output_->header.status == 2, "Native worker did not finish cleanly");
          std::lock_guard lock(pipe_.mutex);
          pipe_.ended = true;
          pipe_.changed.notify_all();
          break;
        }
        auto qpc = source.capture_qpc();
        require(qpc > 0, "Native MAT completion clock");
        progress = GetTickCount64();
        std::lock_guard lock(pipe_.mutex);
        bool first = !pipe_.ready;
        pipe_.ready = true;
        pipe_.encoder = domain_;
        pipe_.last_cycle = progress;
        ++pipe_.cycles;
        if (first) printf("NATIVE_MAT_READY source=%s carrierBytes=61440 inputRenderer=0x%llx fileInput=0\n", sourceKind_, domain_);
        if (pipe_.playing && qpc >= pipe_.play_qpc) {
          require(pipe_.queue.size() < 4, "Live consumer overflow; no drop/replay");
          Cycle cycle {};
          memcpy(cycle.bytes.data(), frame.bytes.data(), frame.bytes.size());
          cycle.qpc = qpc;
          pipe_.queue.push_back(std::move(cycle));
          pipe_.changed.notify_all();
        }
      }
    }
    catch (const std::exception &e) {
      Broken(pipe_, e.what());
      if (input_) receiver::fail(*input_, receiver::codec);
      if (cancel_) SetEvent(cancel_);
    }
  }
  void
  cleanup() {
    if (input_ && !finished_) receiver::fail(*input_, receiver::cancelled);
    if (cancel_ && !finished_) SetEvent(cancel_);
    if (consumer_.joinable()) consumer_.join();
    bool done = !thread_ || WaitForSingleObject(thread_, 10000) == WAIT_OBJECT_0;
    if (done)
      report();
    else
      puts("NATIVE_WORKER blocked-call; retained remote state; no forced unload");
    // No forced unload or thread termination under a blocked vendor call.
    if (done) {
      for (auto h : remote_) CloseRemote(engine_, h);
      remote_.clear();
      if (arguments_) VirtualFreeEx(engine_, arguments_, 0, MEM_RELEASE);
    }
    if (thread_) CloseHandle(thread_);
    if (input_) UnmapViewOfFile(input_);
    if (output_) UnmapViewOfFile(output_);
    for (auto h : { input_map_, output_map_, data_, cancel_ })
      if (h) CloseHandle(h);
  }

public:
  NativeSession(HANDLE engine, const fs::path &codec, uintptr_t loaded, const std::wstring &vendorDirectory, uint64_t domain, uint64_t epoch, Shared &pipe, const char *sourceKind): engine_(engine), pipe_(pipe), domain_(domain), epoch_(epoch), sourceKind_(sourceKind) {
    try {
      input_map_ = Map(sizeof(receiver::Shared));
      output_map_ = Map(sizeof(spatial_stream::Output));
      input_ = (receiver::Shared *) MapViewOfFile(input_map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(receiver::Shared));
      output_ = (spatial_stream::Output *) MapViewOfFile(output_map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(spatial_stream::Output));
      require(input_ && output_, "Map native queues");
      uint64_t nonce[2];
      require(BCryptGenRandom(nullptr, (PUCHAR) nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Native session nonce");
      sonic_live::initialize_native_queue(*input_, *output_, domain, epoch, nonce);
      data_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      require(data_ && cancel_, "Native queue events");
      auto dup = [&](HANDLE h, DWORD access = 0) {auto copy=DuplicateTo(engine_,h,access);remote_.push_back(copy);return copy; };
      spatial_stream::Args args { sizeof(args), spatial_stream::streamVersion, dup(input_map_), dup(data_), dup(cancel_), dup(GetCurrentProcess(), SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION), dup(output_map_), domain, epoch, { nonce[0], nonce[1] }, native_mat_guard::created(GetCurrentProcess()), GetCurrentProcessId(), 0, 0, 0 };
      require(vendorDirectory.size() < std::size(args.vendorDirectory), "Dolby Access directory length");
      wmemcpy(args.vendorDirectory, vendorDirectory.c_str(), vendorDirectory.size() + 1);
      arguments_ = RemoteCopy(engine_, &args, sizeof(args));
      thread_ = RemoteThread(engine_, Export(codec, loaded, "StartMatStreamReceiver"), arguments_);
      consumer_ = std::thread([this] { consume(); });
    }
    catch (...) {
      cleanup();
      throw;
    }
  }
  ~NativeSession() { cleanup(); }
  bool
  send(const receiver::Slot &packet) {
    bool accepted = sonic_live::enqueue_native(*input_, packet, data_);
    if (!accepted) fprintf(stderr, "NATIVE_INPUT_REFUSED fault=%ld cancel=%ld done=%ld queued=%lld capacity=32 workerStatus=%ld workerError=%ld\n",
      receiver::get(input_->common.fault), receiver::get(input_->common.cancel), receiver::get(input_->common.producerDone),
      receiver::get(input_->writer.write) - receiver::get(input_->reader.read), receiver::get(output_->header.status), receiver::get(output_->header.error));
    return accepted;
  }
  void
  heartbeat() { receiver::put(input_->writer.heartbeat, receiver::qpc()); }
  void
  finish() {
    auto until = GetTickCount64() + 2000;
    while (WaitForSingleObject(thread_, 10) == WAIT_TIMEOUT && GetTickCount64() < until) heartbeat();
    require(WaitForSingleObject(thread_, 0) == WAIT_OBJECT_0, "Native MAT drain timeout");
    if (consumer_.joinable()) consumer_.join();
    const auto &s = output_->header;
    report();
    require(s.status == 2 && !s.error && s.workerFinished && !s.workerTimedOut && s.eofSeen && s.vendorLoads == 1 && s.activations == 1 && s.initializations == 1 && s.hostGuardPassed && s.createHr == S_OK && s.mapperHr == S_OK && s.initializeHr == S_OK && s.submitHr == S_OK && s.processHr == S_OK, "Native MAT worker result");
    finished_ = true;
  }
};
