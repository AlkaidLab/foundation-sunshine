#include "cpp_bridge_core.hpp"
#include "cpp_bridge_queue.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>
#include <thread>
using namespace cpp_bridge;
namespace {
struct Scene {
    PositionCache source;uint64_t sequence=0;
    RecordInput make(uint64_t host=7,uint32_t obj=42,uint32_t flags=6,Position xyz=std::array<float,3>{1,2,3},float sample=.1f,uint64_t loss=0){
        Record raw{};raw.magic=0x474f424a;raw.frames=quantum;raw.host=host;raw.qpc=1000000+sequence*1000;raw.objectId=obj;raw.type=1;
        raw.x=raw.y=raw.z=nan();if(xyz){raw.x=(*xyz)[0];raw.y=(*xyz)[1];raw.z=(*xyz)[2];}raw.volume=.5f;raw.callGain=.25f;raw.tid=1;raw.sequence=sequence++;
        RecordInput out{};out.state=source.Apply(raw,flags,loss);out.pcm.fill(sample);return out;
    }
};
std::unique_ptr<Batch> batch(unsigned index,std::initializer_list<RecordInput> records){
    auto result=std::make_unique<Batch>();result->index=index;result->count=records.size();std::copy(records.begin(),records.end(),result->records.begin());return result;
}
std::vector<CallHeader> headers(const FrameBuffer& frame){
    auto bytes=frame.bytes();FrameHeader h{};memcpy(&h,bytes.data(),sizeof(h));size_t offset=sizeof(h);std::vector<CallHeader> result;
    for(unsigned i=0;i<h.calls;++i){CallHeader call{};check(offset+sizeof(call)<=bytes.size(),"test call header");memcpy(&call,bytes.data()+offset,sizeof(call));offset+=sizeof(call)+call.frames*4;result.push_back(call);}
    check(offset==bytes.size(),"test call length");return result;
}
template<class Fn>void rejected(Fn fn){bool threw=false;try{fn();}catch(const std::exception&){threw=true;}check(threw,"expected rejection");}
}
int runSelftests(){
    std::vector<std::string> passed;
    auto test=[&](const char* name,auto fn){fn();passed.push_back(name);};
    test("ffmpeg_variable_chunks_pcm_exact",[]{
        Framer framer;std::vector<float> input(1440),output;for(size_t i=0;i<input.size();++i)input[i]=static_cast<float>(i);
        size_t offset=0;for(unsigned n:{113,367,960}){for(auto& f:framer.push(std::span(input).subspan(offset,n),std::array<float,3>{1,2,3})){check(f.singlePositionReady(),"same metadata not preserved");output.insert(output.end(),f.pcm.begin(),f.pcm.end());}offset+=n;}
        framer.finish();check(input==output && framer.maxBuffered<=Framer::capacity,"PCM reassembly");
    });
    test("metadata_change_inside_480_remains_piecewise",[]{Framer f;std::array<float,257> a{};std::array<float,223>b{};f.push(a,std::array<float,3>{1,0,0});auto out=f.push(b,std::array<float,3>{2,0,0});check(out.size()==1 && !out[0].singlePositionReady() && out[0].spans[1].offset==257,"metadata flattened");f.finish();});
    test("partial_tail_rejected",[]{Framer f;std::array<float,17> a{};f.push(a,std::array<float,3>{0,0,0});rejected([&]{f.finish();});});
    test("oversize_input_rejected_without_fifo_growth",[]{Framer f;std::vector<float>a(max_source_frames+1);rejected([&]{f.push(a,std::nullopt);});});
    test("known_nonfinite_metadata_rejected",[]{Framer f;std::array<float,480>a{};rejected([&]{f.push(a,std::array<float,3>{nan(),0,0});});});
    test("cached_xyz_and_f32_gain_once",[]{
        Scene s;Bridge b;b.consume(*batch(0,{s.make()}));auto& out=const_cast<Output&>(b.consume(*batch(1,{s.make(7,42,2)})));auto h=headers(out.dynamic);
        check(h.size()==1 && h[0].id==1 && h[0].flags==6 && h[0].xyz[0]==1,"cached metadata");float pcm{};auto bytes=out.dynamic.bytes();memcpy(&pcm,bytes.data()+sizeof(FrameHeader)+sizeof(CallHeader),4);check(pcm==.0125f,"double/missing gain");
    });
    test("unknown_silent_deferred_not_origin",[]{Scene s;Bridge b;auto& out=const_cast<Output&>(b.consume(*batch(0,{s.make(7,42,2,std::nullopt,0)})));check(headers(out.dynamic).empty() && b.counts["deferred_unknown_silent_records"]==1,"unknown origin fabricated");});
    test("unknown_audible_quarantined_explicitly",[]{Scene s;Bridge b;auto& out=const_cast<Output&>(b.consume(*batch(0,{s.make(7,42,2,std::nullopt)})));check(headers(out.dynamic).empty() && b.counts["quarantined_unknown_audible_records"]==1,"audible unknown not reported");});
    test("default_origin_only_real_create",[]{Scene s;Bridge b;auto& out=const_cast<Output&>(b.consume(*batch(0,{s.make(7,42,1,std::nullopt)})));auto h=headers(out.dynamic);check(h.size()==1 && h[0].xyz[0]==0 && h[0].xyz[1]==0 && h[0].xyz[2]==0,"create origin");});
    test("call_reorder_preserves_native_ids_and_source_slots",[]{
        Scene s;Bridge b;b.consume(*batch(0,{s.make(7,42),s.make(7,43)}));auto& out=const_cast<Output&>(b.consume(*batch(1,{s.make(7,43,2),s.make(7,42,2)})));auto h=headers(out.dynamic);
        check(h.size()==2 && h[0].id==2 && h[0].slot==1 && h[1].id==1 && h[1].slot==0,"reorder rebound identity");
    });
    test("same_id_different_host_isolated",[]{Scene s;Bridge b;auto& out=const_cast<Output&>(b.consume(*batch(0,{s.make(),s.make(8)})));auto h=headers(out.dynamic);check(h.size()==2 && h[0].id!=h[1].id,"host collision");});
    test("source_end_preserves_pcm_then_new_generation",[]{
        Scene s;Bridge b;b.consume(*batch(0,{s.make()}));auto& last=const_cast<Output&>(b.consume(*batch(1,{s.make(7,42,8)})));check(headers(last.dynamic)[0].flags==6,"last source PCM ended early");
        auto& next=const_cast<Output&>(b.consume(*batch(2,{s.make(7,42,7)})));auto h=headers(next.dynamic);check(h.size()==2 && h[0].id==1 && h[0].flags==8 && h[1].id==2 && h[1].flags==7,"generation reuse");
    });
    test("new_generation_unknown_retires_old_cached_object",[]{
        Scene s;Bridge b;b.consume(*batch(0,{s.make()}));auto& out=const_cast<Output&>(b.consume(*batch(1,{s.make(7,42,7,std::nullopt,0)})));auto h=headers(out.dynamic);check(h.size()==1 && h[0].flags==8,"stale old generation retained");
    });
    test("loss_epoch_ends_all_cached_objects",[]{
        Scene s;Bridge b;b.consume(*batch(0,{s.make(7),s.make(8)}));auto& out=const_cast<Output&>(b.consume(*batch(1,{s.make(7,42,2,std::nullopt,0,1)})));auto h=headers(out.dynamic);check(h.size()==2 && h[0].flags==8 && h[1].flags==8,"loss did not invalidate");
    });
    test("corrupted_stored_snapshot_rejected",[]{Scene s;Bridge b;b.consume(*batch(0,{s.make()}));auto bad=s.make(7,42,2);bad.state.audio.x=0;rejected([&]{b.consume(*batch(1,{bad}));});});
    test("more_than_twenty_objects_rejected",[]{
        Scene s;Bridge b;auto first=std::make_unique<Batch>();first->count=20;for(unsigned i=0;i<20;++i)first->records[i]=s.make(7,100+i);b.consume(*first);
        rejected([&]{b.consume(*batch(1,{s.make(7,999)}));});
    });
    test("eof_end_marked_and_no_restart",[]{Scene s;Bridge b;b.consume(*batch(0,{s.make()}));auto& end=b.finish();FrameHeader h{};auto bytes=end.bytes();memcpy(&h,bytes.data(),sizeof(h));check(h.flags==1 && h.firstQpc==-1 && headers(end)[0].flags==8,"EOF mistaken for source event");rejected([&]{b.consume(*batch(1,{s.make()}));});});
    test("boost_queue_full_is_nonblocking_no_overwrite",[]{boost::lockfree::spsc_queue<unsigned,boost::lockfree::capacity<8>> q;check(q.is_lock_free(),"not lock-free");for(unsigned i=0;i<8;++i)check(q.push(i),"early full");check(!q.push(99),"overwrote full queue");for(unsigned i=0;i<8;++i){unsigned out;check(q.pop(out)&&out==i,"queue order");}unsigned out;check(!q.pop(out),"empty queue pop");});
    test("boost_spsc_threaded_order_stress",[]{
        boost::lockfree::spsc_queue<unsigned,boost::lockfree::capacity<8>> q;bool correct=true;
        std::thread reader([&]{for(unsigned i=0;i<10000;++i){unsigned out;while(!q.pop(out))std::this_thread::yield();if(out!=i)correct=false;}});
        for(unsigned i=0;i<10000;++i)while(!q.push(i))std::this_thread::yield();reader.join();check(correct,"concurrent SPSC order");
    });
    test("stop_racing_final_push_still_drains",[]{
        struct RacingQueue {
            std::atomic<bool>& done;unsigned pops=0;
            bool pop(unsigned& value){if(++pops==1){done.store(true,std::memory_order_release);return false;}if(pops==2){value=42;return true;}return false;}
        };
        std::atomic<bool> done=false;RacingQueue queue{done};unsigned value=0;
        check(pollUntilDrained(queue,done,value)==QueuePoll::item && value==42,"last enqueued item lost on stop");
        check(pollUntilDrained(queue,done,value)==QueuePoll::finished,"drained queue did not finish");
    });
    test("bounded_pool_backpressure_no_overwrite_and_drain",[]{
        boost::lockfree::spsc_queue<unsigned,boost::lockfree::capacity<8>> ready;
        boost::lockfree::spsc_queue<unsigned,boost::lockfree::capacity<9>> free;
        std::array<unsigned,9> pool{};std::atomic<bool> done=false;unsigned slot=0;
        for(unsigned i=0;i<9;++i)check(free.push(i),"pool init");
        for(unsigned i=0;i<8;++i){check(free.pop(slot),"free slot");pool[slot]=100+i;check(ready.push(slot),"ready early full");}
        check(free.pop(slot),"ninth pool slot");pool[slot]=999;check(!ready.push(slot),"backpressure overwritten queue");
        check(free.push(slot),"return unsent slot");done.store(true,std::memory_order_release);
        for(unsigned i=0;i<8;++i){check(pollUntilDrained(ready,done,slot)==QueuePoll::item && pool[slot]==100+i,"pending PCM corrupted");check(free.push(slot),"return processed slot");}
        check(pollUntilDrained(ready,done,slot)==QueuePoll::finished && free.read_available()==9,"pool not recovered after explicit abort");
    });
    std::cout<<nlohmann::json({{"selftests_passed",passed.size()},{"tests",passed},{"encoder_called",false}}).dump(2)<<'\n';return 0;
}
