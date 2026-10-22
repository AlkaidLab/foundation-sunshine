#include "cpp_mat_startup.hpp"
#include "cpp_mat_stream_fake.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace mat_startup;
namespace {
struct Gate {
    std::mutex mutex;std::condition_variable cv;bool open=false,entered=false;
    void wait(){std::unique_lock lock(mutex);entered=true;cv.notify_all();check(cv.wait_for(lock,std::chrono::seconds(3),[&]{return open;}),"test gate wait");}
    void ready(){std::unique_lock lock(mutex);check(cv.wait_for(lock,std::chrono::seconds(3),[&]{return entered;}),"test gate entry");}
    void release(){std::lock_guard lock(mutex);open=true;cv.notify_all();}
};
std::unique_ptr<Packet> make(unsigned index,bool end=false,uint64_t domain=1){
    auto p=std::make_unique<Packet>();p->domain=domain;p->header={index,480,1,end?1u:0u,0,0};
    p->calls[0].header={1,0,1,end?8u:(index?6u:7u),{.1f,.2f,.3f},480};p->calls[0].pcm.fill(end?0.f:.125f);return p;
}
template<class Predicate>void until(Predicate ready){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!ready()){check(std::chrono::steady_clock::now()<deadline,"test deadline");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
}
int runMatStartupTests(){
    std::vector<std::string> passed;auto test=[&](const char* name,auto fn){fn();passed.emplace_back(name);};
    test("32_ingress_slots_then_33rd_aborts_without_overwrite",[]{
        FakeEncoder sink;Gate gate;Session s(1,sink,{false,2,2},{},[&]{gate.wait();});gate.ready();auto p=make(0);
        for(unsigned n=0;n<32;++n){p->header.index=n;check(s.tryPush(*p),"reservoir capacity");}
        p->header.index=32;check(!s.tryPush(*p)&&s.failure()==Failure::startupOverflow,"33rd not terminal");gate.release();
        check(!s.join().completed&&s.highWater()==32&&!s.tryPush(*p),"overflow resumed epoch");
    });
    test("startup_32_frame_burst_preserves_order_PCM_XYZ_and_EOF",[]{
        FakeEncoder sink;Gate gate;Session s(1,sink,{false,2,2},{},[&]{gate.wait();});gate.ready();auto p=make(0);
        for(unsigned n=0;n<32;++n){p->header.index=n;p->calls[0].header.flags=n?6:7;p->calls[0].pcm.fill(.125f+n*.001f);p->calls[0].header.xyz[0]=n*.01f;check(s.tryPush(*p),"startup push");}
        p->calls[0].pcm.fill(.9f);gate.release();until([&]{return s.forwarded()>=32;});auto eof=make(32,true);check(s.tryPush(*eof),"EOF");
        auto m=s.join();check(m.completed&&m.sourceRounds==32&&m.sourceSamples==15360&&m.prerollRounds==2&&sink.blocks.size()==36,"startup counts");
        for(unsigned n=0;n<32;++n){const auto& c=sink.blocks[n+2].calls[0];check(c.pcm[0]==.125f+n*.001f&&c.header.xyz[0]==n*.01f,"startup data reordered/aliased");}
    });
    test("EOF_queued_during_startup_survives_drain",[]{
        FakeEncoder sink;Gate gate;Session s(1,sink,{false,2,2},{},[&]{gate.wait();});gate.ready();auto p=make(0),e=make(1,true);check(s.tryPush(*p)&&s.tryPush(*e),"early EOF");gate.release();
        auto m=s.join();check(m.completed&&m.sourceRounds==1&&m.drainRounds==2&&m.closeControls==1&&sink.blocks.size()==5,"EOF inserted source Process");
    });
    test("cancel_wakes_empty_pump_and_empty_encoder",[]{FakeEncoder sink;Session s(1,sink,{false,2,2});s.cancel();check(!s.join().completed&&s.failure()==Failure::cancelled,"cancel lost");});
    test("encoder_Process_failure_propagates_through_ingress",[]{FakeEncoder sink;sink.failProcess=0;Session s(1,sink,{false,2,2});auto p=make(0),e=make(1,true);s.tryPush(*p);s.tryPush(*e);until([&]{return s.finished();});check(!s.join().completed&&s.failure()!=Failure::none&&!sink.completed,"sink failure hidden");});
    test("sequence_gap_invalidates_buffered_epoch",[]{FakeEncoder sink;Session s(1,sink,{false,2,2});auto p=make(2),e=make(3,true);s.tryPush(*p);s.tryPush(*e);until([&]{return s.finished();});check(!s.join().completed&&s.failure()!=Failure::none,"gap accepted");});
    test("explicit_fresh_session_after_overflow_has_no_stale_samples",[]{
        {FakeEncoder sink;Gate gate;Session s(1,sink,{false,2,2},{},[&]{gate.wait();});gate.ready();auto p=make(0);for(unsigned n=0;n<33;++n){p->header.index=n;s.tryPush(*p);}gate.release();check(!s.join().completed,"overflow accepted");}
        FakeEncoder sink;Session s(1,sink,{false,2,2});auto p=make(0),e=make(1,true);p->calls[0].pcm.fill(.375f);check(s.tryPush(*p)&&s.tryPush(*e),"fresh session enqueue");check(s.join().completed&&sink.blocks[2].calls[0].pcm[0]==.375f,"old epoch leaked");
    });
    test("domains_keep_independent_reservoirs_and_encoders",[]{FakeEncoder a,b;Session x(1,a,{false,2,2}),y(2,b,{false,2,2});auto p=make(0),q=make(0,false,2),e=make(1,true),f=make(1,true,2);q->calls[0].pcm.fill(.25f);check(x.tryPush(*p)&&y.tryPush(*q)&&x.tryPush(*e)&&y.tryPush(*f),"domain enqueue");check(x.join().completed&&y.join().completed&&a.blocks[2].calls[0].pcm[0]==.125f&&b.blocks[2].calls[0].pcm[0]==.25f,"domain leak");});
    test("20_object_startup_snapshot_is_owned",[]{FakeEncoder sink;Session s(1,sink,{false,2,2});auto p=make(0);p->header.calls=20;for(unsigned n=0;n<20;++n){p->calls[n]=p->calls[0];p->calls[n].header.id=n+1;p->calls[n].pcm.fill((n+1)*.001f);}check(s.tryPush(*p),"20 create");p->header.index=1;p->header.flags=1;for(unsigned n=0;n<20;++n){p->calls[n].header.flags=8;p->calls[n].pcm.fill(0);}check(s.tryPush(*p),"20 EOF");check(s.join().completed&&sink.blocks[2].calls.size()==20&&sink.blocks[2].calls[19].pcm[0]==20*.001f,"20 object corruption");});
    std::cout<<nlohmann::json({{"mat_startup_tests_passed",passed.size()},{"tests",passed},{"native_encoder_called",false}}).dump(2)<<'\n';return 0;
}
