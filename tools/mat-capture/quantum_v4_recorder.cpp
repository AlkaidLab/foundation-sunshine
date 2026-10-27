#include "quantum_v4_recorder.h"
#include <cstring>
#include <climits>
namespace q4 {
  static LONG64
  read64(volatile LONG64 *value) { return InterlockedCompareExchange64(value, 0, 0); }
  static void
  maximum(volatile LONG64 *value, LONG64 candidate) {
    auto old = read64(value);
    if (candidate > old) InterlockedCompareExchange64(value, candidate, old);
  }
  static constexpr uint64_t owned = uint64_t { 1 } << 63;
  void
  Recorder::start(Ring *ring) {
    ring_ = ring;
    loss_ = 0;
    for (auto &binding : bindings_) binding = Binding {};
  }
  void
  Recorder::fail() {
    if (!ring_) return;
    InterlockedIncrement64(&loss_);
    InterlockedIncrement64(&ring_->invalid);
    InterlockedExchange(&ring_->stop, 1);
  }
  bool
  Recorder::healthy() { return ring_ && read64(&loss_) == 0; }
  Header
  Recorder::header(const Pass &pass, uint32_t kind) const {
    Header value {};
    value.kind = kind;
    value.renderer = pass.renderer;
    value.encoder = pass.encoder;
    value.implementation = pass.implementation;
    value.domain = pass.domain;
    value.pass = pass.pass;
    return value;
  }
  Recorder::Binding *
  Recorder::acquire(uint64_t encoder, bool create) {
    if (!encoder || (encoder & owned)) {
      fail();
      return nullptr;
    }
    for (auto &binding : bindings_) {
      const auto owner = static_cast<uint64_t>(read64(&binding.owner));
      if ((owner & ~owned) != encoder) continue;
      if ((owner & owned) || InterlockedCompareExchange64(&binding.owner, static_cast<LONG64>(encoder | owned), static_cast<LONG64>(encoder)) != static_cast<LONG64>(encoder)) {
        fail();
        return nullptr;  // Overlapping pass/lifecycle for this encoder.
      }
      return &binding;
    }
    if (!create) return nullptr;
    for (auto &binding : bindings_) {
      if (InterlockedCompareExchange64(&binding.owner, static_cast<LONG64>(encoder | owned), 0)) continue;
      // Two first observations of the SAME encoder can claim separate free
      // entries. The later claimant must see the earlier one and reject it.
      for (auto &other : bindings_)
        if (&other != &binding && (static_cast<uint64_t>(read64(&other.owner)) & ~owned) == encoder) {
          fail();
          InterlockedExchange64(&binding.owner, 0);
          return nullptr;
        }
      return &binding;
    }
    fail();
    return nullptr;  // Fixed active-domain capacity, never grow or wait.
  }
  bool
  Recorder::emit(Header &value, const float *pcm, bool allowStopped) {
    if (!ring_ || (ring_->stop && !allowStopped)) return false;
    const auto ticket = static_cast<uint64_t>(InterlockedIncrement64(&ring_->write)) - 1;
    const auto read = read64(&ring_->read);
    if (ticket >= LLONG_MAX || ticket - static_cast<uint64_t>(read) >= kSlots) {
      InterlockedIncrement64(&ring_->droppedFull);
      InterlockedIncrement64(&loss_);
      InterlockedExchange(&ring_->stop, 1);
      return false;
    }
    const auto write = static_cast<LONG64>(ticket);
    bool copied = false;
    LARGE_INTEGER begin {}, end {};
    QueryPerformanceCounter(&begin);
    __try {
      auto &slot = ring_->slots[write % kSlots];
      value.version = version;
      value.audio.magic = magic;
      value.audio.sequence = write;
      value.audio.qpc = begin.QuadPart;
      value.audio.tid = GetCurrentThreadId();
      value.lossEpoch = read64(&loss_);
      if (value.kind == DomainBegin) value.domain = static_cast<uint64_t>(write) + 1;
      slot.header = value;
      if (value.audio.frames) {
        if (pcm)
          memcpy(slot.pcm, pcm, value.audio.frames * sizeof(float));
        else
          memset(slot.pcm, 0, value.audio.frames * sizeof(float));
      }
      copied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      fail();
    }
    if (copied) {
      InterlockedAdd64(&ring_->frames, value.audio.frames);
      maximum(&ring_->highWater, write - read + 1);
      InterlockedExchange64(&ring_->slots[write % kSlots].ready, write + 1);
    }
    QueryPerformanceCounter(&end);
    InterlockedAdd64(&ring_->copyTicks, end.QuadPart - begin.QuadPart);
    maximum(&ring_->maxCopyTicks, end.QuadPart - begin.QuadPart);
    return copied;
  }
  bool
  Recorder::close(Binding &binding, uint32_t reason, bool allowStopped) {
    Pass pass {};
    pass.renderer = binding.renderer;
    pass.encoder = binding.encoder;
    pass.implementation = binding.implementation;
    pass.domain = binding.domain;
    auto value = header(pass, DomainEnd);
    value.reason = reason;
    value.flags = reason == CaptureStop ? 0 : PreCall;
    bool okay = emit(value, nullptr, allowStopped);
    binding.used = false;
    if (okay) InterlockedIncrement64(&ring_->domainsClosed);
    return okay;
  }
  bool
  Recorder::begin(uint64_t renderer, uint64_t encoder, uint64_t implementation, uint32_t frames, Pass &pass) {
    pass = Pass {};
    if (!ring_ || ring_->stop) return false;
    if (!renderer || !encoder || !implementation || !frames || frames > kSamples) {
      fail();
      return false;
    }
    Binding *binding = acquire(encoder, true);
    if (!binding) return false;
    bool okay = true;
    if (binding->used && (binding->renderer != renderer || binding->implementation != implementation)) {
      okay = close(*binding, BindingChanged);
    }
    if (okay && !binding->used) {
      binding->renderer = renderer;
      binding->encoder = encoder;
      binding->implementation = implementation;
      binding->nextPass = 0;
      Pass fresh {};
      fresh.renderer = renderer;
      fresh.encoder = encoder;
      fresh.implementation = implementation;
      auto value = header(fresh, DomainBegin);
      value.reason = Discovery;
      value.flags = ObservedNotCreated;
      okay = emit(value);
      if (okay) {
        binding->domain = value.domain;
        binding->used = true;
        InterlockedIncrement64(&ring_->domainsOpened);
      }
    }
    if (okay) {
      if (binding->nextPass == UINT64_MAX) {
        fail();
        okay = false;
      }
      else {
        pass.renderer = renderer;
        pass.encoder = encoder;
        pass.implementation = implementation;
        pass.domain = binding->domain;
        pass.pass = ++binding->nextPass;
        pass.frames = frames;
        pass.valid = true;
        pass.binding = static_cast<uint32_t>(binding - bindings_);
        auto value = header(pass, PassBegin);
        value.inputFrames = frames;
        value.flags = PreCall;
        okay = emit(value);
        if (!okay) pass.valid = false;
      }
    }
    if (!okay) InterlockedExchange64(&binding->owner, static_cast<LONG64>(encoder));
    return okay;
  }
  bool
  Recorder::before(Pass &pass, const void *info, const void *properties, const float *pcm, uint32_t frames, float gain) {
    if (!pass.valid || !healthy()) return false;
    InterlockedIncrement64(&ring_->calls);
    if (pass.insideSubmit || pass.sawProcess || !frames || frames > kSamples || frames > pass.frames || pass.call >= objectCapacity) {
      fail();
      return false;
    }
    pass.insideSubmit = true;
    Header value = header(pass, ObjectBefore);
    value.call = ++pass.call;
    value.flags = PreCall | (pcm ? 0 : NullPcm);
    value.audio.frames = frames;
    value.audio.callGain = gain;
    value.result = E_PENDING;
    bool readable = false;
    __try {
      const auto *bytes = static_cast<const BYTE *>(properties);
      memcpy(&value.audio.host, static_cast<const BYTE *>(info) + 4, 8);
      memcpy(&value.audio.objectId, bytes, 4);
      memcpy(&value.audio.type, bytes + 8, 4);
      memcpy(&value.properties, bytes + 12, 4);
      memcpy(&value.audio.x, bytes + 16, 16);
      readable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      fail();
    }
    if (!readable) return false;
    if (!pcm) InterlockedIncrement64(&ring_->silent);
    return emit(value, pcm, true);  // An admitted pass may drain across a normal stop.
  }
  void
  Recorder::returned(Pass &pass, HRESULT result, bool snapshotPublished) {
    if (snapshotPublished && pass.valid && healthy()) {
      if (!pass.insideSubmit) {
        fail();
        return;
      }
      auto value = header(pass, SubmitResult);
      value.call = pass.call;
      value.result = result;
      value.flags = PostCall;
      if (emit(value, nullptr, true)) InterlockedIncrement64(&ring_->submitResults);
      if (result != S_OK) fail();
    }
    pass.insideSubmit = false;
  }
  void
  Recorder::processed(Pass &pass, uint32_t frames, uint32_t capacity, const uint32_t *produced, HRESULT result) {
    if (!pass.valid || !healthy()) return;
    if (pass.insideSubmit || pass.sawProcess || frames != pass.frames) {
      fail();
      return;
    }
    pass.sawProcess = true;
    auto value = header(pass, ProcessResult);
    value.inputFrames = frames;
    value.capacity = capacity;
    value.result = result;
    value.flags = PostCall;
    bool readable = false;
    __try {
      value.producedBytes = *produced;
      readable = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
      fail();
    }
    if (!readable) return;
    if (value.producedBytes > capacity) {
      fail();
      return;
    }
    if (emit(value, nullptr, true)) InterlockedIncrement64(&ring_->processResults);
    if (result != S_OK) fail();
  }
  void
  Recorder::end(Pass &pass) {
    if (!pass.valid) return;
    if (healthy() && (!pass.sawProcess || pass.insideSubmit)) fail();
    if (healthy()) {
      auto value = header(pass, PassEnd);
      value.flags = PostCall;
      emit(value, nullptr, true);
    }
    if (pass.binding >= domainCapacity)
      fail();
    else
      InterlockedExchange64(&bindings_[pass.binding].owner, static_cast<LONG64>(pass.encoder));
    pass.valid = false;
  }
  void
  Recorder::retire(uint64_t encoder, uint32_t reason) {
    if (!ring_ || ring_->stop) return;
    if (reason != Reset && reason != Destroy) {
      fail();
      return;
    }
    if (auto *binding = acquire(encoder, false)) {
      close(*binding, reason);
      InterlockedExchange64(&binding->owner, 0);
    }
  }
  void
  Recorder::unbound() {
    if (!ring_ || ring_->stop) return;
    if (read64(&ring_->renderCalls) == 0)
      InterlockedIncrement64(&ring_->ignoredBeforeFirstRender);
    else {
      InterlockedIncrement64(&ring_->unboundAfterArming);
      fail();
    }
  }
  void
  Recorder::finish() {
    if (!ring_) return;
    for (auto &binding : bindings_)
      if (binding.used) {
        if (static_cast<uint64_t>(read64(&binding.owner)) & owned) {
          fail();
          continue;
        }
        close(binding, CaptureStop, true);
        InterlockedExchange64(&binding.owner, 0);
      }
  }
}  // namespace q4
