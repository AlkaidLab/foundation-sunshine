#pragma once
#include "cpp_mat_stream.hpp"
#include <condition_variable>
#include <mutex>

namespace mat_stream {
struct FakeBlock {Tick tick;std::vector<Call> calls;};
// Deterministic recorder, explicitly NOT an encoder or MAT output generator.
class FakeEncoder:public Encoder {
    const std::thread::id creator_=std::this_thread::get_id();
    std::thread::id owner_;std::vector<Call> pending_;
    void owner(){check(owner_==std::this_thread::get_id(),"encoder accessed outside worker");}
public:
    std::vector<FakeBlock> blocks;
    uint64_t domain=0;unsigned opens=0,closes=0,submits=0;
    bool completed=false,failOpen=false,failClose=false;
    int failProcess=-1,failSubmit=-1;
    std::mutex gateMutex;std::condition_variable gateCv;
    bool gate=false,entered=false;
    void waitEntered(){std::unique_lock lock(gateMutex);check(gateCv.wait_for(lock,std::chrono::seconds(3),[&]{return entered;}),"fake gate timeout");}
    void release(){std::lock_guard lock(gateMutex);gate=false;gateCv.notify_all();}
    void open(uint64_t value)override{
        check(std::this_thread::get_id()!=creator_,"encoder opened on producer thread");
        owner_=std::this_thread::get_id();domain=value;++opens;
        {std::unique_lock lock(gateMutex);entered=true;gateCv.notify_all();
            check(gateCv.wait_for(lock,std::chrono::seconds(3),[&]{return !gate;}),"fake open blocked too long");}
        check(!failOpen,"injected open failure");
    }
    void submit(const Call& call,float volume,float callGain)override{
        owner();check(volume==1.f&&callGain==1.f,"gain applied again");
        check(static_cast<int>(submits++)!=failSubmit,"injected submit failure");pending_.push_back(call);
    }
    void process(Tick tick)override{
        owner();check(static_cast<int>(blocks.size())!=failProcess,"injected Process failure");
        blocks.push_back({tick,std::move(pending_)});pending_.clear();
    }
    void close(bool success)override{
        owner();++closes;check(!failClose,"injected close failure");completed=success;pending_.clear();
    }
};
} // namespace mat_stream
