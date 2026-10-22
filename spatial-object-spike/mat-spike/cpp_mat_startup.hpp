#pragma once
#include "cpp_mat_stream.hpp"

namespace mat_startup {
using namespace mat_stream;
constexpr unsigned startupCapacity=32;
enum class Failure:uint32_t {none,startupOverflow,encoder,cancelled,pump};
// Experimental offline-validated ingress. Fixed reservoir -> dedicated pump ->
// existing 8-slot encoder worker. Reservoir persists: 1:1 source/codec clocks
// cannot erase startup latency. No promise of zero-delay or real RT hook safety.
class Session {
    using Queue=boost::lockfree::spsc_queue<Packet,boost::lockfree::capacity<startupCapacity>>;
    std::unique_ptr<Queue> queue_=std::make_unique<Queue>();
    std::unique_ptr<Pipeline> encoder_;
    std::thread pump_;
    std::atomic<bool> done_{false},finished_{false};
    std::atomic<uint64_t> wakeEpoch_{0},forwarded_{0};
    std::atomic<Failure> failure_{Failure::none};
    unsigned high_=0;bool closeQueued_=false,joined_=false;
    Metrics result_;std::string error_;
    void wake(){wakeEpoch_.fetch_add(1,std::memory_order_release);wakeEpoch_.notify_one();}
    void fail(Failure why){Failure expected=Failure::none;failure_.compare_exchange_strong(expected,why);done_.store(true,std::memory_order_release);encoder_->cancel();wake();}
    void pump(std::function<void()> before){
        try{
            if(before)before();
            auto packet=std::make_unique<Packet>();
            for(;;){
                auto epoch=wakeEpoch_.load(std::memory_order_acquire);
                check(failure_.load()==Failure::none,"startup ingress aborted");
                auto state=pollUntilDrained(*queue_,done_,*packet);
                check(state!=QueuePoll::finished,"ingress ended without EOF");
                if(state==QueuePoll::wait){wakeEpoch_.wait(epoch,std::memory_order_acquire);continue;}
                // Only the dedicated non-RT pump may wait for encoder space.
                // This is the existing bounded test adapter, not a hook API.
                check(encoder_->pushOffline(*packet),"encoder queue unavailable/cancelled");
                forwarded_.fetch_add(1,std::memory_order_release);
                if(packet->header.flags){
                    result_=encoder_->join();
                    if(!result_.completed){Failure expected=Failure::none;failure_.compare_exchange_strong(expected,Failure::encoder);}
                    finished_.store(true,std::memory_order_release);return;
                }
            }
        }catch(const std::exception& e){error_=e.what();fail(Failure::pump);}
        catch(...){error_="unknown pump failure";fail(Failure::pump);}
        result_=encoder_->join();finished_.store(true,std::memory_order_release);
    }
public:
    Session(uint64_t domain,Encoder& sink,Options options={true,20,30},
            std::function<void(const Packet&)> observer={},std::function<void()> beforePump={}){
        check(queue_->is_lock_free(),"startup SPSC not lock-free");
        encoder_=std::make_unique<Pipeline>(domain,sink,options,std::move(observer));
        try{pump_=std::thread([this,before=std::move(beforePump)]{pump(before);});}
        catch(...){encoder_->cancel();encoder_->join();throw;}
    }
    ~Session(){if(!joined_){cancel();pump_.join();}}
    Session(const Session&)=delete;Session& operator=(const Session&)=delete;
    // SINGLE bridge producer only: no capacity wait, no retry, no heap allocation.
    // Copy+atomic notification is not a platform-certified hard-RT primitive.
    bool tryPush(const Packet& packet){
        if(failure_.load()!=Failure::none||closeQueued_||finished_.load())return false;
        if(encoder_->failure()!=Fault::none){fail(Failure::encoder);return false;}
        if(!queue_->push(packet)){fail(Failure::startupOverflow);return false;}
        high_=std::max<unsigned>(high_,queue_->read_available());
        if(packet.header.flags){closeQueued_=true;done_.store(true,std::memory_order_release);}
        wake();return true;
    }
    void cancel(){fail(Failure::cancelled);}
    Metrics join(){check(!joined_,"session already joined");pump_.join();joined_=true;auto result=result_;if(failure_.load()!=Failure::none)result.completed=false;return result;}
    bool finished()const{return finished_.load(std::memory_order_acquire);}
    Failure failure()const{return failure_.load();}
    uint64_t forwarded()const{return forwarded_.load(std::memory_order_acquire);}
    unsigned highWater()const{check(joined_,"startup metrics before join");return high_;}
    unsigned encoderHighWater()const{check(joined_,"encoder metrics before join");return encoder_->highWater();}
    Fault encoderFailure()const{return encoder_->failure();}
    const std::string& error()const{check(joined_,"error before join");return error_;}
    static constexpr size_t queueObjectBytes(){return sizeof(Queue);}
};
} // namespace mat_startup
