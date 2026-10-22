#include "quantum_assembler.h"
#include <cstdio>
#include <functional>
using namespace quantum_capture;
struct Fixture {
    uint64_t sequence=0;int64_t qpc=100;
    std::array<float,kSamples> pcm{};
    QuantumRecord object(uint32_t id=1,uint32_t frames=480,uint32_t flags=6) {
        QuantumRecord r{};r.audio={0x474f424a,frames,7,qpc++,id,1,1,2,3,.5f,.25f,123,sequence++};
        r.kind=kQuantumObject;r.propertyFlags=flags;r.encoder=0x123400;return r;
    }
    QuantumRecord marker(uint32_t frames=480) {
        QuantumRecord r{};r.audio.magic=0x474f424a;r.audio.sequence=sequence++;r.audio.qpc=qpc++;r.audio.tid=123;
        r.kind=kQuantumProcess;r.encoder=0x123400;r.inputFrames=frames;r.capacity=30720;return r;
    }
};
template<class F>static void Reject(F fn) {bool caught=false;try{fn();}catch(const std::exception&){caught=true;}Require(caught,"expected rejection");}
int main() {
    unsigned tests=0;
    auto test=[&](const char* name,auto fn){try{fn();++tests;}catch(...){printf("FAILED %s\n",name);throw;}};
    try {
        test("discard_partial_attach_pass",[]{Fixture f;Assembler a;Quantum q;a.push(f.object(),f.pcm.data(),q);Require(!a.push(f.marker(),nullptr,q) && a.discardedAttachObjects==1,"partial attach leaked");a.finish();});
        test("variable_membership_and_empty_pass",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);Require(a.push(f.marker(),nullptr,q)&&q.objects.empty()&&q.sampleStart==0,"empty pass");a.push(f.object(9),f.pcm.data(),q);Require(a.push(f.marker(),nullptr,q)&&q.objects.size()==1&&q.sampleStart==480,"one key");for(unsigned i:{3,2,1})a.push(f.object(i),f.pcm.data(),q);Require(a.push(f.marker(),nullptr,q)&&q.objects.size()==3&&q.objects[0].source.audio.objectId==3&&q.sampleStart==960,"changed/reordered key set");a.finish();});
        test("integer_sample_clock_not_qpc",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(1,240),f.pcm.data(),q);a.push(f.marker(240),nullptr,q);f.qpc+=9999999;a.push(f.object(1,720),f.pcm.data(),q);Require(a.push(f.marker(720),nullptr,q)&&q.sampleStart==240&&q.frames==720&&q.objects[0].offset==0,"sample count guessed from wall time");});
        test("unknown_short_offset_preserved",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(1,240),f.pcm.data(),q);a.push(f.marker(),nullptr,q);Require(!q.fullSpanLayoutReady&&q.objects[0].offset==UINT32_MAX&&a.unknownOffsetObjects==1,"invented short-block zero offset");});
        test("multiple_packets_not_flattened",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.object(1,240);a.push(r,f.pcm.data(),q);r=f.object(1,240);r.audio.x=9;a.push(r,f.pcm.data(),q);a.push(f.marker(),nullptr,q);Require(!q.fullSpanLayoutReady&&q.objects.size()==2&&q.objects[0].state.audio.x==1&&q.objects[1].state.audio.x==9,"flattened in-pass metadata");});
        test("volume_only_keeps_observed_position",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(),f.pcm.data(),q);a.push(f.marker(),nullptr,q);auto r=f.object(1,480,2);r.audio.x=r.audio.y=r.audio.z=0;a.push(r,f.pcm.data(),q);a.push(f.marker(),nullptr,q);Require(q.objects[0].state.audio.x==1&&q.objects[0].state.positionState==2,"incremental XYZ reset");});
        test("first_pass_metadata_not_claimed_complete",[]{Fixture f;Assembler a;Quantum q;a.push(f.object(),f.pcm.data(),q);a.push(f.marker(),nullptr,q);a.push(f.object(1,480,2),f.pcm.data(),q);a.push(f.marker(),nullptr,q);Require(q.objects[0].state.positionState==0&&std::isnan(q.objects[0].state.audio.x),"discarded initial coordinates leaked");});
        test("sequence_gap_poisons_domain",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.object();++r.audio.sequence;Reject([&]{a.push(r,f.pcm.data(),q);});Reject([&]{a.push(f.marker(),nullptr,q);});});
        test("loss_epoch_change_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.object();r.lossEpoch=1;Reject([&]{a.push(r,f.pcm.data(),q);});});
        test("foreign_encoder_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.object();r.encoder=999;Reject([&]{a.push(r,f.pcm.data(),q);});});
        test("concurrent_submitter_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(),f.pcm.data(),q);auto r=f.object(2);r.audio.tid=456;Reject([&]{a.push(r,f.pcm.data(),q);});});
        test("failed_process_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.marker();r.result=E_FAIL;Reject([&]{a.push(r,nullptr,q);});});
        test("failed_submit_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.object();r.result=E_FAIL;Reject([&]{a.push(r,f.pcm.data(),q);});});
        test("non_S_OK_success_semantics_not_assumed",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);auto r=f.marker();r.result=S_FALSE;Reject([&]{a.push(r,nullptr,q);});});
        test("object_larger_than_process_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(),f.pcm.data(),q);Reject([&]{a.push(f.marker(240),nullptr,q);});});
        test("pending_capacity_bounded",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);for(unsigned i=0;i<64;++i)a.push(f.object(i),f.pcm.data(),q);Reject([&]{a.push(f.object(999),f.pcm.data(),q);});});
        test("trailing_unsealed_objects_reported_not_emitted",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(),f.pcm.data(),q);a.finish();Require(a.trailingObjects==1&&a.completed==0,"truncated pass emitted");});
        test("process_thread_mismatch_stops",[]{Fixture f;Assembler a;Quantum q;a.push(f.marker(),nullptr,q);a.push(f.object(),f.pcm.data(),q);auto r=f.marker();r.audio.tid=456;Reject([&]{a.push(r,nullptr,q);});});
        printf("{\"passed\":true,\"tests\":%u,\"scope\":\"synthetic Process-marker contract only\",\"live_boundary_verified\":false}\n",tests);return 0;
    } catch(const std::exception& e){printf("FAILED %s\n",e.what());return 1;}
}
