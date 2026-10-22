#include "cpp_mat_stream_fake.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace mat_stream;
namespace {
std::unique_ptr<Packet> packet(uint32_t index,uint32_t flags=7,bool eof=false,uint64_t domain=1){
    auto p=std::make_unique<Packet>();p->domain=domain;p->header={index,480,1,eof?1u:0u,0,0};
    p->calls[0].header={1,0,1,flags,{.25f,.5f,.75f},480};p->calls[0].pcm.fill(flags==8?0.f:.125f);return p;
}
std::unique_ptr<Packet> makeEOF(uint32_t index,uint64_t domain=1){return packet(index,8,true,domain);}
Options fast(){return {false,2,2};}
void waitDone(Pipeline& p){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!p.done()){check(std::chrono::steady_clock::now()<until,"test worker timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
}
int runMatStreamTests(){
    std::vector<std::string> passed;
    auto test=[&](const char* name,auto fn){fn();passed.emplace_back(name);};
    test("owned_snapshot_and_queued_PCM_XYZ_not_alias",[]{
        FakeEncoder sink;sink.gate=true;Pipeline pipe(1,sink,fast());sink.waitEntered();
        FrameBuffer f;f.begin(0,1,2);std::array<float,480> pcm;pcm.fill(.125f);f.call(1,0,1,7,std::array<float,3>{1,2,3},pcm);
        auto p=std::make_unique<Packet>();snapshot(*p,1,f);check(pipe.tryPush(*p),"push");
        f.begin(1,3,4);p->calls[0].pcm.fill(.9f);p->calls[0].header.xyz[0]=99;
        auto end=makeEOF(1);check(pipe.tryPush(*end),"close push");sink.release();auto m=pipe.join();
        check(m.completed&&sink.blocks.size()==5&&sink.blocks[2].calls[0].pcm[0]==.125f&&sink.blocks[2].calls[0].header.xyz[0]==1,"snapshot overwritten");
        check(sink.blocks[0].calls[0].pcm[0]==0&&sink.blocks[0].calls[0].header.flags==7&&sink.blocks[1].calls[0].header.flags==6&&sink.blocks[2].calls[0].header.flags==6,"preroll/source transform");
        check(sink.blocks[3].tick.phase==Phase::drain&&sink.blocks[3].calls[0].header.flags==8&&m.sourceRounds==1&&m.closeControls==1,"EOF became source audio");
    });
    test("exact_capacity_8_overflow_aborts_epoch_no_silent_resume",[]{
        FakeEncoder sink;sink.gate=true;Pipeline pipe(1,sink,fast());sink.waitEntered();
        auto p=packet(0);for(unsigned n=0;n<8;++n){p->header.index=n;check(pipe.tryPush(*p),"capacity smaller than eight");}
        p->header.index=8;check(!pipe.tryPush(*p)&&pipe.failure()==Fault::overflow,"full not terminal");
        sink.release();auto m=pipe.join();check(!m.completed&&!sink.completed&&sink.blocks.empty()&&!pipe.tryPush(*p)&&pipe.highWater()==8,"overflow resumed");
    });
    test("20_objects_create_update_EOS_next_round_and_control_only_EOF",[]{
        FakeEncoder sink;Pipeline pipe(1,sink,fast());auto p=packet(0);p->header.calls=20;
        for(unsigned j=0;j<20;++j){p->calls[j]=p->calls[0];p->calls[j].header.id=j+1;p->calls[j].pcm.fill((j+1)*.001f);}
        check(pipe.pushOffline(*p),"create");p->header.index=1;for(unsigned j=0;j<20;++j)p->calls[j].header.flags=6;check(pipe.pushOffline(*p),"update");
        p->header.index=2;for(unsigned j=0;j<20;++j){p->calls[j].header.flags=8;p->calls[j].pcm.fill(0);}check(pipe.pushOffline(*p),"end");
        auto eof=makeEOF(3);eof->header.calls=0;check(pipe.pushOffline(*eof),"EOF");auto m=pipe.join();
        check(m.completed&&m.sourceRounds==3&&m.sourceSamples==19200&&m.prerollRounds==2&&m.drainRounds==2&&sink.blocks.size()==7,"20 object accounting");
    });
    test("independent_domains_and_generation_reuse",[]{
        FakeEncoder a,b;Pipeline x(1,a,fast()),y(2,b,fast());auto p=packet(0),q=packet(0,7,false,2);q->calls[0].pcm.fill(.25f);
        check(x.pushOffline(*p)&&y.pushOffline(*q),"domains create");p->header.index=1;p->header.calls=2;p->calls[1]=p->calls[0];p->calls[0].header.flags=8;p->calls[0].pcm.fill(0);p->calls[1].header.id=2;
        check(x.pushOffline(*p),"generation retire/create");auto e=makeEOF(2);e->calls[0].header.id=2;auto f=makeEOF(1,2);check(x.pushOffline(*e)&&y.pushOffline(*f),"close domains");
        check(x.join().completed&&y.join().completed&&a.domain==1&&b.domain==2&&a.blocks[2].calls[0].pcm[0]==.125f&&b.blocks[2].calls[0].pcm[0]==.25f,"domain state leak");
    });
    test("slow_consumer_offline_backpressure_drains_last_push",[]{
        FakeEncoder sink;Pipeline pipe(1,sink,{true,1,1});auto p=packet(0);
        for(unsigned n=0;n<16;++n){p->header.index=n;p->calls[0].header.flags=n?6:7;check(pipe.pushOffline(*p),"offline push");}
        auto eof=makeEOF(16);check(pipe.pushOffline(*eof),"last close");auto m=pipe.join();check(m.completed&&m.sourceRounds==16&&pipe.failure()==Fault::none,"final push lost");
    });
    test("cancelled_waiting_consumer_exits_without_success",[]{FakeEncoder sink;Pipeline pipe(1,sink,fast());pipe.cancel();auto m=pipe.join();check(!m.completed&&pipe.failure()==Fault::cancelled,"cancel");});
    test("offline_timeout_is_terminal_not_unbounded_wait",[]{FakeEncoder sink;sink.gate=true;Pipeline pipe(1,sink,fast());sink.waitEntered();auto p=packet(0);for(unsigned n=0;n<8;++n){p->header.index=n;check(pipe.tryPush(*p),"fill");}check(!pipe.pushOffline(*p,std::chrono::milliseconds(5)),"timeout");sink.release();check(!pipe.join().completed&&pipe.failure()==Fault::cancelled,"timeout result");});
    for(const auto& mode:std::vector<std::string>{"sequence_gap","wrong_domain","duplicate_create","unknown_update","nonfinite_PCM","nonfinite_XYZ","oversized_packet","EOF_missing_ends"}){
        test(mode.c_str(),[&]{FakeEncoder sink;Pipeline pipe(1,sink,fast());auto p=packet(0);
            if(mode=="sequence_gap")p->header.index=1;
            if(mode=="wrong_domain")p->domain=2;
            if(mode=="duplicate_create"){p->header.calls=2;p->calls[1]=p->calls[0];}
            if(mode=="unknown_update")p->calls[0].header.flags=6;
            if(mode=="nonfinite_PCM")p->calls[0].pcm[0]=cpp_bridge::nan();
            if(mode=="nonfinite_XYZ")p->calls[0].header.xyz[0]=cpp_bridge::nan();
            if(mode=="oversized_packet")p->header.calls=65;
            check(pipe.pushOffline(*p),"bad input enqueue");
            if(mode=="EOF_missing_ends"){auto e=makeEOF(1);e->header.calls=0;check(pipe.pushOffline(*e),"bad EOF");}
            waitDone(pipe);auto m=pipe.join();check(!m.completed&&pipe.failure()==Fault::worker&&!sink.completed,"invalid stream accepted");});
    }
    for(unsigned which=0;which<4;++which){test(("encoder_failure_stage_"+std::to_string(which)).c_str(),[&]{
        FakeEncoder sink;if(which==0)sink.failOpen=true;if(which==1)sink.failSubmit=0;if(which==2)sink.failProcess=0;if(which==3)sink.failClose=true;
        Pipeline pipe(1,sink,fast());auto p=packet(0),e=makeEOF(1);pipe.pushOffline(*p);pipe.pushOffline(*e);waitDone(pipe);auto m=pipe.join();check(!m.completed&&pipe.failure()==Fault::worker,"sink failure hidden");
    });}
    test("empty_domain_preserves_Process_and_no_fake_object",[]{FakeEncoder sink;Pipeline pipe(1,sink,fast());auto p=packet(0);p->header.calls=0;check(pipe.pushOffline(*p),"empty pass");auto e=makeEOF(1);e->header.calls=0;check(pipe.pushOffline(*e),"empty EOF");auto m=pipe.join();check(m.completed&&m.prerollRounds==0&&m.sourceRounds==1&&m.drainRounds==2&&m.submissions==0,"empty stream invented audio");});
    test("missing_live_object_is_discontinuity_not_inferred_silence",[]{FakeEncoder sink;Pipeline pipe(1,sink,fast());auto p=packet(0);check(pipe.pushOffline(*p),"create");p->header.index=1;p->header.calls=0;check(pipe.pushOffline(*p),"missing object frame");waitDone(pipe);check(!pipe.join().completed&&pipe.failure()==Fault::worker,"absent live object accepted");});
    test("fresh_epoch_after_failure_has_no_stale_objects_or_PCM",[]{
        {FakeEncoder bad;Pipeline pipe(1,bad,fast());auto p=packet(2);check(pipe.pushOffline(*p),"bad epoch");waitDone(pipe);check(!pipe.join().completed,"gap accepted");}
        FakeEncoder good;Pipeline pipe(1,good,fast());auto p=packet(0);p->calls[0].pcm.fill(.375f);auto e=makeEOF(1);
        check(pipe.pushOffline(*p)&&pipe.pushOffline(*e),"fresh epoch");check(pipe.join().completed&&good.blocks[2].calls[0].pcm[0]==.375f&&good.opens==1,"stale failed epoch leaked");
    });
    std::cout<<nlohmann::json({{"mat_stream_tests_passed",passed.size()},{"tests",passed},{"native_encoder_called",false}}).dump(2)<<'\n';return 0;
}
