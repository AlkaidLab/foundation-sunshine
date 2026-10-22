#define NOMINMAX
#include "cpp_mat_startup.hpp"
#include "cpp_mat_stream_fake.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <windows.h>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
using namespace mat_startup;
using Json=nlohmann::json;namespace fs=std::filesystem;
int runSelftests();int runNative4Tests();int runMatStreamTests();int runMatStartupTests();
static int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
class LocalTimer {
    HANDLE timer_{};
public:
    LocalTimer(){timer_=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);check(timer_!=nullptr,"high-resolution local timer unavailable; no fallback");}
    ~LocalTimer(){CloseHandle(timer_);}
    LocalTimer(const LocalTimer&)=delete;LocalTimer& operator=(const LocalTimer&)=delete;
    void until(int64_t target){
        for(;;){auto left=target-now();if(left<=0)return;LARGE_INTEGER due{};due.QuadPart=-((left+99)/100);
            check(SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)!=0,"local timer set failed");
            check(WaitForSingleObject(timer_,1000)==WAIT_OBJECT_0,"local timer wait failed/timed out");}
    }
};
static std::string hashFile(const fs::path& path){std::ifstream f(path,std::ios::binary);check(bool(f),"hash open");AVSHA* s=av_sha_alloc();check(s&&av_sha_init(s,256)==0,"hash init");std::array<uint8_t,65536>b;while(f){f.read(reinterpret_cast<char*>(b.data()),b.size());av_sha_update(s,b.data(),static_cast<size_t>(f.gcount()));}check(f.eof(),"hash read");uint8_t d[32];av_sha_final(s,d);av_free(s);std::ostringstream out;for(auto v:d)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return out.str();}
static Json load(const fs::path& path){std::ifstream f(path);check(bool(f),"JSON open");Json j;f>>j;return j;}
static void save(const fs::path& path,const Json& j){std::ofstream f(path);f<<j.dump(2)<<'\n';check(bool(f),"JSON write");}
template<class T>static void write(std::ofstream& f,const T& value){f.write(reinterpret_cast<const char*>(&value),sizeof(value));check(bool(f),"binary write");}
static void writePacket(std::ofstream& f,const Packet& p){write(f,p.header);for(unsigned n=0;n<p.header.calls;++n){write(f,p.calls[n].header);write(f,p.calls[n].pcm);}}
struct Source {
    FileHeader header{};std::vector<uint8_t> bytes;std::vector<size_t> offsets;
    explicit Source(const fs::path& path){
        check(fs::file_size(path)<=16*1024*1024,"source size bound");std::ifstream f(path,std::ios::binary);bytes.assign(std::istreambuf_iterator<char>(f),{});
        check(bytes.size()>=sizeof(header),"source header");memcpy(&header,bytes.data(),sizeof(header));
        check(memcmp(header.magic,"MATBRG1\0",8)==0&&header.version==1&&header.rate==48000&&header.frames==480&&header.records>=2&&header.records<=1000,"source format");
        size_t at=sizeof(header);for(unsigned n=0;n<header.records;++n){check(at+sizeof(FrameHeader)<=bytes.size(),"frame bounds");FrameHeader h{};memcpy(&h,bytes.data()+at,sizeof(h));check(h.index==n&&h.frames==480&&h.calls<=64&&h.flags==unsigned(n+1==header.records),"frame sequence/EOF");offsets.push_back(at);at+=sizeof(h)+h.calls*(sizeof(CallHeader)+quantum*4);check(at<=bytes.size(),"PCM bounds");}check(at==bytes.size(),"source trailing bytes");
    }
    void copy(Packet& p,unsigned index,uint64_t domain)const{
        auto* at=bytes.data()+offsets.at(index);p.domain=domain;memcpy(&p.header,at,sizeof(p.header));at+=sizeof(p.header);
        for(unsigned n=0;n<p.header.calls;++n){memcpy(&p.calls[n].header,at,sizeof(CallHeader));at+=sizeof(CallHeader);memcpy(p.calls[n].pcm.data(),at,quantum*4);at+=quantum*4;}
    }
};
struct ProcessTime {Tick tick;int64_t target,start,finish;};
class TimedFake:public Encoder {
    unsigned delay_,stall_;
    bool clockStarted_=false;std::chrono::steady_clock::time_point next_;
    LocalTimer timer_;
public:
    FakeEncoder fake;std::vector<ProcessTime> times;unsigned clockRebases=0;
    TimedFake(unsigned delay,unsigned stall):delay_(delay),stall_(stall){}
    void open(uint64_t domain)override{if(delay_)std::this_thread::sleep_for(std::chrono::milliseconds(delay_));fake.open(domain);}
    void submit(const Call& c,float v,float g)override{fake.submit(c,v,g);}
    void process(Tick tick)override{
        auto current=std::chrono::steady_clock::now();
        if(!clockStarted_){next_=current;clockStarted_=true;}
        // Do not burst through warmup or erase backlog when the producer or a
        // Process stalls. Rebase late deadlines instead of unlimited catch-up.
        if(current-next_>std::chrono::milliseconds(2)){next_=current;++clockRebases;}
        auto target=std::chrono::duration_cast<std::chrono::nanoseconds>(next_.time_since_epoch()).count();
        timer_.until(target);auto start=now();next_+=std::chrono::milliseconds(10);
        if(stall_&&tick.phase==Phase::source&&tick.sourceIndex==60)std::this_thread::sleep_for(std::chrono::milliseconds(stall_));
        fake.process(tick);times.push_back({tick,target,start,now()});
    }
    void close(bool completed)override{fake.close(completed);}
};
static Json stats(std::vector<double> v){check(!v.empty(),"empty stats");std::sort(v.begin(),v.end());auto p=[&](double f){double ix=f*(v.size()-1);size_t lo=static_cast<size_t>(ix),hi=std::min(lo+1,v.size()-1);return v[lo]+(v[hi]-v[lo])*(ix-lo);};return {{"min",v.front()},{"p50",p(.5)},{"p99",p(.99)},{"max",v.back()}};}
static Json replay(const fs::path& root,const fs::path& out,const Json& info,bool delayed){
    uint64_t domain=info["domain"].get<uint64_t>();auto original=root/info["file"].get<std::string>();check(hashFile(original)==info["sha256"].get<std::string>(),"bridge fixture changed");Source source(original);
    auto name="domain-"+std::to_string(domain);std::ofstream exported(out/(name+"-source.bin"),std::ios::binary);write(exported,source.header);
    TimedFake encoder(delayed?60:0,delayed?40:0);Session session(domain,encoder,{false,20,30},[&](const Packet& p){writePacket(exported,p);});
    std::vector<int64_t> targets(source.header.records),arrivals(source.header.records),costs(source.header.records);
    auto packet=std::make_unique<Packet>();auto origin=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);LocalTimer producerTimer;
    unsigned rejected=0;
    for(unsigned n=0;n<source.header.records;++n){
        auto target=origin+std::chrono::milliseconds(n*10);targets[n]=std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count();producerTimer.until(targets[n]);
        source.copy(*packet,n,domain);arrivals[n]=now();
        // No space check, capacity retry, or backpressure in this producer.
        bool accepted=session.tryPush(*packet);costs[n]=now()-arrivals[n];if(!accepted){++rejected;session.cancel();break;}
    }
    auto m=session.join();check(m.completed&&rejected==0,"100Hz producer/consumer session failed");
    source.copy(*packet,source.header.records-1,domain);writePacket(exported,*packet);exported.close();check(bool(exported),"source export close");
    auto plan=out/(name+"-fake-plan.bin");std::ofstream transcript(plan,std::ios::binary);
    for(const auto& block:encoder.fake.blocks){write(transcript,static_cast<uint32_t>(block.tick.phase));write(transcript,block.tick.sourceIndex);write(transcript,static_cast<uint32_t>(block.calls.size()));for(const auto& c:block.calls){write(transcript,c.header.id);write(transcript,c.header.flags);write(transcript,c.header.xyz);write(transcript,c.pcm);}}
    transcript.close();check(bool(transcript),"transcript close");
    std::ofstream csv(out/(name+"-timing.csv"));csv<<"source_index,target_ns,ingress_ns,try_push_ns,Process_finish_ns\n";
    std::vector<double> latency,lateness,pushCost,interval;
    for(const auto& t:encoder.times)if(t.tick.phase==Phase::source){auto n=t.tick.sourceIndex;check(n+1<source.header.records,"Process at EOF");csv<<n<<','<<targets[n]<<','<<arrivals[n]<<','<<costs[n]<<','<<t.finish<<'\n';latency.push_back((t.finish-arrivals[n])/1e6);lateness.push_back((arrivals[n]-targets[n])/1e6);pushCost.push_back(costs[n]/1e6);if(n)interval.push_back((arrivals[n]-arrivals[n-1])/1e6);}
    csv.close();check(bool(csv),"timing write");
    std::ofstream processCsv(out/(name+"-process-clock.csv"));processCsv<<"phase,source_index,target_ns,start_ns,finish_ns\n";
    std::vector<double> processIntervals;int64_t previous=0;
    for(const auto& t:encoder.times){processCsv<<static_cast<unsigned>(t.tick.phase)<<','<<t.tick.sourceIndex<<','<<t.target<<','<<t.start<<','<<t.finish<<'\n';if(previous)processIntervals.push_back((t.start-previous)/1e6);previous=t.start;}
    processCsv.close();check(bool(processCsv),"Process clock write");
    bool identical=hashFile(out/(name+"-source.bin"))==info["sha256"].get<std::string>();
    return {{"domain",domain},{"completed",m.completed},{"fault",static_cast<unsigned>(session.failure())},{"error",session.error()},
        {"source_process_rounds",m.sourceRounds},{"source_samples",m.sourceSamples},{"preroll_rounds",m.prerollRounds},{"drain_rounds",m.drainRounds},{"submissions",m.submissions},
        {"scheduled_rounds",encoder.fake.blocks.size()},{"EOF_close_controls",m.closeControls},{"encoder_opens",encoder.fake.opens},{"encoder_closes",encoder.fake.closes},{"encoder_completed",encoder.fake.completed},
        {"queue_capacity",8},{"queue_high_water",session.encoderHighWater()},{"startup_capacity",32},{"startup_high_water",session.highWater()},
        {"startup_queue_object_bytes",Session::queueObjectBytes()},{"encoder_queue_object_bytes",Pipeline::queueObjectBytes()},{"snapshot_bytes",sizeof(Packet)},
        {"producer_capacity_retries",0},{"producer_rejected",rejected},{"producer_backpressure",false},
        {"simulated_open_delay_ms",delayed?60:0},{"simulated_single_source_Process_stall_ms",delayed?40:0},
        {"process_clock_rebases",encoder.clockRebases},{"process_start_interval_ms",stats(processIntervals)},
        {"source_ingress_to_fake_Process_finish_ms",stats(latency)},{"producer_start_lateness_ms",stats(lateness)},{"try_push_wall_ms",stats(pushCost)},{"producer_interval_ms",stats(interval)},
        {"source_file",name+"-source.bin"},{"source_sha256",hashFile(out/(name+"-source.bin"))},{"source_export_matches_frozen_bridge",identical},
        {"fake_plan_file",name+"-fake-plan.bin"},{"fake_plan_sha256",hashFile(plan)}};
}
int main(int argc,char** argv){
    fs::path out;bool created=false;
    try{
        check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg ABI mismatch");
        if(argc==2&&std::string(argv[1])=="--selftest"){check(!runSelftests()&&!runNative4Tests()&&!runMatStreamTests(),"frozen regressions");return runMatStartupTests();}
        check(argc==3||argc==4,"Usage: mat-startup-replay BRIDGE_EXPORT NEW_OUTPUT [--delayed]");bool delayed=argc==4;check(!delayed||std::string(argv[3])=="--delayed","unknown option");
        auto root=fs::absolute(argv[1]);auto golden=load(root/"summary.json");check(golden["offline_bridge_replay_passed"]==true&&golden["domains"].size()==2,"frozen bridge required");
        out=fs::absolute(argv[2]);check(!fs::exists(out)&&fs::create_directories(out),"new output required");created=true;Json domains=Json::array();bool passed=true;
        for(const auto& info:golden["domains"]){auto d=replay(root,out,info,delayed);passed=passed&&d["source_export_matches_frozen_bridge"].get<bool>();domains.push_back(d);}
        Json result={{"offline_threaded_fake_passed",passed},{"paced_worker",true},{"producer_paced_100Hz",true},{"producer_backpressure",false},{"domains",domains},
            {"bridge_oracle_sha256",hashFile(root/"summary.json")},{"source_unchanged",true},{"native_encoder_called",false},{"mat_encoded",false},{"live_capture_verified",false},{"production_default_changed",false},
            {"policy","Fixed 32-slot reservoir + 8-slot codec queue + bounded in-flight packets; source capacity never waited/retried; overflow invalidates epoch; startup backlog persists as delay."},
            {"clock","Per-thread high-resolution waitable timers, rechecked monotonic deadlines; no timeBeginPeriod/global timer changes"},
            {"limitations",{"Paced recorded bridge output + fake sink, not an actual real-time audio callback or native MAT codec.","No zero-latency claim: 200ms pre-roll creates inventory; queue reservoir is not reclaimed at equal source/codec rates.","Fake clock rebases when over 2ms late rather than unlimited catch-up; no hard 10ms deadline claim.","Source preloaded and fake transcripts retained; queue memory bounded but this executable is not a resident collector."}}};
        save(out/"summary.json",result);std::cout<<result.dump(2)<<'\n';return passed?0:1;
    }catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';if(created)try{save(out/"failure.json",{{"error",e.what()},{"native_encoder_called",false}});}catch(...){}return 1;}
}
