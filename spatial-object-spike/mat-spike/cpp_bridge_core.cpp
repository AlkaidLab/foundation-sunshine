#include <stdexcept>
#include <cstring>
#include <algorithm>
#include "cpp_bridge_core.hpp"

namespace cpp_bridge {
bool Framed::singlePositionReady() const {
    if(spans.empty() || !spans.front().xyz)return false;
    return std::all_of(spans.begin(),spans.end(),[&](const auto& s){return s.xyz==spans.front().xyz;});
}
Framer::Framer(){fifo_=av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT,1,capacity);check(fifo_,"FFmpeg FIFO allocation");}
Framer::~Framer(){av_audio_fifo_free(fifo_);}
std::vector<Framed> Framer::push(std::span<const float> samples,Position xyz){
    check(samples.size()<=max_source_frames,"source PCM larger than bounded input");
    check(std::all_of(samples.begin(),samples.end(),[](float x){return std::isfinite(x);}),"nonfinite FIFO PCM");
    check(!xyz || std::all_of(xyz->begin(),xyz->end(),[](float x){return std::isfinite(x);}),"nonfinite known FIFO position");
    check(samples.size()<=static_cast<size_t>(av_audio_fifo_space(fifo_)),"bounded FIFO full; no implicit realloc");
    if(samples.empty())return {};
    void* input=const_cast<float*>(samples.data());
    check(av_audio_fifo_write(fifo_,&input,static_cast<int>(samples.size()))==static_cast<int>(samples.size()),"FIFO write");
    metadata_.push_back({static_cast<unsigned>(samples.size()),xyz});
    maxBuffered=std::max(maxBuffered,static_cast<unsigned>(buffered()));
    std::vector<Framed> result;result.reserve(buffered()/quantum);
    while(buffered()>=static_cast<int>(quantum)){
        Framed block{};void* output=block.pcm.data();check(av_audio_fifo_read(fifo_,&output,quantum)==quantum,"FIFO read");
        unsigned cursor=0;
        while(cursor<quantum){
            check(!metadata_.empty(),"metadata shorter than PCM");auto& span=metadata_.front();unsigned n=std::min(quantum-cursor,span.frames);
            block.spans.push_back({cursor,n,span.xyz});cursor+=n;span.frames-=n;if(!span.frames)metadata_.pop_front();
        }
        result.push_back(std::move(block));
    }
    return result;
}
void Framer::finish() const {check(buffered()==0 && metadata_.empty(),"partial final quantum; no silent pad/drop");}

FrameBuffer::FrameBuffer(){data_.reserve(sizeof(FrameHeader)+max_calls*(sizeof(CallHeader)+quantum*sizeof(float)));}
void FrameBuffer::begin(uint32_t index,int64_t first,int64_t last,bool eof){
    header_={index,quantum,0,static_cast<uint32_t>(eof),first,last};data_.clear();data_.resize(sizeof(header_));memcpy(data_.data(),&header_,sizeof(header_));
}
void FrameBuffer::call(uint32_t id,uint32_t slot,uint32_t type,uint32_t flags,Position xyz,std::span<const float> pcm){
    check(header_.calls<max_calls && pcm.size()==quantum,"output call bounds");
    check(std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}),"nonfinite output PCM");
    CallHeader h{id,slot,type,flags,{nan(),nan(),nan()},quantum};if(xyz)std::copy(xyz->begin(),xyz->end(),h.xyz);
    const size_t old=data_.size(),next=old+sizeof(h)+pcm.size_bytes();check(next<=data_.capacity(),"output capacity; no growth");
    data_.resize(next);memcpy(data_.data()+old,&h,sizeof(h));memcpy(data_.data()+old+sizeof(h),pcm.data(),pcm.size_bytes());++header_.calls;memcpy(data_.data(),&header_,sizeof(header_));
}
std::span<const uint8_t> FrameBuffer::bytes() const{return data_;}
FileHeader fileHeader(const char* magic,uint32_t records,uint64_t hz){FileHeader h{};memcpy(h.magic,magic,8);h.version=1;h.rate=48000;h.frames=quantum;h.records=records;h.qpcHz=hz;return h;}
SourceKey sourceKey(const RecordInput& r){return {r.state.audio.host,r.state.audio.objectId,r.state.audio.type};}

