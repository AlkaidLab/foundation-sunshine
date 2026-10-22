#define NOMINMAX
#include <windows.h>
#include "cpp_bridge_core.hpp"
#include "cpp_bridge_queue.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/version.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <regex>
#include <thread>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
using namespace cpp_bridge;
namespace fs=std::filesystem;
using Json=nlohmann::json;
static int64_t now(){LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
static int64_t frequency(){LARGE_INTEGER value{};QueryPerformanceFrequency(&value);return value.QuadPart;}
class Sha {
    AVSHA* context_{};
public:
    Sha(){context_=av_sha_alloc();check(context_ && av_sha_init(context_,256)==0,"SHA256 init");}
    ~Sha(){av_free(context_);}
    void add(std::span<const uint8_t> bytes){av_sha_update(context_,bytes.data(),bytes.size());}
    template<typename T>void object(const T& value){add({reinterpret_cast<const uint8_t*>(&value),sizeof(value)});}
    std::string finish(){std::array<uint8_t,32> hash{};av_sha_final(context_,hash.data());std::ostringstream out;for(auto b:hash)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return out.str();}
};
static std::string hashFile(const fs::path& path){
    std::ifstream input(path,std::ios::binary);check(bool(input),"hash open");Sha hash;std::array<uint8_t,65536> buffer{};
    while(input){input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());hash.add({buffer.data(),static_cast<size_t>(input.gcount())});}
    check(input.eof(),"hash read error");return hash.finish();
}
static Json readJson(const fs::path& path){std::ifstream in(path);check(bool(in),"JSON open");Json data;in>>data;return data;}
static void writeJson(const fs::path& path,const Json& data){std::ofstream out(path);check(bool(out),"JSON output");out<<data.dump(2)<<'\n';check(bool(out),"JSON write");}
struct Source {
    Json summary,position,manifest;std::vector<fs::path> files;uint32_t rounds{};uint64_t hz{};std::string session;
    explicit Source(const fs::path& root){
        summary=readJson(root/"summary.json");position=readJson(root/"position-summary.json");auto backend=readJson(root/"backend.json");auto format=readJson(root/"format.json");
        check(summary["status"]==2 && summary["error"]==0 && summary["original_bytes_restored"]==true
            && summary["dropped_full"]==0 && summary["dropped_busy"]==0 && summary["invalid"]==0
            && position["stateful"]==true && position["evictions"]==0 && backend["extended_calls"]==0
            && format["version"]==2 && format["header_bytes"]==112,"source capture integrity/format");
        hz=summary.at("qpc_frequency").get<uint64_t>();check(hz>0,"source QPC frequency");
        uint64_t records=summary.at("blocks").get<uint64_t>();check(records>0 && records%max_round_objects==0 && records/max_round_objects<UINT32_MAX-1,"complete fixture rounds");rounds=static_cast<uint32_t>(records/max_round_objects);
        files.push_back(root/"global-objects.bin");std::map<unsigned,fs::path> numbered;
        std::regex pattern("global-objects-([0-9]+)[.]bin");
        for(auto& e:fs::directory_iterator(root)){
            std::smatch match;std::string name=e.path().filename().string();
            if(e.is_regular_file() && std::regex_match(name,match,pattern))check(numbered.emplace(std::stoul(match[1]),e.path()).second,"duplicate chunk number");
        }
        unsigned wanted=1;for(auto& [number,path]:numbered){check(number==wanted++,"missing source chunk");files.push_back(path);}
        manifest=Json::array();uint64_t bytes=0;
        for(auto& path:files){uint64_t size=fs::file_size(path);bytes+=size;manifest.push_back({{"file",path.filename().string()},{"bytes",size},{"sha256",hashFile(path)}});}
        check(bytes==summary["bytes"].get<uint64_t>(),"source byte count");
        // Match Python's json.dumps(manifest, sort_keys=True), including separator spaces.
        std::string text="[";bool first=true;
        for(const auto& item:manifest){if(!first)text+=", ";first=false;text+="{\"bytes\": "+item["bytes"].dump()+", \"file\": "+item["file"].dump()+", \"sha256\": "+item["sha256"].dump()+"}";}
        text+=']';Sha hash;hash.add({reinterpret_cast<const uint8_t*>(text.data()),text.size()});session=hash.finish();
    }
    void unchanged() const {for(size_t i=0;i<files.size();++i)check(fs::file_size(files[i])==manifest[i]["bytes"].get<uint64_t>() && hashFile(files[i])==manifest[i]["sha256"].get<std::string>(),"source changed");}
};
class Reader {
    const Source& source_;std::ifstream stream_;size_t file_=0;uint64_t sequence_=0,frames_=0;int64_t previousQpc_=-1;
    std::array<SourceKey,max_round_objects> initialOrder_{},keySet_{};bool first_=true;uint32_t index_=0;
    bool record(RecordInput& out){
        for(;;){
            if(!stream_.is_open()){if(file_==source_.files.size())return false;stream_.clear();stream_.open(source_.files[file_],std::ios::binary);check(bool(stream_),"source chunk open");}
            stream_.read(reinterpret_cast<char*>(&out.state),sizeof(out.state));auto got=stream_.gcount();
            if(!got){check(stream_.eof(),"source chunk I/O error");stream_.close();++file_;continue;}
            check(got==sizeof(out.state),"truncated source header");auto& h=out.state;auto& a=h.audio;
            check(a.magic==0x324f424a && h.version==2 && h.reserved==0 && a.sequence==sequence_ && a.frames>0 && a.frames<=max_source_frames,"source header/sequence");
            check(previousQpc_<0 || a.qpc>=previousQpc_,"source QPC backwards");
            stream_.read(reinterpret_cast<char*>(out.pcm.data()),a.frames*sizeof(float));check(stream_.gcount()==a.frames*4,"truncated source PCM");
            check(std::isfinite(a.callGain) && std::all_of(out.pcm.begin(),out.pcm.begin()+a.frames,[](float x){return std::isfinite(x);}),"nonfinite source PCM/gain");
            ++sequence_;frames_+=a.frames;previousQpc_=a.qpc;return true;
        }
    }
public:
    uint32_t reordered=0;
    explicit Reader(const Source& source):source_(source){}
    bool batch(Batch& out){
        out.index=index_;out.count=0;
        for(auto& r:out.records){if(!record(r)){check(!out.count,"partial source round");check(sequence_==source_.summary["blocks"].get<uint64_t>() && frames_==source_.summary["frames"].get<uint64_t>(),"source counters");return false;}++out.count;}
        std::array<SourceKey,max_round_objects> order{},sorted{};
        for(unsigned i=0;i<out.count;++i){check(out.records[i].state.audio.frames==quantum,"fixture needs aligned 480 frames");order[i]=sourceKey(out.records[i]);}
        sorted=order;std::sort(sorted.begin(),sorted.end());check(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate key within source round");
        if(first_){initialOrder_=order;keySet_=sorted;first_=false;}
        check(sorted==keySet_,"source key set changed; no heuristic live grouping");reordered+=order!=initialOrder_;++index_;return true;
    }
};
struct BinaryWriter {
    std::ofstream output;Sha hash;
    BinaryWriter(const fs::path& path,const FileHeader& header):output(path,std::ios::binary){check(bool(output),"binary output");write({reinterpret_cast<const uint8_t*>(&header),sizeof(header)});}
    void write(std::span<const uint8_t> bytes){output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());check(bool(output),"binary write");hash.add(bytes);}
    std::string close(){output.close();check(bool(output),"binary close");return hash.finish();}
};
static Json cacheCounts(const PositionCache& c){return {{"position_state_0",c.unknown},{"position_state_1",c.updates},{"position_state_2",c.cached},{"position_state_3",c.defaults},{"position_state_4",c.statics},{"creates",c.creates},{"ends",c.ends},{"loss_invalidations",c.gapInvalidations}};}
static Json stats(std::vector<double> values){check(!values.empty(),"empty timing samples");std::sort(values.begin(),values.end());auto percentile=[&](double p){double x=(values.size()-1)*p;auto lo=static_cast<size_t>(x),hi=std::min(lo+1,values.size()-1);return values[lo]+(values[hi]-values[lo])*(x-lo);};return {{"p50",percentile(.5)},{"p99",percentile(.99)},{"max",values.back()}};}
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}Handle()=default;Handle(const Handle&)=delete;};
static void waitUntil(HANDLE timer,int64_t target,int64_t hz){
    for(;;){int64_t left=target-now();if(left<=0)return;LARGE_INTEGER due{};due.QuadPart=-((left*10000000+hz-1)/hz);
        check(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)!=0,"set worker replay timer");check(WaitForSingleObject(timer,50)!=WAIT_FAILED,"wait replay timer");}
}
struct Work {uint32_t pool,index;int64_t target,enqueued;};
struct Timing {uint32_t index;int64_t target,enqueued,start,finish;};
struct PacedResult {Json report;std::vector<Timing> timing;};
static PacedResult paced(const Source& source,uint32_t start,const std::string& expected){
    Reader reader(source);Bridge bridge;auto prefix=std::make_unique<Batch>();
    for(uint32_t i=0;i<start;++i){check(reader.batch(*prefix),"paced prefix");bridge.consume(*prefix);}
    constexpr unsigned poolSize=9,capacity=8;
    auto pool=std::make_unique<std::array<Batch,poolSize>>();
    boost::lockfree::spsc_queue<Work,boost::lockfree::capacity<capacity>> ready;
    boost::lockfree::spsc_queue<uint32_t,boost::lockfree::capacity<poolSize>> free;
    check(ready.is_lock_free() && free.is_lock_free(),"target queue atomic operations not lock-free");
    for(uint32_t i=0;i<poolSize;++i)check(free.push(i),"initial free pool");
    Handle event,timer;event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event.value,"replay event");
    timer.value=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);check(timer.value,"high resolution replay timer");
    std::atomic<bool> done=false,failed=false;std::exception_ptr workerError;Sha window;
    std::vector<Timing> samples;samples.reserve(source.rounds-start);int64_t hz=frequency();
    std::thread consumer([&]{
        try{
            for(;;){
                Work item{};
                auto state=pollUntilDrained(ready,done,item);
                if(state==QueuePoll::item){
                    int64_t began=now();auto& output=bridge.consume((*pool)[item.pool]);window.add(output.dynamic.bytes());int64_t end=now();
                    samples.push_back({item.index,item.target,item.enqueued,began,end});check(free.push(item.pool),"free pool invariant");
                }else if(state==QueuePoll::finished)break;
                else check(WaitForSingleObject(event.value,50)!=WAIT_FAILED,"consumer event wait");
            }
        }catch(...){workerError=std::current_exception();failed.store(true,std::memory_order_release);}
    });
    unsigned overrun=0,highWater=0;int64_t origin=0,sourceOrigin=0;std::exception_ptr producerError;
    try{
        for(uint32_t i=start;i<source.rounds && !failed.load(std::memory_order_acquire);++i){
            uint32_t slot{};if(!free.pop(slot)){++overrun;break;}
            auto& batch=(*pool)[slot];check(reader.batch(batch),"paced source ended early");
            if(!origin){origin=now();sourceOrigin=batch.records[0].state.audio.qpc;}
            int64_t target=origin+static_cast<int64_t>((batch.records[0].state.audio.qpc-sourceOrigin)*static_cast<long double>(hz)/source.hz);
            waitUntil(timer.value,target,hz);Work item{slot,i,target,now()};
            if(!ready.push(item)){++overrun;break;}
            highWater=std::max(highWater,static_cast<unsigned>(ready.read_available()));SetEvent(event.value);
        }
    }catch(...){producerError=std::current_exception();}
    done.store(true,std::memory_order_release);SetEvent(event.value);consumer.join();
    if(producerError)std::rethrow_exception(producerError);if(workerError)std::rethrow_exception(workerError);
    std::vector<double> work,late;unsigned missed=0;
    for(auto t:samples){work.push_back((t.finish-t.start)*1000./hz);late.push_back((t.finish-t.target)*1000./hz);missed+=t.finish-t.target>hz/100;}
    std::string hash=window.finish();bool okay=overrun==0 && samples.size()==source.rounds-start && hash==expected && missed==0;
    Json report={{"window_start_frame",start},{"window_end_frame_exclusive",source.rounds},{"prefix_reconstructed_unpaced",true},
        {"paced_source_seconds",(source.rounds-start)*.01},{"completed_rounds",samples.size()},{"queue_capacity_rounds",capacity},{"pool_slots",poolSize},
        {"observed_queue_high_water_rounds",highWater},{"producer_overrun_count",overrun},{"worker_wall_ms",stats(work)},{"arrival_to_finish_ms",stats(late)},
        {"finish_after_nominal_10ms_count",missed},{"dynamic_sha256",hash},{"matches_unpaced_and_golden",false},{"matches_unpaced",hash==expected},{"passed",okay},
        {"queue","Boost.Lockfree fixed-capacity SPSC queues of pool indices; no batch pointers in IPC"},
        {"caveat","Native C++ disk replay worker only; not audiodg capture, MAT encoding, real game load, or cross-process transport."}};
    return {report,std::move(samples)};
}

