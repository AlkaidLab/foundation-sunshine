#pragma once
// Offline-testable seam only. No Windows audio/COM/vendor dependencies.
#include "cpp_bridge_native_v4.hpp"
#include "cpp_bridge_queue.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

namespace mat_stream {
using namespace cpp_bridge;
constexpr unsigned queueCapacity=8, callCapacity=64;
struct Call {CallHeader header{};std::array<float,quantum> pcm{};};
struct Packet {
    uint64_t domain{};FrameHeader header{};std::array<Call,callCapacity> calls{};
};
inline void snapshot(Packet& p,uint64_t domain,const FrameBuffer& frame){
    auto bytes=frame.bytes();check(bytes.size()>=sizeof(FrameHeader),"short bridge frame");
    p.domain=domain;std::memcpy(&p.header,bytes.data(),sizeof(FrameHeader));
    check(domain&&p.header.frames==quantum&&p.header.calls<=callCapacity&&p.header.flags<=1,"bridge frame bounds");
    check(bytes.size()==sizeof(FrameHeader)+p.header.calls*(sizeof(CallHeader)+quantum*4),"bridge frame bytes");
    auto* cursor=bytes.data()+sizeof(FrameHeader);
    for(unsigned n=0;n<p.header.calls;++n){
        auto& c=p.calls[n];std::memcpy(&c.header,cursor,sizeof(CallHeader));cursor+=sizeof(CallHeader);
        std::memcpy(c.pcm.data(),cursor,quantum*4);cursor+=quantum*4;
        check(c.header.id&&c.header.type==1&&c.header.frames==quantum,"bridge call dimensions");
        check(c.header.flags==7||c.header.flags==6||c.header.flags==8,"bridge lifecycle flags");
        for(float v:c.header.xyz)check(std::isfinite(v),"nonfinite XYZ");
        for(float v:c.pcm)check(std::isfinite(v)&&std::abs(v)<=1,"nonfinite/range PCM");
        if(c.header.flags==8)for(float v:c.pcm)check(v==0,"EOS control must be silent");
        check(!p.header.flags||c.header.flags==8,"EOF may contain only end controls");
    }
}
enum class Phase:uint32_t {preroll=1,source=2,drain=3};
struct Tick {Phase phase;uint32_t sourceIndex;};
// A future native implementation must use volume=1/callGain=1, return errors,
// and never install hooks or wait on the audiodg producer. Fake only this turn.
struct Encoder {
    virtual ~Encoder()=default;
    virtual void open(uint64_t domain)=0;
    virtual void submit(const Call& call,float volume,float callGain)=0;
    virtual void process(Tick tick)=0;
    virtual void close(bool completed)=0;
};
enum class Fault:uint32_t {none,overflow,cancelled,worker};
struct Metrics {
    uint64_t sourceRounds=0,prerollRounds=0,drainRounds=0,submissions=0,sourceSamples=0;
    uint64_t closeControls=0,sourceFrameBytes=0;
    bool completed=false;std::string error;
};
struct Options {bool paced=false;unsigned preroll=20,drain=30;};
class Pipeline {
    using Queue=boost::lockfree::spsc_queue<Packet,boost::lockfree::capacity<queueCapacity>>;
    std::unique_ptr<Queue> queue_=std::make_unique<Queue>();
    std::atomic<bool> producerDone_{false},workerDone_{false};
    std::atomic<uint64_t> wakeEpoch_{0};
    std::atomic<Fault> fault_{Fault::none};
    uint64_t domain_;Encoder& sink_;Options options_;std::thread worker_;
    unsigned high_=0;Metrics metrics_;bool joined_=false,closeQueued_=false;
    std::function<void(const Packet&)> sourceObserver_;
    void wake(){wakeEpoch_.fetch_add(1,std::memory_order_release);wakeEpoch_.notify_one();}
    void fault(Fault value){Fault expected=Fault::none;fault_.compare_exchange_strong(expected,value);}
    void run(){
        bool opened=false,closed=false;uint32_t next=0;bool warmed=false;
        std::map<uint32_t,Call> active;
        auto origin=std::chrono::steady_clock::now();uint64_t emitted=0;
        auto process=[&](Phase phase,uint32_t index){
            if(options_.paced)std::this_thread::sleep_until(origin+std::chrono::milliseconds(emitted*10));
            check(fault_.load()==Fault::none,"stream aborted before Process");
            sink_.process({phase,index});++emitted;
            if(phase==Phase::source)++metrics_.sourceRounds;
            if(phase==Phase::preroll)++metrics_.prerollRounds;
            if(phase==Phase::drain)++metrics_.drainRounds;
        };
        auto submit=[&](const Call& c){
            check(fault_.load()==Fault::none,"stream aborted before Submit");
            sink_.submit(c,1.f,1.f);++metrics_.submissions;
        };
        auto apply=[&](const Packet& p,bool firstSource=false){
            for(unsigned n=0;n<p.header.calls;++n){
                const auto& c=p.calls[n];auto id=c.header.id,flags=c.header.flags;
                if(flags==7){check(!active.contains(id)&&active.size()<20,"duplicate create/capacity");active.emplace(id,c);}
                else {check(active.contains(id),"update/end without live ID");if(flags==8)active.erase(id);else active[id]=c;}
                if(firstSource){auto update=c;update.header.flags=6;submit(update);}else submit(c);
                if(!p.header.flags&&flags!=8)metrics_.sourceSamples+=quantum;
            }
        };
        try{
            sink_.open(domain_);opened=true;origin=std::chrono::steady_clock::now();
            auto packet=std::make_unique<Packet>();
            for(;;){
                auto observedWake=wakeEpoch_.load(std::memory_order_acquire);
                check(fault_.load()==Fault::none,"producer/consumer abort");
                auto status=pollUntilDrained(*queue_,producerDone_,*packet);
                if(status==QueuePoll::finished)throw std::runtime_error("producer finished without control close");
                if(status==QueuePoll::wait){wakeEpoch_.wait(observedWake,std::memory_order_acquire);continue;}
                const auto& p=*packet;
                check(p.domain==domain_&&p.header.index==next,"domain/sequence discontinuity");
                check(p.header.frames==quantum&&p.header.calls<=callCapacity&&p.header.flags<=1,"packet bounds");
                std::set<uint32_t> seen;
                for(unsigned n=0;n<p.header.calls;++n){
                    const auto& c=p.calls[n];check(c.header.id&&c.header.type==1&&c.header.frames==quantum,"packet call bounds");
                    check(seen.insert(c.header.id).second,"duplicate ID within source round");
                    check(c.header.flags==7||c.header.flags==6||c.header.flags==8,"packet lifecycle flags");
                    for(float v:c.header.xyz)check(std::isfinite(v),"packet XYZ");
                    for(float v:c.pcm)check(std::isfinite(v)&&std::abs(v)<=1,"packet PCM");
                    if(c.header.flags==8)for(float v:c.pcm)check(v==0,"nonzero end PCM");
                    check(!p.header.flags||c.header.flags==8,"non-end EOF call");
                }
                if(p.header.flags){
                    apply(p);metrics_.closeControls=p.header.calls;
                    check(active.empty(),"EOF missing end controls");
                    // EOF submits controls only; its first Process is an explicit
                    // drain round, never a fabricated source round/sample clock.
                    for(unsigned n=0;n<options_.drain;++n)process(Phase::drain,UINT32_MAX);
                    check(fault_.load()==Fault::none,"abort at completion");
                    check(queue_->read_available()==0,"frames after EOF");
                    closed=true;sink_.close(true);metrics_.completed=true;break;
                }
                bool firstSource=false;
                if(!warmed&&p.header.calls){
                    check(active.empty(),"unexpected startup state");
                    std::set<uint32_t> ids;
                    for(unsigned n=0;n<p.header.calls;++n)check(p.calls[n].header.flags==7&&ids.insert(p.calls[n].header.id).second&&ids.size()<=20,"startup must create distinct known IDs");
                    for(unsigned n=0;n<options_.preroll;++n){
                        for(unsigned j=0;j<p.header.calls;++j){auto c=p.calls[j];c.pcm.fill(0);c.header.flags=n?6:7;submit(c);}
                        process(Phase::preroll,UINT32_MAX);
                    }
                    warmed=true;firstSource=true;
                }
                // Warmup already created the first known IDs in the codec, but
                // active[] follows original bridge lifecycle, independently.
                apply(p,firstSource);
                for(const auto& [id,call]:active){(void)call;check(seen.contains(id),"still-live object absent from source round");}
                if(sourceObserver_)sourceObserver_(p);
                metrics_.sourceFrameBytes+=sizeof(FrameHeader)+p.header.calls*(sizeof(CallHeader)+quantum*4);
                process(Phase::source,next++);
            }
        }catch(const std::exception& e){metrics_.error=e.what();fault(Fault::worker);}
        catch(...){metrics_.error="unknown worker failure";fault(Fault::worker);}
        if(opened&&!closed){try{sink_.close(false);}catch(...){if(metrics_.error.empty())metrics_.error="abort close failed";fault(Fault::worker);}}
        workerDone_.store(true,std::memory_order_release);
    }
public:
    Pipeline(uint64_t domain,Encoder& sink,Options options={},std::function<void(const Packet&)> observer={}):
        domain_(domain),sink_(sink),options_(options),sourceObserver_(std::move(observer)){
        check(domain&&queue_->is_lock_free()&&options.preroll>0&&options.preroll<=20&&options.drain>0&&options.drain<=30,"pipeline config");
        worker_=std::thread([this]{run();});
    }
    ~Pipeline(){if(!joined_){cancel();worker_.join();}}
    Pipeline(const Pipeline&)=delete;Pipeline& operator=(const Pipeline&)=delete;
    static constexpr size_t queueObjectBytes(){return sizeof(Queue);}
    // SINGLE bridge-producer API. It does not allocate/wait. Full is terminal:
    // do not overwrite, drop, or resume the same encoder epoch after a gap.
    bool tryPush(const Packet& packet){
        if(fault_.load()!=Fault::none||producerDone_.load()||closeQueued_)return false;
        if(!queue_->push(packet)){fault(Fault::overflow);producerDone_.store(true,std::memory_order_release);wake();return false;}
        high_=std::max<unsigned>(high_,queue_->read_available());
        if(packet.header.flags){closeQueued_=true;producerDone_.store(true,std::memory_order_release);}
        wake();
        return true;
    }
    // Offline adapter only. Never call this waiting helper on an RT hook.
    bool pushOffline(const Packet& packet,std::chrono::milliseconds timeout=std::chrono::seconds(5)){
        auto until=std::chrono::steady_clock::now()+timeout;
        while(!queue_->write_available()){
            if(fault_.load()!=Fault::none||workerDone_.load())return false;
            if(std::chrono::steady_clock::now()>=until){cancel();return false;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return tryPush(packet);
    }
    void cancel(){fault(Fault::cancelled);producerDone_.store(true,std::memory_order_release);wake();}
    Metrics join(){check(!joined_,"already joined");worker_.join();joined_=true;return metrics_;}
    Fault failure()const{return fault_.load();}
    unsigned highWater()const{check(joined_,"metrics before join");return high_;}
    bool done()const{return workerDone_.load(std::memory_order_acquire);}
};
} // namespace mat_stream
