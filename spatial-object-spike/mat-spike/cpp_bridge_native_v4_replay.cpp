#define NOMINMAX
#include <windows.h>
#include "cpp_bridge_native_v4.hpp"
#include "cpp_bridge_queue.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/version.hpp>
#if BOOST_VERSION != 108500
#error Native v4 offline replay is pinned to the existing complete UCRT Boost 1.85 headers
#endif
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <thread>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
using namespace cpp_bridge_native4;
namespace fs=std::filesystem;
using Json=nlohmann::json;
int runSelftests();int runNative4Tests();
class Sha {
    AVSHA* p_{};
public:
    Sha(){p_=av_sha_alloc();check(p_&&av_sha_init(p_,256)==0,"SHA init");}
    ~Sha(){av_free(p_);}Sha(const Sha&)=delete;
    void add(std::span<const uint8_t> b){av_sha_update(p_,b.data(),b.size());}
    std::string finish(){uint8_t b[32];av_sha_final(p_,b);std::ostringstream s;for(auto v:b)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();}
};
static std::string hashFile(const fs::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"hash open");Sha s;std::array<uint8_t,65536>b;while(f){f.read(reinterpret_cast<char*>(b.data()),b.size());s.add({b.data(),static_cast<size_t>(f.gcount())});}check(f.eof(),"hash read");return s.finish();}
static Json readJson(const fs::path& p){std::ifstream f(p);check(bool(f),"JSON open");Json j;f>>j;return j;}
static void writeJson(const fs::path& p,const Json& j){std::ofstream f(p);check(bool(f),"JSON output");f<<j.dump(2)<<'\n';check(bool(f),"JSON write");}
static int64_t now(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
static int64_t hz(){LARGE_INTEGER v;QueryPerformanceFrequency(&v);return v.QuadPart;}
struct Item {q4::Header h{};q4::Quantum q;int64_t first=0;};
struct Source {
    Json footer,manifest=Json::array();std::vector<fs::path> files;std::vector<Item> items;
    std::map<uint64_t,uint64_t> passes;uint64_t frequency=0,packets=0,samples=0;
    Source(const fs::path& root,const fs::path& proofPath){
        auto format=readJson(root/"format.json");footer=readJson(root/"summary.json");auto proof=readJson(proofPath);
        check(format["version"]==4&&format["header_bytes"]==160&&format["source_kind"]=="native-pre-submit-paired"&&format["pcm_stage"]=="before-original-submit","native v4 required");
        check(footer["status"]==2&&footer["original_bytes_restored"]==true,"native stop/restoration");
        for(auto key:{"error","dropped_full","dropped_busy","invalid","extended_calls","unbound_after_arming"})check(footer[key]==0,"native source integrity");
        check(proof["complete_encoder_input_capture_contract_passed"]==true&&proof["first_block_coordinates_native_verified"]==true,"separate source/gain proof required");
        frequency=footer.at("qpc_frequency").get<uint64_t>();check(frequency>0,"QPC frequency");
        files.push_back(root/"global-objects.bin");std::map<unsigned,fs::path> numbered;std::regex pattern("global-objects-([0-9]+)[.]bin");
        for(auto& e:fs::directory_iterator(root)){std::smatch match;auto name=e.path().filename().string();if(e.is_regular_file()&&std::regex_match(name,match,pattern))check(numbered.emplace(std::stoul(match[1]),e.path()).second,"duplicate chunk");}
        unsigned next=1;for(auto& [n,p]:numbered){check(n==next++,"missing chunk");files.push_back(p);}
        uint64_t bytes=0;for(auto& p:files){auto size=fs::file_size(p);bytes+=size;manifest.push_back({{"file",p.filename().string()},{"bytes",size},{"sha256",hashFile(p)}});}
        check(manifest==proof["source_files"]&&bytes==footer["bytes"].get<uint64_t>(),"content proof/source hash mismatch");
        q4::Assembler parser;uint64_t records=0;std::map<uint64_t,int64_t> first;
        for(auto& p:files){std::ifstream f(p,std::ios::binary);check(bool(f),"source open");for(;;){q4::Header h{};f.read(reinterpret_cast<char*>(&h),sizeof(h));auto got=f.gcount();if(!got){check(f.eof(),"source I/O");break;}
            check(got==sizeof(h)&&h.audio.frames<=kSamples,"source header length");std::array<float,kSamples> pcm{};f.read(reinterpret_cast<char*>(pcm.data()),h.audio.frames*4);check(f.gcount()==h.audio.frames*4,"source PCM length");++records;
            if(h.kind==q4::ObjectBefore){++packets;samples+=h.audio.frames;}
            if(h.kind==q4::PassBegin)first[h.domain]=h.audio.qpc;
            q4::Quantum q;bool sealed=parser.push(h,pcm.data(),q);
            if(h.kind==q4::DomainBegin||h.kind==q4::DomainEnd){Item item;item.h=h;item.first=h.audio.qpc;items.push_back(std::move(item));}
            if(sealed){Item item;item.h=h;item.first=first.at(h.domain);item.q=std::move(q);++passes[h.domain];items.push_back(std::move(item));}
        }}
        parser.finish();check(records==footer["blocks"].get<uint64_t>()&&samples==footer["frames"].get<uint64_t>()&&parser.results==footer["submit_results"].get<uint64_t>()&&parser.markers==footer["process_results"].get<uint64_t>()&&parser.opened==footer["domains_opened"].get<uint64_t>()&&parser.ended==footer["domains_closed"].get<uint64_t>(),"native footer counters");
        check(parser.quanta>0&&parser.unknownOffsets==0&&!parser.unreadyQuanta,"unaligned native layout");
    }
    void unchanged()const{for(size_t i=0;i<files.size();++i)check(fs::file_size(files[i])==manifest[i]["bytes"].get<uint64_t>()&&hashFile(files[i])==manifest[i]["sha256"].get<std::string>(),"source changed");}
};
struct State {
    DomainBridge bridge;Sha sha;std::ofstream file;uint64_t bytes=0;bool closed=false;uint32_t closeReason=0;std::string hash,name;
    State(uint64_t domain,uint32_t passes,uint64_t frequency,const fs::path& out):bridge(domain){
        name="domain-"+std::to_string(domain)+"-mat-object-input.bin";
        if(!out.empty()){file.open(out/name,std::ios::binary);check(bool(file),"domain file open");}
        auto h=fileHeader("MATBRG1",passes+1,frequency);write({reinterpret_cast<const uint8_t*>(&h),sizeof(h)});
    }
    void write(std::span<const uint8_t> data){sha.add(data);bytes+=data.size();if(file.is_open()){file.write(reinterpret_cast<const char*>(data.data()),data.size());check(bool(file),"domain write");}}
    void close(uint32_t reason){check(!closed,"duplicate close");write(bridge.finish().bytes());closed=true;closeReason=reason;hash=sha.finish();if(file.is_open()){file.close();check(bool(file),"domain close");}}
};
class Sink {
    const Source& source_;fs::path out_;std::map<uint64_t,std::unique_ptr<State>> states_;std::ofstream clocks_;
public:
    explicit Sink(const Source& source,const fs::path& out={}):source_(source),out_(out){
        if(!out.empty()){clocks_.open(out/"input-clock.csv");check(bool(clocks_),"clock CSV open");clocks_<<"domain,pass,sample_start,input_frames,object_packets,first_qpc,last_qpc\n";}
    }
    void accept(const Item& item){
        auto domain=item.h.domain;
        if(item.h.kind==q4::DomainBegin){check(!states_.contains(domain)&&source_.passes.at(domain)<UINT32_MAX,"domain begin/count");states_[domain]=std::make_unique<State>(domain,static_cast<uint32_t>(source_.passes.at(domain)),source_.frequency,out_);return;}
        auto& state=*states_.at(domain);check(!state.closed,"event after close");
        if(item.h.kind==q4::DomainEnd){state.close(item.h.reason);return;}
        check(item.h.kind==q4::PassEnd,"unknown work item");auto& q=item.q;
        state.write(state.bridge.consume(q,item.first,item.h.audio.qpc).bytes());
        if(clocks_.is_open())clocks_<<domain<<','<<q.pass<<','<<q.sampleStart<<','<<q.frames<<','<<q.objects.size()<<','<<item.first<<','<<item.h.audio.qpc<<'\n';
    }
    Json finish(){
        if(clocks_.is_open()){clocks_.close();check(bool(clocks_),"clock CSV close");}Json result=Json::array();
        for(auto& [domain,s]:states_){check(s->closed&&s->bridge.passes()==source_.passes.at(domain),"domain missing close/pass");Json ids=Json::array();for(auto i:s->bridge.identities)ids.push_back({{"native_id",i.id},{"source_slot",i.slot},{"host",i.host},{"original_id",i.originalId},{"generation",i.generation},{"first_ready_frame",i.firstReadyFrame},{"observed_source_create",i.observedCreate}});
            result.push_back({{"domain",domain},{"file",s->name},{"bytes",s->bytes},{"sha256",s->hash},{"process_passes",s->bridge.passes()},{"input_sample_frames",s->bridge.sample()},{"close_reason",s->closeReason},{"counts",s->bridge.counts},{"identities",ids}});
        }return result;
    }
};
static Json stats(std::vector<double> values){check(!values.empty(),"timing empty");std::sort(values.begin(),values.end());auto p=[&](double f){double x=(values.size()-1)*f;size_t a=static_cast<size_t>(x),b=std::min(a+1,values.size()-1);return values[a]+(values[b]-values[a])*(x-a);};return {{"p50",p(.5)},{"p99",p(.99)},{"max",values.back()}};}
struct Work {uint32_t index;int64_t target,enqueued;};
struct Timing {uint32_t index;int64_t target,enqueued,start,finish;};
static Json paced(const Source& source,const Json& expected,const fs::path& out){
    Sink sink(source);boost::lockfree::spsc_queue<Work,boost::lockfree::capacity<8>> queue;check(queue.is_lock_free(),"queue not lock-free");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr),timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    if(!event||!timer){if(event)CloseHandle(event);if(timer)CloseHandle(timer);throw std::runtime_error("replay event/timer");}
    std::atomic<bool> done=false,failed=false;std::exception_ptr workerError,producerError;std::vector<Timing> times;times.reserve(source.items.size());
    std::thread worker([&]{try{for(;;){Work w{};auto status=pollUntilDrained(queue,done,w);if(status==QueuePoll::item){auto start=now();sink.accept(source.items.at(w.index));times.push_back({w.index,w.target,w.enqueued,start,now()});}else if(status==QueuePoll::finished)break;else check(WaitForSingleObject(event,50)!=WAIT_FAILED,"worker event");}}catch(...){workerError=std::current_exception();failed.store(true,std::memory_order_release);}});
    uint32_t overrun=0,high=0;auto frequency=hz(),origin=now()+frequency/50,sourceOrigin=source.items.front().h.audio.qpc;
    try{for(uint32_t i=0;i<source.items.size();++i){check(!failed.load(std::memory_order_acquire),"worker failed");auto target=origin+static_cast<int64_t>((source.items[i].h.audio.qpc-sourceOrigin)*static_cast<long double>(frequency)/source.frequency);
        for(;;){auto left=target-now();if(left<=0)break;LARGE_INTEGER due;due.QuadPart=-((left*10000000+frequency-1)/frequency);check(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)!=0,"replay timer set");check(WaitForSingleObject(timer,50)!=WAIT_FAILED,"replay timer wait");}
        if(!queue.push({i,target,now()})){++overrun;break;}high=std::max(high,static_cast<uint32_t>(queue.read_available()));SetEvent(event);
    }}catch(...){producerError=std::current_exception();}
    done.store(true,std::memory_order_release);SetEvent(event);worker.join();CloseHandle(event);CloseHandle(timer);
    if(workerError)std::rethrow_exception(workerError);if(producerError)std::rethrow_exception(producerError);
    auto result=sink.finish();bool same=result==expected;std::vector<double> work,late;uint32_t missed=0;
    std::ofstream csv(out/"paced-timing.csv");check(bool(csv),"timing CSV");csv<<"item,domain,kind,target,enqueue,start,finish,qpc_frequency\n";
    for(auto t:times){const auto& item=source.items[t.index];csv<<t.index<<','<<item.h.domain<<','<<item.h.kind<<','<<t.target<<','<<t.enqueued<<','<<t.start<<','<<t.finish<<','<<frequency<<'\n';if(item.h.kind==q4::PassEnd){work.push_back((t.finish-t.start)*1000./frequency);late.push_back((t.finish-t.target)*1000./frequency);missed+=t.finish-t.target>frequency/100;}}
    csv.close();check(bool(csv),"timing CSV close");
    return {{"passed",same&&overrun==0&&missed==0&&times.size()==source.items.size()},{"matches_unpaced_all_domain_bytes_and_state",same},{"processed_items",times.size()},{"queue_capacity",8},{"queue_high_water",high},{"producer_overruns",overrun},{"process_deadline_10ms_misses",missed},{"process_worker_ms",stats(work)},{"process_target_to_finish_ms",stats(late)},
        {"source_schedule_seconds",(source.items.back().h.audio.qpc-sourceOrigin)/double(source.frequency)},
        {"scope","Offline preloaded immutable capture -> bounded Boost SPSC indices -> worker. Not audiodg/codec/transport latency."}};
}
int main(int argc,char** argv){
    fs::path out;bool created=false;
    try{
        check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg header/library mismatch");
        if(argc==2&&std::string(argv[1])=="--selftest"){check(runSelftests()==0,"legacy regressions");return runNative4Tests();}
        check(argc==4,"Usage: native-v4-replay CAPTURE CONTENT_PROOF NEW_OUTPUT (or --selftest)");
        Source source(fs::absolute(argv[1]),fs::absolute(argv[2]));out=fs::absolute(argv[3]);check(!fs::exists(out)&&fs::create_directories(out),"output must be new");created=true;
        auto began=now();Sink sink(source,out);for(const auto& item:source.items)sink.accept(item);auto result=sink.finish();double ms=(now()-began)*1000./hz();
        auto replay=paced(source,result,out);source.unchanged();
        Json report={{"offline_bridge_replay_passed",replay["passed"]},{"independent_golden_passed",false},{"source_files",source.manifest},{"source_unchanged",true},{"source_packets",source.packets},{"source_samples",source.samples},{"source_chunk_switches",source.files.size()-1},{"domains",result},{"paced",replay},{"unpaced_wall_ms_including_export_IO",ms},{"source_content_proof_sha256",hashFile(fs::absolute(argv[2]))},
            {"input_quantum_frames",480},{"rate",48000},{"gain_policy","F32(pcm * F32(volume * callGain)); exactly once; future vendor submission volume=1, gain=1"},
            {"close_record_semantics","MATBRG1 EOF is control-only: never issue Process or advance sample clock for it"},
            {"native_process_boundaries_from_recording",true},{"ffmpeg_avutil_version_int",avutil_version()},{"boost_version",BOOST_LIB_VERSION},
            {"mat_encoded",false},{"vendor_called",false},{"admin_required",false},{"production_default_changed",false},{"cross_domain_sample_sync_verified",false},
            {"limitations",{"Native dynamic full-span 480-frame input only. Short blocks, static beds, duplicate keys and missing still-live objects are rejected.","Per-domain files/identity and input clocks are independent; EOF is not audio. No domain concatenation or RTP timeline.","FFmpeg FIFO is bounded; worker containers allocate. Source is preloaded for offline pacing, not an RT producer design.","These files are diagnostic call plans, not MAT or MATLIF1; native lifecycle/encoder feed still needs a separate scoped test."}}};
        writeJson(out/"summary.json",report);std::cout<<report.dump(2)<<'\n';return replay["passed"].get<bool>()?0:1;
    }catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';if(created){try{writeJson(out/"failure.json",{{"failed",true},{"message",e.what()}});}catch(...){}}return 1;}
}