int runSelftests();
int main(int argc,char** argv){
    fs::path out;bool outputCreated=false;
    try{
        check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg header/library version mismatch");
        if(argc==2 && std::string(argv[1])=="--selftest")return runSelftests();
        check(argc==5,"Usage: cpp-bridge-replay SOURCE NEW_OUTPUT PACED_ROUNDS 37 (or --selftest)");
        // Fifth argument is a bounded fixture width, kept explicit to avoid a fake general live adapter.
        check(std::string(argv[4])=="37","only the verified 37-key-set fixture adapter is supported");
        fs::path root=fs::absolute(argv[1]);out=fs::absolute(argv[2]);uint32_t requested=std::stoul(argv[3]);
        check(requested>0 && requested<=1000,"paced replay must be 1..1000 rounds");
        check(!fs::exists(out) && fs::create_directories(out),"output must be new");
        outputCreated=true;
        Source source(root);uint32_t start=source.rounds-std::min(source.rounds,requested);Reader reader(source);Bridge bridge;Sha window;
        BinaryWriter objects(out/"mat-object-input.bin",fileHeader("MATBRG1",source.rounds+1,source.hz));
        BinaryWriter bed(out/"static-bed-input.bin",fileHeader("MATBED1",source.rounds,source.hz));
        auto batch=std::make_unique<Batch>();int64_t began=now();uint32_t count=0;
        while(reader.batch(*batch)){
            auto& output=bridge.consume(*batch);auto bytes=output.dynamic.bytes();objects.write(bytes);bed.write(output.bed.bytes());
            if(batch->index>=start)window.add(bytes);++count;
        }
        check(count==source.rounds,"source round count");objects.write(bridge.finish().bytes());
        double processingMs=(now()-began)*1000./frequency();std::string dynamicHash=objects.close(),bedHash=bed.close(),windowHash=window.finish();
        check(bridge.counts["source_records"]==source.summary["blocks"].get<uint64_t>() && bridge.counts["source_object_samples"]==source.summary["frames"].get<uint64_t>(),"bridge sample counters");
        auto& c=bridge.cache;
        check(c.unknown==source.position["unknown"] && c.updates==source.position["updated"] && c.cached==source.position["cached"]
            && c.defaults==source.position["create_default_origin"] && c.statics==source.position["static_implicit"] && !c.evictions,"cache counters");
        std::cout<<"C++ unpaced input replay complete; source-snapshot/golden validation remains external.\n"<<std::flush;
        PacedResult replay=paced(source,start,windowHash);
        std::ofstream csv(out/"paced-timing.csv");check(bool(csv),"timing CSV open");
        csv<<"source_round,target_perf_seconds,enqueue_perf_seconds,worker_start_perf_seconds,worker_finish_perf_seconds\n"<<std::setprecision(17);
        double hz=static_cast<double>(frequency());for(auto t:replay.timing)csv<<t.index<<','<<t.target/hz<<','<<t.enqueued/hz<<','<<t.start/hz<<','<<t.finish/hz<<'\n';csv.close();check(bool(csv),"timing CSV close");
        source.unchanged();Json identities=Json::array(),slots=Json::array();
        for(auto i:bridge.identities)identities.push_back({{"native_id",i.id},{"source_slot",i.slot},{"session",source.session},{"host",i.host},{"original_id",i.originalId},{"type",i.type},{"loss_epoch",i.epoch},{"generation",i.generation},{"first_ready_frame",i.firstReadyFrame},{"source_create_observed",i.observedCreate}});
        for(auto [key,slot]:bridge.sourceSlots())slots.push_back({{"source_slot",slot},{"host",std::get<0>(key)},{"original_id",std::get<1>(key)},{"type",std::get<2>(key)}});
        Json result={{"implementation","C++20; existing collector PositionCache + FFmpeg AVAudioFifo + Boost SPSC"},
            {"source",root.string()},{"source_session_sha256",source.session},{"source_files",source.manifest},{"source_unchanged",true},
            {"source_rounds",source.rounds},{"source_audio_clock_seconds",source.rounds*.01},{"source_records",source.summary["blocks"]},{"source_object_samples",source.summary["frames"]},
            {"chunks",source.files.size()},{"chunk_switches",source.files.size()-1},{"reordered_rounds",reader.reordered},{"counts",bridge.counts},{"cache_counts",cacheCounts(c)},
            {"dynamic_id_map",identities},{"source_slot_map",slots},{"hashes",{{"dynamic_sha256",dynamicHash},{"bed_sha256",bedHash}}},
            {"paced",replay.report},{"unpaced_processing_wall_ms_including_export_IO",processingMs},{"offline_bridge_replay_passed",replay.report["passed"]},
            {"independent_golden_passed",false},{"dynamic_subset_has_no_unknown_audible_or_range_error",bridge.counts["quarantined_unknown_audible_records"]==0 && bridge.counts["native_pcm_range_exceedances"]==0},
            {"ffmpeg_avutil_version_int",avutil_version()},{"boost_version",BOOST_LIB_VERSION},{"mat_encoded",false},{"vendor_called",false},{"admin_required",false},{"game_started",false},
            {"live_quantum_boundaries_captured",false},{"static_bed_encoder_route_verified",false},{"full_game_mix_mat_ready",false},
            {"caveats",{"Only the recorded 37-key-set/480-frame fixture has verified grouping. QPC schedules arrivals, never synthesizes sample timestamps.",
                         "FFmpeg FIFOs are bounded and worker-owned. General in-quantum metadata spans remain explicit and cannot be flattened.",
                         "Unknown audible samples are quarantined/reported; silent unknowns are deferred. Bed data is preserved, not encoded.",
                         "Inputs are diagnostic MATBRG1/MATBED1, not MAT or MATLIF1. Independent Python/raw-snapshot validation is a separate required step."}}};
        writeJson(out/"summary.json",result);std::cout<<Json({{"counts",bridge.counts},{"hashes",result["hashes"]},{"paced",replay.report}}).dump(2)<<'\n';return replay.report["passed"].get<bool>()?0:3;
    }catch(const std::exception& e){
        std::cerr<<"FAILED: "<<e.what()<<'\n';
        if(outputCreated){try{if(!fs::exists(out/"failure.json"))writeJson(out/"failure.json",{{"failed",true},{"message",e.what()},{"mat_encoded",false}});}catch(...){}}
        return 1;
    }
}
