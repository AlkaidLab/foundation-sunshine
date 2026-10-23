#pragma once
#include "protocol.h"
#include "../../src/spatial_audio/stream_output_protocol.h"
#include <map>
#include <set>
#include <tuple>
#include <cmath>
#include <stdexcept>
#include <memory>
#include <string>
namespace sonic_live {
inline void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
// Strict first route: one Sonic renderer, 48kHz/480-frame full-span dynamic
// objects, <=20. No HRTF stereo, wall-clock frame guesses, offsets or resampling.
class Bridge {
    struct Active {uint32_t outputId;bool ended;};
    using Key=std::pair<uint64_t,uint32_t>;
    std::map<Key,Active> active_;uint64_t renderer_=0,nextPass_=0;uint32_t nextId_=1,index_=0;
    bool closed_=false,poisoned_=false;
public:
    uint64_t inputSamples=0,sourcePasses=0,emptyPasses=0;
    uint64_t renderer()const{return renderer_;}
    template<class Emit>void push(const Pass& p,Emit emit){
        check(!closed_&&!poisoned_,"Sonic bridge terminal");
        try {
            check(p.sequence==nextPass_++&&p.renderer&&p.count<=objects&&p.endCount<=objects&&p.firstQpc>0&&p.lastQpc>=p.firstQpc,"Sonic pass sequence/identity/time");
            if(!renderer_&&p.count)renderer_=p.renderer;
            if(!renderer_)return;
            if(p.renderer!=renderer_){check(!p.count,"Multiple Sonic renderers with objects; explicit routing required");return;}
            check(p.rate==48000&&p.deviceFrames==480,"Sonic source must be 48kHz/480-frame device passes");
            check(index_<UINT32_MAX-100,"Sonic source index overflow");
            auto packet=std::make_unique<receiver::Slot>();packet->header.index=index_++;packet->header.frames=480;packet->header.kind=1;
            packet->header.firstQpc=p.firstQpc;packet->header.lastQpc=p.lastQpc;uint32_t count=0;
            for(auto at=active_.begin();at!=active_.end();){if(at->second.ended){auto& c=packet->calls[count++];c.id=at->second.outputId;c.type=1;c.flags=8;c.frames=480;at=active_.erase(at);}else ++at;}
            std::set<Key> seen;
            for(uint32_t n=0;n<p.count;++n){const auto& raw=p.objects[n];
                check(raw.host&&raw.id!=UINT32_MAX&&raw.type==1&&raw.offset==0&&raw.frames==480&&raw.result==S_OK&&raw.ended<=1&&raw.nullPcm<=1,"Unsupported Sonic object type/span/result");
                for(auto v:raw.xyz)check(std::isfinite(v)&&std::abs(v)<=10000,"Sonic XYZ not finite/in range");
                check(std::isfinite(raw.volume)&&std::isfinite(raw.gain),"Sonic gain not finite");float gain=raw.volume*raw.gain;check(std::isfinite(gain),"Sonic combined gain overflow");
                Key key{raw.host,raw.id};check(seen.insert(key).second,"Duplicate/fragmented Sonic object in pass");
                auto at=active_.find(key);bool created=at==active_.end();
                if(created){check(active_.size()<20&&nextId_!=0,"Sonic object capacity/ID overflow");at=active_.emplace(key,Active{nextId_++,false}).first;}
                check(count<receiver::callCapacity,"Sonic call capacity");auto& c=packet->calls[count++];
                c.id=at->second.outputId;c.type=1;c.flags=created?7:6;c.frames=480;memcpy(c.xyz,raw.xyz,sizeof(c.xyz));
                for(unsigned s=0;s<480;++s){check(std::isfinite(raw.pcm[s])&&(!raw.nullPcm||raw.pcm[s]==0),"Sonic PCM invalid");c.pcm[s]=raw.pcm[s]*gain;check(std::isfinite(c.pcm[s])&&std::abs(c.pcm[s])<=1,"Sonic scaled PCM out of native range");}
                at->second.ended=raw.ended!=0;inputSamples+=480;
            }
            // A successful native EOS can arrive in a later pass with no Mono
            // write. Preserve that explicit control instead of inferring end
            // from absence, or falsely rejecting the now-retired object.
            for(uint32_t n=0;n<p.endCount;++n){const auto& e=p.ends[n];check(e.host&&e.id!=UINT32_MAX&&!e.reserved,"Sonic EOS identity");Key key{e.host,e.id};auto at=active_.find(key);if(at==active_.end())continue;
                if(seen.contains(key))at->second.ended=true;
                else {check(count<receiver::callCapacity,"Sonic EOS capacity");auto& c=packet->calls[count++];c.id=at->second.outputId;c.type=1;c.flags=8;c.frames=480;active_.erase(at);}
            }
            for(const auto& [key,a]:active_)if(!seen.contains(key))throw std::runtime_error("Missing live object; no inferred silence/end: identity="+std::to_string(key.first)+" id="+std::to_string(key.second)+" pass="+std::to_string(p.sequence)+" observedObjects="+std::to_string(p.count));
            packet->header.calls=count;check(emit(*packet),"Native MAT input refused; epoch terminal");++sourcePasses;if(!p.count)++emptyPasses;
        }catch(...){poisoned_=true;throw;}
    }
    template<class Emit>void finish(Emit emit){
        check(!closed_&&!poisoned_&&renderer_&&sourcePasses,"Sonic input cannot close empty/failed");closed_=true;
        auto packet=std::make_unique<receiver::Slot>();packet->header.index=index_;packet->header.frames=480;packet->header.kind=2;
        for(const auto& [key,a]:active_){(void)key;auto& c=packet->calls[packet->header.calls++];c.id=a.outputId;c.type=1;c.flags=8;c.frames=480;}
        check(emit(*packet),"Native MAT EOF refused");active_.clear();
    }
};
}
