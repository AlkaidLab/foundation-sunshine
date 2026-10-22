#define NOMINMAX
#include "cpp_bridge_core.hpp"
#include "../resident/quantum_v4_assembler.h"
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
int main(int argc,char** argv) {
    try {
        cpp_bridge::check(argc==2,"Usage: quantum-v4-bridge-test SYNTHETIC_EVENTS");
        std::ifstream input(argv[1],std::ios::binary);cpp_bridge::check(bool(input),"fixture open");
        q4::Assembler parser;std::map<uint64_t,std::unique_ptr<cpp_bridge::Bridge>> bridges;
        uint64_t quanta=0,matchedSamples=0,unknownObjects=0;
        for(;;) {
            q4::Header h{};input.read(reinterpret_cast<char*>(&h),sizeof(h));auto got=input.gcount();
            if(!got){cpp_bridge::check(input.eof(),"fixture I/O");break;}
            cpp_bridge::check(got==sizeof(h)&&h.audio.frames<=kSamples,"fixture header/bounds");
            std::array<float,kSamples> pcm{};input.read(reinterpret_cast<char*>(pcm.data()),h.audio.frames*4);cpp_bridge::check(input.gcount()==h.audio.frames*4,"fixture PCM");
            q4::Quantum q;
            if(!parser.push(h,pcm.data(),q))continue;
            cpp_bridge::check(q.frames==480&&q.fullSpanLayoutReady&&!q.objects.empty()&&q.objects.size()<=cpp_bridge::max_round_objects,"this bridge test accepts full 480-frame nonempty fixture passes only");
            auto& bridge=bridges[q.domain];if(!bridge)bridge=std::make_unique<cpp_bridge::Bridge>();
            auto batch=std::make_unique<cpp_bridge::Batch>();batch->index=static_cast<uint32_t>(q.pass-1);batch->count=static_cast<uint32_t>(q.objects.size());
            for(size_t i=0;i<q.objects.size();++i){batch->records[i].state=q.objects[i].state;std::copy(q.objects[i].pcm.begin(),q.objects[i].pcm.end(),batch->records[i].pcm.begin());}
            const auto& output=bridge->consume(*batch);auto bytes=output.dynamic.bytes();cpp_bridge::FrameHeader frame{};memcpy(&frame,bytes.data(),sizeof(frame));size_t offset=sizeof(frame);unsigned calls=0;
            for(const auto& object:q.objects) {
                if(object.state.positionState==0){++unknownObjects;continue;}
                cpp_bridge::CallHeader call{};cpp_bridge::check(offset+sizeof(call)+480*4<=bytes.size(),"bridge call length");memcpy(&call,bytes.data()+offset,sizeof(call));offset+=sizeof(call);
                cpp_bridge::check(call.flags==7&&call.xyz[0]==object.state.audio.x,"bridge domain identity/position");
                for(unsigned i=0;i<480;++i) {
                    float actual=0;memcpy(&actual,bytes.data()+offset+i*4,4);
                    float first=object.pcm[i]*object.state.audio.volume;float expected=first*object.state.audio.callGain;
                    cpp_bridge::check(actual==expected,"pre-snapshot -> existing FFmpeg bridge did not apply gain exactly once");
                    if(i==479){float twice=expected*object.state.audio.volume;twice*=object.state.audio.callGain;cpp_bridge::check(actual!=twice,"test failed to distinguish double gain");}
                    ++matchedSamples;
                }
                offset+=480*4;++calls;
            }
            cpp_bridge::check(frame.calls==calls&&offset==bytes.size(),"unknown domain inherited a stale known position");++quanta;
        }
        parser.finish();for(auto& item:bridges)item.second->finish();
        cpp_bridge::check(quanta==3&&bridges.size()==3&&matchedSamples==1440&&unknownObjects==1,"fixture coverage changed");
        std::cout<<"{\"passed\":true,\"independent_domains\":3,\"quanta\":3,\"samples_gain_applied_exactly_once\":1440,\"unknown_position_objects_quarantined\":1,\"uses_existing_FFmpeg_bridge\":true,\"native_verified\":false}\n";return 0;
    }catch(const std::exception& error){std::cerr<<"FAILED "<<error.what()<<'\n';return 1;}
}
