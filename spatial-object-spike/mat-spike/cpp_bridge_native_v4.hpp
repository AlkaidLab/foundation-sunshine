#pragma once
#include "cpp_bridge_core.hpp"
#include "../resident/quantum_v4_assembler.h"
#include <set>

namespace cpp_bridge_native4 {
using namespace cpp_bridge;
// Worker-only adapter. No process/codec access. The old v2 Bridge stays frozen.
// One instance per observed domain; no QPC-derived or cross-domain sample clock.
class DomainBridge {
    using Key=std::tuple<uint64_t,uint32_t,uint32_t,uint64_t>;
    struct Active {uint32_t id,slot;Position xyz;std::unique_ptr<Framer> fifo;};
    struct End {uint32_t id,slot;Position xyz;};
    std::map<Key,Active> active_;
    std::map<SourceKey,uint32_t> slots_;
    std::vector<End> pending_;
    FrameBuffer output_;
    uint64_t domain_,pass_=0,sample_=0;
    uint32_t nextId_=1;
    bool closed_=false;
    void end(uint32_t id,uint32_t slot,Position xyz){
        std::array<float,quantum> silence{};output_.call(id,slot,1,8,xyz,silence);++counts["end_calls"];
    }
public:
    std::map<std::string,uint64_t> counts;
    std::vector<Identity> identities;
    explicit DomainBridge(uint64_t domain):domain_(domain){check(domain!=0,"zero domain");pending_.reserve(20);}
    uint64_t sample()const{return sample_;}
    uint64_t passes()const{return pass_;}
    const FrameBuffer& consume(const q4::Quantum& q,int64_t first,int64_t last){
        check(!closed_&&q.domain==domain_&&q.pass==pass_+1&&q.pass<UINT32_MAX&&q.sampleStart==sample_,"domain/pass/sample gap");
        check(q.frames==quantum&&q.fullSpanLayoutReady&&q.objects.size()<=20,"requires full-span 480-frame dynamic passes, <=20 objects");
        check(first>=0&&last>=first,"invalid captured pass QPC");
        output_.begin(static_cast<uint32_t>(q.pass-1),first,last);
        for(const auto& e:pending_)end(e.id,e.slot,e.xyz);
        pending_.clear();
        std::set<Key> seen;
        for(const auto& object:q.objects){
            const auto& s=object.state;const auto& a=s.audio;
            check(a.type==1&&a.frames==quantum&&object.pcm.size()==quantum&&object.offset==0,"static/short/unknown-offset input not supported here");
            check(s.lossEpoch==0&&s.generation>0&&!(s.propertyFlags&~15u),"state/loss invalid");
            check(std::all_of(object.pcm.begin(),object.pcm.end(),[](float v){return std::isfinite(v);}),"nonfinite input PCM");
            Key key{a.host,a.objectId,a.type,s.generation};check(seen.insert(key).second,"duplicate object within pass");
            SourceKey source{a.host,a.objectId,a.type};
            auto slot=slots_.find(source);
            if(slot==slots_.end()){check(slots_.size()<4096,"source registry full");slot=slots_.emplace(source,static_cast<uint32_t>(slots_.size())).first;}
            // A new generation retires old state even if its new XYZ is unknown.
            for(auto it=active_.begin();it!=active_.end();){
                const auto& k=it->first;
                if(std::get<0>(k)==a.host&&std::get<1>(k)==a.objectId&&std::get<2>(k)==a.type&&k!=key){
                    it->second.fifo->finish();end(it->second.id,it->second.slot,it->second.xyz);it=active_.erase(it);++counts["generation_retirements"];
                }else ++it;
            }
            ++counts["source_object_packets"];counts["source_samples"]+=quantum;
            Position xyz;
            if(s.positionState>=1&&s.positionState<=3)xyz=std::array<float,3>{a.x,a.y,a.z};
            auto found=active_.find(key);
            if(!xyz||!s.volumeKnown){
                bool audible=std::any_of(object.pcm.begin(),object.pcm.end(),[](float v){return v!=0;});
                ++counts[audible?"quarantined_unknown_audible":"deferred_unknown_silent"];counts["quarantined_samples"]+=quantum;
                if(found!=active_.end()){found->second.fifo->finish();end(found->second.id,found->second.slot,found->second.xyz);active_.erase(found);}
                continue;
            }
            check(std::isfinite(a.volume)&&std::isfinite(a.callGain)&&std::all_of(xyz->begin(),xyz->end(),[](float v){return std::isfinite(v);}),"nonfinite known metadata/gain");
            uint32_t flags=6;
            if(found==active_.end()){
                check(active_.size()<20&&nextId_!=0,"dynamic capacity/ID overflow");
                uint32_t id=nextId_++;
                found=active_.emplace(key,Active{id,slot->second,xyz,std::make_unique<Framer>()}).first;flags=7;
                identities.push_back({id,slot->second,a.host,0,s.generation,a.objectId,a.type,static_cast<uint32_t>(q.pass-1),(s.propertyFlags&1)!=0});
                ++counts["creates"];
            }
            counts["max_active_objects"]=std::max<uint64_t>(counts["max_active_objects"],active_.size());
            // Native v3 recorded F32(source * F32(volume * callGain)); preserve
            // that operation order, NOT the legacy bridge's two multiplications.
            float gain=a.volume*a.callGain;check(std::isfinite(gain),"combined gain overflow");
            std::array<float,quantum> scaled{};
            for(unsigned i=0;i<quantum;++i){scaled[i]=object.pcm[i]*gain;check(std::isfinite(scaled[i])&&std::abs(scaled[i])<=1,"scaled PCM out of diagnostic native range");}
            auto blocks=found->second.fifo->push(scaled,xyz);
            check(blocks.size()==1&&blocks[0].singlePositionReady()&&!found->second.fifo->buffered(),"FFmpeg/XYZ frame alignment");
            counts["max_fifo_samples"]=std::max<uint64_t>(counts["max_fifo_samples"],found->second.fifo->maxBuffered);
            output_.call(found->second.id,found->second.slot,1,flags,xyz,blocks[0].pcm);found->second.xyz=xyz;
            ++counts["audio_calls"];counts["emitted_audio_samples"]+=quantum;
            if(s.propertyFlags&8){
                found->second.fifo->finish();pending_.push_back({found->second.id,found->second.slot,xyz});active_.erase(found);++counts["observed_source_ends"];
            }
        }
        // Do not guess that an absent still-live object's PCM is silence. Native
        // empty-pass behavior with active missing objects requires its own proof.
        for(const auto& [key,a]:active_){(void)a;check(seen.contains(key),"missing live object in pass; refuse stale-buffer or silence guess");}
        if(q.objects.empty())++counts["empty_input_passes"];
        ++pass_;sample_+=quantum;++counts["process_frames"];return output_;
    }
    const FrameBuffer& finish(){
        check(!closed_&&pass_<UINT32_MAX,"domain already closed/overflow");closed_=true;
        // EOF is a control record, NOT another 480-frame Process invocation.
        output_.begin(static_cast<uint32_t>(pass_),-1,-1,true);
        for(const auto& e:pending_)end(e.id,e.slot,e.xyz);pending_.clear();
        for(auto& [key,a]:active_){(void)key;a.fifo->finish();end(a.id,a.slot,a.xyz);++counts["capture_close_retirements"];}
        active_.clear();++counts["control_close_records"];return output_;
    }
};
} // namespace cpp_bridge_native4
