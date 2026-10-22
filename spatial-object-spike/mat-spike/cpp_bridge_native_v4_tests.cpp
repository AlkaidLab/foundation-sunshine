#include "cpp_bridge_native_v4.hpp"
#include "cpp_bridge_queue.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace cpp_bridge_native4;
namespace {
struct Scene {
    PositionCache cache;uint64_t seq=0;
    q4::Object object(uint32_t id=42,uint32_t flags=7,float sample=.2f,bool known=true){
        Record r{};r.magic=q4::magic;r.frames=480;r.host=7;r.objectId=id;r.type=1;r.sequence=seq++;r.qpc=100+seq;
        r.x=known?1.f:cpp_bridge::nan();r.y=known?2.f:cpp_bridge::nan();r.z=known?3.f:cpp_bridge::nan();r.volume=.1f;r.callGain=.44632625579833984f;
        q4::Object o;o.source.audio=r;o.source.properties=flags;o.source.call=1;o.state=cache.Apply(r,flags,0);o.pcm.assign(480,sample);o.offset=0;o.resultSeen=true;return o;
    }
};
q4::Quantum frame(uint64_t domain,uint64_t pass,std::vector<q4::Object> objects={}){q4::Quantum q;q.domain=domain;q.pass=pass;q.sampleStart=(pass-1)*480;q.frames=480;q.fullSpanLayoutReady=true;q.objects=std::move(objects);return q;}
FrameHeader head(const FrameBuffer& f){FrameHeader h{};memcpy(&h,f.bytes().data(),sizeof(h));return h;}
CallHeader call(const FrameBuffer& f,unsigned which=0){CallHeader c{};auto b=f.bytes();memcpy(&c,b.data()+sizeof(FrameHeader)+which*(sizeof(CallHeader)+480*4),sizeof(c));return c;}
template<class Fn>void rejects(Fn fn){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,"expected explicit rejection");}
}
int runNative4Tests(){
    std::vector<std::string> passed;auto test=[&](const char* name,auto fn){fn();passed.push_back(name);};
    test("leading_empty_pass_advances_input_clock",[]{DomainBridge b(1);check(head(b.consume(frame(1,1),10,11)).calls==0&&b.sample()==480,"empty clock");Scene s;auto& out=b.consume(frame(1,2,{s.object()}),20,21);check(head(out).index==1&&call(out).flags==7,"leading empty collapsed");});
    test("eos_last_pcm_then_end_on_next_empty_no_extra_process",[]{DomainBridge b(1);Scene s;auto& last=b.consume(frame(1,1,{s.object(42,15)}),1,2);check(call(last).flags==7,"last PCM dropped");auto& end=b.consume(frame(1,2),3,4);check(head(end).calls==1&&call(end).flags==8,"EOS not drained on empty");auto& eof=b.finish();check(head(eof).flags==1&&head(eof).calls==0&&b.sample()==960,"EOF advanced clock/duplicate end");});
    test("all_empty_domain_and_single_close",[]{DomainBridge b(2);b.consume(frame(2,1),1,2);b.consume(frame(2,2),3,4);check(head(b.finish()).calls==0&&b.sample()==960,"empty domain");rejects([&]{b.finish();});});
    test("new_domain_same_source_key_gets_fresh_id_and_clock",[]{Scene a,c;DomainBridge b(1),d(2);b.consume(frame(1,1,{a.object()}),1,2);b.finish();auto& out=d.consume(frame(2,1,{c.object()}),3,4);check(head(out).index==0&&call(out).id==1&&call(out).flags==7&&d.sample()==480,"domain state leaked");});
    test("gain_combined_f32_once_not_legacy_or_twice",[]{Scene s;auto o=s.object();for(unsigned i=0;i<480;++i)o.pcm[i]=static_cast<float>(i)*.000371f;auto input=o.pcm;float v=o.state.audio.volume,g=o.state.audio.callGain;DomainBridge b(1);auto bytes=b.consume(frame(1,1,{o}),1,2).bytes();bool differs=false;for(unsigned i=0;i<480;++i){float actual;memcpy(&actual,bytes.data()+sizeof(FrameHeader)+sizeof(CallHeader)+i*4,4);float expected=input[i]*(v*g);check(std::bit_cast<uint32_t>(actual)==std::bit_cast<uint32_t>(expected),"gain mismatch");differs|=actual!=(input[i]*v)*g;if(i>0)check(actual!=expected*(v*g),"double gain indistinguishable");}check(differs,"legacy rounding negative control");});
    test("unknown_audible_quarantined_not_origin",[]{Scene s;DomainBridge b(1);auto o=s.object(42,6,.2f,false);check(head(b.consume(frame(1,1,{o}),1,2)).calls==0&&b.counts["quarantined_unknown_audible"]==1,"unknown silently passed");});
    test("unknown_silent_deferred",[]{Scene s;DomainBridge b(1);auto o=s.object(42,6,0,false);check(head(b.consume(frame(1,1,{o}),1,2)).calls==0&&b.counts["deferred_unknown_silent"]==1,"silent unknown");});
    test("active_missing_on_empty_explicitly_rejected",[]{Scene s;DomainBridge b(1);b.consume(frame(1,1,{s.object()}),1,2);rejects([&]{b.consume(frame(1,2),3,4);});});
    test("generation_reuse_unknown_retires_old",[]{Scene s;DomainBridge b(1);b.consume(frame(1,1,{s.object()}),1,2);auto& out=b.consume(frame(1,2,{s.object(42,7,0,false)}),3,4);check(head(out).calls==1&&call(out).flags==8,"old generation retained");});
    test("reordered_calls_keep_ids",[]{Scene s;DomainBridge b(1);b.consume(frame(1,1,{s.object(42),s.object(43)}),1,2);auto& out=b.consume(frame(1,2,{s.object(43,6),s.object(42,6)}),3,4);check(call(out).id==2&&call(out,1).id==1,"identity rebound");});
    test("reject_short_unknown_offset_and_static",[]{Scene s;auto q=frame(1,1,{s.object()});q.frames=240;DomainBridge a(1);rejects([&]{a.consume(q,1,2);});q.frames=480;q.fullSpanLayoutReady=false;DomainBridge b(1);rejects([&]{b.consume(q,1,2);});q.fullSpanLayoutReady=true;q.objects[0].state.audio.type=2;DomainBridge c(1);rejects([&]{c.consume(q,1,2);});});
    test("reject_nonfinite_and_21_objects",[]{Scene s;DomainBridge b(1);auto q=frame(1,1,{s.object()});q.objects[0].pcm[0]=cpp_bridge::nan();rejects([&]{b.consume(q,1,2);});q.objects.clear();for(unsigned i=0;i<21;++i)q.objects.push_back(s.object(100+i));DomainBridge c(1);rejects([&]{c.consume(q,1,2);});});
    test("reject_pass_sample_or_domain_gap",[]{DomainBridge a(1);rejects([&]{a.consume(frame(1,2),1,2);});DomainBridge b(1);auto q=frame(1,1);q.sampleStart=1;rejects([&]{b.consume(q,1,2);});DomainBridge c(1);rejects([&]{c.consume(frame(2,1),1,2);});});
    std::cout<<nlohmann::json({{"native_v4_selftests_passed",passed.size()},{"tests",passed},{"vendor_called",false}}).dump(2)<<'\n';return 0;
}