Bridge::Bridge(){active_.reserve(20);pending_.reserve(20);identities.reserve(64);}
void Bridge::endActive(size_t index,const char* reason){
    auto& a=active_.at(index);a.framer->finish();std::array<float,quantum> silence{};
    output_.dynamic.call(a.id,a.slot,1,8,a.xyz,silence);++counts[reason];active_.erase(active_.begin()+index);
}
const Output& Bridge::consume(const Batch& batch){
    check(!closed_ && batch.index==lastFrame_+1,"closed bridge or quantum gap");
    check(batch.count>0 && batch.count<=max_round_objects,"source round width");lastFrame_=batch.index;
    output_.dynamic.begin(batch.index,batch.records[0].state.audio.qpc,batch.records[batch.count-1].state.audio.qpc);
    output_.bed.begin(batch.index,batch.records[0].state.audio.qpc,batch.records[batch.count-1].state.audio.qpc);
    std::array<float,quantum> silence{};
    for(const auto& e:pending_){output_.dynamic.call(e.id,e.slot,1,8,e.xyz,silence);++counts["bridge_end_source_end"];}
    pending_.clear();
    for(unsigned ri=0;ri<batch.count;++ri){
        const auto& in=batch.records[ri];const auto& disk=in.state;const auto& audio=disk.audio;
        check(audio.frames==quantum && (disk.propertyFlags&~15u)==0,"this adapter requires aligned 480-frame source and known flags");
        auto skey=sourceKey(in);auto slotIt=sourceSlots_.find(skey);
        if(slotIt==sourceSlots_.end()){check(sourceSlots_.size()<4096,"source registry capacity");slotIt=sourceSlots_.emplace(skey,static_cast<uint32_t>(sourceSlots_.size())).first;}
        const uint32_t slot=slotIt->second;
        if(disk.lossEpoch!=epoch_){
            while(!active_.empty())endActive(0,"bridge_end_loss_invalidation");
            for(const auto& [key,f]:bedFramers_)f->finish();bedFramers_.clear();epoch_=disk.lossEpoch;
        }
        // Reuse the collector's actual C++ state machine, reconstructing the raw property XYZ.
        Record raw=audio;raw.x=disk.rawX;raw.y=disk.rawY;raw.z=disk.rawZ;
        auto state=cache.Apply(raw,disk.propertyFlags,disk.lossEpoch);
        auto equal=[](float x,float y){return x==y || (std::isnan(x)&&std::isnan(y));};
        check(state.generation==disk.generation && state.positionState==disk.positionState && state.volumeKnown==disk.volumeKnown
            && equal(state.audio.volume,audio.volume) && equal(state.audio.x,audio.x) && equal(state.audio.y,audio.y) && equal(state.audio.z,audio.z)
            && cache.evictions==0,"C++ cache differs from captured snapshot/generation");
        ++counts["source_records"];counts["source_object_samples"]+=quantum;
        Position xyz;if(state.positionState>=1 && state.positionState<=3)xyz=std::array<float,3>{state.audio.x,state.audio.y,state.audio.z};
        const Key key{audio.host,audio.objectId,audio.type,disk.lossEpoch,state.generation};
        // Retire an old generation even if the NEW generation has unknown coordinates.
        for(size_t i=0;i<active_.size();){
            const auto& a=active_[i];
            if(std::get<0>(a.key)==audio.host && std::get<1>(a.key)==audio.objectId && std::get<2>(a.key)==audio.type && a.key!=key)
                endActive(i,"bridge_end_generation_change");
            else ++i;
        }
        auto activeIt=std::find_if(active_.begin(),active_.end(),[&](const auto& a){return a.key==key;});
        if(audio.type==1 && (!xyz || !state.volumeKnown)){
            bool audible=std::any_of(in.pcm.begin(),in.pcm.begin()+quantum,[](float x){return x!=0;});
            ++counts[audible?"quarantined_unknown_audible_records":"deferred_unknown_silent_records"];
            counts["quarantined_or_deferred_samples"]+=quantum;
            if(activeIt!=active_.end())endActive(activeIt-active_.begin(),"bridge_end_unknown");continue;
        }
        check(state.volumeKnown && std::isfinite(audio.callGain),"unknown volume/nonfinite callGain");
        std::array<float,quantum> scaled{};
        for(unsigned i=0;i<quantum;++i){float first=in.pcm[i]*state.audio.volume;scaled[i]=first*audio.callGain;}
        if(audio.type!=1){
            auto& framer=bedFramers_[skey];if(!framer)framer=std::make_unique<Framer>();
            auto blocks=framer->push(scaled,std::nullopt);check(blocks.size()==1 && !framer->buffered(),"bed alignment");
            output_.bed.call(0,slot,audio.type,disk.propertyFlags,std::nullopt,blocks[0].pcm);
            counts["preserved_static_samples"]+=quantum;continue;
        }
        uint32_t flags=6;
        if(activeIt==active_.end()){
            check(active_.size()<20 && nextId_!=0,"dynamic object/ID capacity");
            uint32_t id=nextId_++;
            active_.push_back({key,id,slot,xyz,std::make_unique<Framer>()});activeIt=active_.end()-1;flags=7;
            identities.push_back({id,slot,audio.host,disk.lossEpoch,state.generation,audio.objectId,audio.type,batch.index,(disk.propertyFlags&1)!=0});
            ++counts["bridge_creates"];
        }
        activeIt->xyz=xyz;auto blocks=activeIt->framer->push(scaled,xyz);
        check(blocks.size()==1 && blocks[0].singlePositionReady() && !activeIt->framer->buffered(),"native single-snapshot alignment");
        if(std::any_of(scaled.begin(),scaled.end(),[](float x){return std::abs(x)>1;}))++counts["native_pcm_range_exceedances"];
        output_.dynamic.call(activeIt->id,slot,1,flags,xyz,blocks[0].pcm);
        counts["emitted_dynamic_samples"]+=quantum;++counts["dynamic_calls"];
        if(disk.propertyFlags&8){activeIt->framer->finish();pending_.push_back({activeIt->id,slot,xyz});active_.erase(activeIt);}
    }
    counts["max_active_objects"]=std::max<uint64_t>(counts["max_active_objects"],active_.size());return output_;
}
FrameBuffer& Bridge::finish(){
    check(!closed_,"bridge already finalized");closed_=true;output_.dynamic.begin(static_cast<uint32_t>(lastFrame_+1),-1,-1,true);
    std::array<float,quantum> silence{};
    for(auto& e:pending_){output_.dynamic.call(e.id,e.slot,1,8,e.xyz,silence);++counts["bridge_end_source_end"];}
    pending_.clear();while(!active_.empty())endActive(0,"bridge_end_capture_eof");
    for(const auto& [key,framer]:bedFramers_)framer->finish();return output_.dynamic;
}
} // namespace cpp_bridge
