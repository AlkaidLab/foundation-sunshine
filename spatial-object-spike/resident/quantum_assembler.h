#pragma once
#include "position_cache.h"
#include <array>
#include <vector>
#include <set>
#include <stdexcept>
#include <limits>

namespace quantum_capture {
inline void Require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
struct Object {
    QuantumRecord source{};
    StateRecord state{};
    std::vector<float> pcm;
    uint32_t offset=UINT32_MAX; // Not supplied by SetAudioObject's known ABI.
};
struct Quantum {
    uint64_t index=0, sampleStart=0, encoder=0;
    uint32_t frames=0;
    int64_t firstQpc=0,lastQpc=0;
    bool fullSpanLayoutReady=false; // Layout only, not known XYZ/gain/MAT readiness.
    std::vector<Object> objects;
};

// Worker-only, one selected encoder observation domain. No key-count framing,
// QPC-to-sample conversion, implicit padding, clipping, or invented short-block offset.
class Assembler {
    static constexpr size_t limit=64;
    uint64_t sequence_=0, encoder_=0, epoch_=0, sample_=0, quantum_=0;
    int64_t qpc_=-1;
    bool started_=false, anchored_=false, finished_=false, poisoned_=false;
    uint32_t thread_=0;
    std::vector<Object> pending_;
    PositionCache cache_;
    void check(bool okay,const char* message) {
        if(!okay) {poisoned_=true;pending_.clear();throw std::runtime_error(message);}
    }
public:
    uint64_t objectsSeen=0,markersSeen=0,discardedAttachObjects=0,trailingObjects=0;
    uint64_t completed=0,empty=0,unknownOffsetObjects=0,notBridgeReady=0;
    Assembler() {pending_.reserve(limit);}
    bool push(const QuantumRecord& record,const float* pcm,Quantum& output) {
        check(!finished_ && !poisoned_,"assembler closed or poisoned; start a new capture domain");
        const auto& a=record.audio;
        check(a.magic==0x474f424a && !record.reserved && record.encoder!=0 && a.tid,"v3 header identity/reserved");
        check(a.sequence==sequence_ && a.qpc>=0 && a.qpc>=qpc_,"v3 sequence gap or backwards QPC");
        ++sequence_;qpc_=a.qpc;
        if(!started_) {encoder_=record.encoder;epoch_=record.lossEpoch;started_=true;}
        check(record.encoder==encoder_,"multiple/replaced encoder domains cannot share a sample clock");
        check(record.lossEpoch==epoch_,"transport loss: stop rather than silently join sample timelines");
        if(record.kind==kQuantumObject) {
            check(a.frames>0 && a.frames<=kSamples && pcm && !record.inputFrames
                && record.result==S_OK && !record.producedBytes && !record.capacity
                && !(record.propertyFlags&~15u) && a.type && !(a.type&(a.type-1))
                && std::isfinite(a.callGain),"v3 object header");
            check(pending_.size()<limit,"v3 bounded pending object capacity exceeded");
            check(!thread_ || thread_==a.tid,"concurrent encoder submitters; ordering contract unverified");
            thread_=a.tid;
            check(std::all_of(pcm,pcm+a.frames,[](float x){return std::isfinite(x);}),"v3 nonfinite PCM");
            Object object;object.source=record;object.pcm.assign(pcm,pcm+a.frames);
            pending_.push_back(std::move(object));++objectsSeen;return false;
        }
        check(record.kind==kQuantumProcess && !a.frames && !record.propertyFlags
            && record.inputFrames<=kSamples && record.result==S_OK
            && record.producedBytes<=record.capacity,"v3 failed/invalid Process marker");
        check(!thread_ || thread_==a.tid,"Process marker not on the submitter thread");
        ++markersSeen;thread_=0;
        if(!anchored_) {
            // Capture may have begun halfway through a pass. Discard BOTH its
            // audio and metadata; unknown state remains unknown until observed again.
            discardedAttachObjects+=pending_.size();pending_.clear();anchored_=true;return false;
        }
        check(sample_<=UINT64_MAX-record.inputFrames,"sample clock overflow");
        output={};output.index=quantum_++;output.sampleStart=sample_;output.frames=record.inputFrames;
        output.encoder=encoder_;output.firstQpc=pending_.empty()?a.qpc:pending_.front().source.audio.qpc;
        output.lastQpc=a.qpc;output.fullSpanLayoutReady=true;
        std::set<std::tuple<uint64_t,uint32_t,uint32_t>> seen;
        for(auto& object:pending_) {
            const auto& raw=object.source.audio;
            check(raw.frames<=record.inputFrames,"object exceeds actual Process inputFrames");
            // A full pass has only one in-pass placement. A short packet does not.
            if(raw.frames==record.inputFrames) object.offset=0;
            else {++unknownOffsetObjects;output.fullSpanLayoutReady=false;}
            if(!seen.emplace(raw.host,raw.objectId,raw.type).second)output.fullSpanLayoutReady=false;
            object.state=cache_.Apply(raw,object.source.propertyFlags,epoch_);
            check(!cache_.evictions,"v3 position cache capacity exceeded");
        }
        output.objects=std::move(pending_);pending_.clear();pending_.reserve(limit);
        sample_+=record.inputFrames;++completed;
        if(output.objects.empty())++empty;
        if(!output.fullSpanLayoutReady)++notBridgeReady;
        return true;
    }
    void finish() {
        check(!finished_ && !poisoned_,"assembler closed or poisoned");
        trailingObjects=pending_.size();pending_.clear();finished_=true;
        // Unsealed trailing data is reported, never emitted as a complete pass.
    }
};
}
