#pragma once
#include "bridge.h"
#include "../mat-capture/quantum_v4_assembler.h"
#include <cstdio>
namespace sonic_live {
  // Worker-only adapter of paired pre-submit observations. The q4 assembler owns
  // lifecycle/property reconstruction; no missing position/gain is fabricated.
  class PluginBridge {
    Bridge bridge_;
    uint64_t domain_ = 0, sequence_ = 0, outputOnlyPasses_ = 0;

  public:
    uint64_t
    renderer() const { return bridge_.renderer(); }
    uint64_t
    passes() const { return bridge_.sourcePasses; }
    uint64_t
    samples() const { return bridge_.inputSamples; }
    uint64_t
    outputOnlyPasses() const { return outputOnlyPasses_; }
    template <class Emit>
    void
    push(const q4::Quantum &quantum, int64_t first, int64_t last, uint32_t rate, Emit emit) {
      check(quantum.fullSpanLayoutReady && quantum.frames == 480 && rate == 48000, "Plugin source layout/rate unsupported");
      if (!domain_ && !quantum.objects.empty()) domain_ = quantum.domain;
      if (!domain_) return;
      check(domain_ == quantum.domain, "Plugin renderer domain changed; end epoch");
      check(first > 0 && last >= first, "Plugin source pass clock invalid");
      // The headphone encoder can drain buffered output without any Submit.
      // Its wrapper's Process ignores the input-frame argument. A paired,
      // successful output-only Process is not another 480 samples of object
      // input for the independent MAT encoder. Keep source IDs/sequence and
      // PCM untouched; zero-output gaps and partial object loss still fail.
      if (quantum.objects.empty() && quantum.producedBytes) {
        ++outputOnlyPasses_;
        return;
      }
      auto pass = std::make_unique<Pass>();
      pass->sequence = sequence_++;
      pass->renderer = quantum.renderer;
      pass->firstQpc = first;
      pass->lastQpc = last;
      pass->rate = rate;
      pass->deviceFrames = pass->requestedFrames = 480;
      check(quantum.objects.size() <= objects, "Plugin object capacity");
      for (const auto &object : quantum.objects) {
        const auto &raw = object.state.audio;
        check(object.resultSeen && object.offset == 0 && object.pcm.size() == 480 && object.state.volumeKnown && receiver::objectType(raw.type) &&
                (raw.type == 1 ? (object.state.positionState == position_state::Updated || object.state.positionState == position_state::Cached || object.state.positionState == position_state::DefaultOrigin) :
                                 object.state.positionState == position_state::StaticChannel),
          "Plugin object metadata/span unknown");
        auto &out = pass->objects[pass->count++];
        out.host = object.state.generation;
        out.id = raw.objectId;
        out.type = raw.type;
        out.frames = 480;
        if (raw.type == 1) {
          out.xyz[0] = raw.x;
          out.xyz[1] = raw.y;
          out.xyz[2] = raw.z;
        }
        out.volume = raw.volume;
        out.gain = raw.callGain;
        out.ended = (object.source.properties & 8) != 0;
        out.nullPcm = (object.source.flags & q4::NullPcm) != 0;
        out.result = S_OK;
        memcpy(out.pcm, object.pcm.data(), sizeof(out.pcm));
      }
      try {
        bridge_.push(*pass, emit);
      }
      catch (...) {
        fprintf(stderr, "PLUGIN_REJECT domain=%llu sourcePass=%llu frames=%u objects=%zu firstQpc=%lld lastQpc=%lld\n", quantum.domain, quantum.pass, quantum.frames, quantum.objects.size(), first, last);
        for (const auto &object : quantum.objects) fprintf(stderr, "PLUGIN_REJECT_OBJECT generation=%llu id=%u type=%u flags=0x%x frames=%u offset=%u positionState=%u volumeKnown=%u\n", object.state.generation, object.state.audio.objectId, object.state.audio.type, object.source.properties, object.source.audio.frames, object.offset, object.state.positionState, object.state.volumeKnown);
        throw;
      }
    }
    template <class Emit>
    void
    finish(Emit emit) { bridge_.finish(emit); }
  };
}  // namespace sonic_live
