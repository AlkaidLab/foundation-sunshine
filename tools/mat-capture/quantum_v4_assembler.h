#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include "quantum_v4_protocol.h"
#include "position_cache.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace q4 {
  struct Object {
    Header source {};
    StateRecord state {};
    std::vector<float> pcm;
    uint32_t offset = UINT32_MAX;
    bool resultSeen = false;
  };
  struct Quantum {
    uint64_t domain = 0, pass = 0, sampleStart = 0, renderer = 0, encoder = 0, implementation = 0;
    uint32_t frames = 0, producedBytes = 0;
    bool fullSpanLayoutReady = false;
    std::vector<Object> objects;
  };
  // Worker-only. Independent cache/clock per observed domain; never concatenate
  // separate instances or infer cross-domain sample synchronization from QPC.
  class Assembler {
    struct Domain {
      uint64_t renderer = 0, encoder = 0, implementation = 0, pass = 0, sample = 0;
      uint32_t frames = 0, thread = 0, producedBytes = 0;
      bool inPass = false, processed = false;
      int64_t qpc = -1;
      std::vector<Object> pending;
      PositionCache cache;
    };
    std::map<uint64_t, Domain> domains_;
    uint64_t sequence_ = 0, lastDomain_ = 0;
    bool poisoned_ = false, closed_ = false;
    void
    check(bool value, const char *message) {
      if (!value) {
        poisoned_ = true;
        throw std::runtime_error(message);
      }
    }

  public:
    uint64_t opened = 0, ended = 0, quanta = 0, objects = 0, results = 0, markers = 0, empty = 0, unknownOffsets = 0, unreadyQuanta = 0;
    std::array<uint64_t, 6> endReasons {};
    bool
    push(const Header &h, const float *pcm, Quantum &output) {
      check(!poisoned_ && !closed_, "v4 assembler closed or poisoned");
      const auto &a = h.audio;
      check(h.version == version && a.magic == magic && !h.reserved && h.domain && h.renderer && h.encoder && h.implementation && a.tid, "v4 identity/version");
      check(a.sequence == sequence_ && a.qpc >= 0, "v4 global sequence/QPC gap");
      ++sequence_;
      check(h.lossEpoch == 0, "v4 capture loss; reject instead of silently recovering");
      if (h.kind != ObjectBefore) check(!a.frames && !h.properties && !a.host && !a.objectId && !a.type && a.x == 0 && a.y == 0 && a.z == 0 && a.volume == 0 && a.callGain == 0, "v4 control event contains PCM/object metadata");
      if (h.kind != PassBegin && h.kind != ProcessResult) check(!h.inputFrames, "v4 unexpected input frame field");
      if (h.kind != ProcessResult) check(!h.producedBytes && !h.capacity, "v4 unexpected encoded output fields");
      if (h.kind != DomainBegin && h.kind != DomainEnd) check(!h.reason, "v4 unexpected lifecycle reason");
      if (h.kind == DomainBegin) {
        check(h.domain > lastDomain_ && domains_.size() < domainCapacity && !h.pass && !h.call && h.reason == Discovery && h.flags == ObservedNotCreated && h.result == 0, "v4 domain begin/order/capacity");
        for (const auto &item : domains_) check(item.second.encoder != h.encoder, "v4 same encoder address in two live domains");
        Domain fresh {};
        fresh.renderer = h.renderer;
        fresh.encoder = h.encoder;
        fresh.implementation = h.implementation;
        fresh.qpc = a.qpc;
        fresh.pending.reserve(64);
        domains_.emplace(h.domain, std::move(fresh));
        lastDomain_ = h.domain;
        ++opened;
        return false;
      }
      auto found = domains_.find(h.domain);
      check(found != domains_.end(), "v4 unknown or retired domain");
      auto &d = found->second;
      check(a.qpc >= d.qpc, "v4 domain QPC reversed");
      d.qpc = a.qpc;
      check(d.renderer == h.renderer && d.encoder == h.encoder && d.implementation == h.implementation, "v4 address/cookie rebound inside a domain");
      if (h.kind == DomainEnd) {
        check(!d.inPass && !h.pass && !h.call && h.result == 0 && h.reason >= Reset && h.reason <= CaptureStop && h.flags == (h.reason == CaptureStop ? 0u : uint32_t(PreCall)), "v4 domain end inside pass/invalid reason");
        ++endReasons[h.reason];
        domains_.erase(found);
        ++ended;
        return false;
      }
      if (h.kind == PassBegin) {
        check(!d.inPass && h.pass == d.pass + 1 && !h.call && h.inputFrames > 0 && h.inputFrames <= kSamples && h.flags == PreCall && h.result == 0, "v4 pass begin/gap");
        d.pass = h.pass;
        d.inPass = true;
        d.processed = false;
        d.producedBytes = 0;
        d.frames = h.inputFrames;
        d.thread = a.tid;
        d.pending.clear();
        return false;
      }
      check(d.inPass && h.pass == d.pass && a.tid == d.thread, "v4 event outside pass or cross-thread");
      if (h.kind == ObjectBefore) {
        check(!d.processed && d.pending.size() < objectCapacity && h.call == d.pending.size() + 1 && (d.pending.empty() || d.pending.back().resultSeen) && h.result == E_PENDING && (h.flags == PreCall || h.flags == (PreCall | NullPcm)) && a.frames > 0 && a.frames <= d.frames && pcm && a.type && !(a.type & (a.type - 1)) && !(h.properties & ~15u) && std::isfinite(a.callGain), "v4 unpaired/reentrant/object bounds");
        check(std::all_of(pcm, pcm + a.frames, [](float value) { return std::isfinite(value); }), "v4 nonfinite PCM");
        if (h.flags & NullPcm) check(std::all_of(pcm, pcm + a.frames, [](float value) { return value == 0; }), "v4 null PCM tag not silent");
        Object object;
        object.source = h;
        object.pcm.assign(pcm, pcm + a.frames);
        d.pending.push_back(std::move(object));
        ++objects;
        return false;
      }
      if (h.kind == SubmitResult) {
        check(!d.processed && !d.pending.empty() && h.call == d.pending.size() && !d.pending.back().resultSeen && h.result == S_OK && h.flags == PostCall, "v4 failed/duplicate/unmatched Submit result");
        d.pending.back().resultSeen = true;
        ++results;
        return false;
      }
      if (h.kind == ProcessResult) {
        check(!d.processed && !h.call && h.inputFrames == d.frames && h.result == S_OK && h.flags == PostCall && h.producedBytes <= h.capacity && (d.pending.empty() || d.pending.back().resultSeen), "v4 failed Process/pending Submit/frame mismatch");
        d.processed = true;
        d.producedBytes = h.producedBytes;
        ++markers;
        return false;
      }
      check(h.kind == PassEnd && d.processed && !h.call && h.flags == PostCall && h.result == 0, "v4 incomplete pass or unknown event");
      check(d.sample <= UINT64_MAX - d.frames, "v4 sample cursor overflow");
      output = {};
      output.domain = h.domain;
      output.pass = h.pass;
      output.sampleStart = d.sample;
      output.frames = d.frames;
      output.producedBytes = d.producedBytes;
      output.renderer = d.renderer;
      output.encoder = d.encoder;
      output.implementation = d.implementation;
      output.fullSpanLayoutReady = true;
      std::set<std::tuple<uint64_t, uint32_t, uint32_t>> keys;
      for (auto &object : d.pending) {
        if (object.source.audio.frames == d.frames)
          object.offset = 0;
        else {
          ++unknownOffsets;
          output.fullSpanLayoutReady = false;
        }
        const auto &raw = object.source.audio;
        if (!keys.emplace(raw.host, raw.objectId, raw.type).second) output.fullSpanLayoutReady = false;
        object.state = d.cache.Apply(raw, object.source.properties, 0);
        check(!d.cache.evictions, "v4 per-domain cache capacity exceeded");
      }
      output.objects = std::move(d.pending);
      d.pending.clear();
      d.pending.reserve(64);
      d.sample += d.frames;
      d.inPass = false;
      ++quanta;
      if (output.objects.empty()) ++empty;
      if (!output.fullSpanLayoutReady) ++unreadyQuanta;
      return true;
    }
    void
    finish() {
      check(!poisoned_ && !closed_ && domains_.empty(), "v4 EOF without explicit domain closure");
      closed_ = true;
    }
  };
}  // namespace q4
