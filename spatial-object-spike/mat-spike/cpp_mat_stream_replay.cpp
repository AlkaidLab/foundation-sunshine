#define NOMINMAX
#include "cpp_mat_stream_fake.hpp"
#include <boost/version.hpp>
#if BOOST_VERSION != 108500
#error Expected existing complete Boost 1.85
#endif
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
using namespace mat_stream;
namespace fs=std::filesystem;using Json=nlohmann::json;
int runSelftests();int runNative4Tests();int runMatStreamTests();
static std::string hashFile(const fs::path& path){
    std::ifstream f(path,std::ios::binary);check(bool(f),"hash open");AVSHA* sha=av_sha_alloc();check(sha&&av_sha_init(sha,256)==0,"hash init");
    std::array<uint8_t,65536> data;while(f){f.read(reinterpret_cast<char*>(data.data()),data.size());av_sha_update(sha,data.data(),static_cast<size_t>(f.gcount()));}
    check(f.eof(),"hash read");uint8_t result[32];av_sha_final(sha,result);av_free(sha);std::ostringstream s;for(auto v:result)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return s.str();
}
static Json load(const fs::path& path){std::ifstream f(path);check(bool(f),"JSON open");Json j;f>>j;return j;}
static void save(const fs::path& path,const Json& j){std::ofstream f(path);f<<j.dump(2)<<'\n';check(bool(f),"JSON write");}
struct Item {q4::Header header{};q4::Quantum quantum;int64_t first=0;};
struct Recording {
    std::vector<Item> items;Json files=Json::array();uint64_t frames=0,frequency=0;std::map<uint64_t,uint64_t> passes;
    Recording(const fs::path& root,const fs::path& proofPath){
        auto proof=load(proofPath),format=load(root/"format.json"),footer=load(root/"summary.json");
        check(proof["complete_encoder_input_capture_contract_passed"]==true&&proof["first_block_coordinates_native_verified"]==true,"native content oracle required");
        check(format["version"]==4&&format["source_kind"]=="native-pre-submit-paired"&&format["pcm_stage"]=="before-original-submit"&&format["header_bytes"]==160,"native v4 recording only");
        check(footer["status"]==2&&footer["original_bytes_restored"]==true,"native capture did not close cleanly");
        for(auto k:{"error","dropped_full","dropped_busy","invalid","extended_calls","unbound_after_arming"})check(footer[k]==0,"capture integrity");
        frequency=footer["qpc_frequency"].get<uint64_t>();check(frequency>0,"source frequency");
        q4::Assembler assembler;uint64_t records=0;std::map<uint64_t,int64_t> first;
        // The independent content proof freezes the exact ordered chunk list.
        for(const auto& source:proof["source_files"]){
            auto name=source["file"].get<std::string>();check(fs::path(name).filename()==name,"source filename only");auto p=root/name;
            check(fs::file_size(p)==source["bytes"].get<uint64_t>()&&hashFile(p)==source["sha256"].get<std::string>(),"frozen source changed");files.push_back(source);
            std::ifstream f(p,std::ios::binary);check(bool(f),"recording open");
            for(;;){q4::Header h{};f.read(reinterpret_cast<char*>(&h),sizeof(h));auto got=f.gcount();if(!got){check(f.eof(),"source I/O");break;}
                check(got==sizeof(h)&&h.audio.frames<=kSamples,"capture record bounds");std::array<float,kSamples> pcm{};f.read(reinterpret_cast<char*>(pcm.data()),h.audio.frames*4);check(f.gcount()==h.audio.frames*4,"capture PCM truncated");++records;
                if(h.kind==q4::ObjectBefore)frames+=h.audio.frames;
                if(h.kind==q4::PassBegin)first[h.domain]=h.audio.qpc;
                q4::Quantum q;bool sealed=assembler.push(h,pcm.data(),q);
                if(h.kind==q4::DomainBegin||h.kind==q4::DomainEnd)items.push_back({h,{},h.audio.qpc});
                if(sealed){items.push_back({h,std::move(q),first.at(h.domain)});++passes[h.domain];}
            }
        }
        assembler.finish();check(records==footer["blocks"].get<uint64_t>()&&frames==footer["frames"].get<uint64_t>()&&assembler.results==footer["submit_results"].get<uint64_t>()&&assembler.markers==footer["process_results"].get<uint64_t>()&&assembler.opened==footer["domains_opened"].get<uint64_t>()&&assembler.ended==footer["domains_closed"].get<uint64_t>(),"capture footer counts");
        check(assembler.quanta>0&&assembler.unknownOffsets==0&&!assembler.unreadyQuanta,"unaligned quantum");
    }
    void unchanged(const fs::path& root)const{for(const auto& f:files)check(hashFile(root/f["file"].get<std::string>())==f["sha256"].get<std::string>(),"recording changed during replay");}
};
template<class Value>static void write(std::ofstream& f,const Value& value){f.write(reinterpret_cast<const char*>(&value),sizeof(value));check(bool(f),"binary write");}
static void writeSource(std::ofstream& f,const Packet& p){write(f,p.header);for(unsigned i=0;i<p.header.calls;++i){write(f,p.calls[i].header);write(f,p.calls[i].pcm);}}
static void savePlan(const fs::path& path,const FakeEncoder& sink){
    std::ofstream f(path,std::ios::binary);check(bool(f),"plan open");
    // Same compact per-block representation as frozen MATV4P1, but no header:
    // this file is a FAKE encoder's call transcript, never an encoded MAT stream.
    for(const auto& b:sink.blocks){write(f,static_cast<uint32_t>(b.tick.phase));write(f,b.tick.sourceIndex);write(f,static_cast<uint32_t>(b.calls.size()));
        for(const auto& c:b.calls){write(f,c.header.id);write(f,c.header.flags);write(f,c.header.xyz);write(f,c.pcm);}}
    f.close();check(bool(f),"plan close");
}
struct State {
    cpp_bridge_native4::DomainBridge bridge;FakeEncoder sink;std::ofstream source;
    std::unique_ptr<Pipeline> pipe;bool closed=false;Json result;
    State(uint64_t domain,uint32_t count,uint64_t hz,const fs::path& out,bool paced):bridge(domain){
        source.open(out/("domain-"+std::to_string(domain)+"-source.bin"),std::ios::binary);check(bool(source),"source export open");
        write(source,fileHeader("MATBRG1",count+1,hz));
        pipe=std::make_unique<Pipeline>(domain,sink,Options{paced,20,30},[this](const Packet& p){writeSource(source,p);});
    }
    void close(uint64_t domain,const fs::path& out){
        auto m=pipe->join();closed=true;source.close();check(bool(source),"source export close");
        auto transcript=out/("domain-"+std::to_string(domain)+"-fake-plan.bin");savePlan(transcript,sink);
        result={{"domain",domain},{"completed",m.completed},{"error",m.error},{"fault",static_cast<unsigned>(pipe->failure())},
            {"source_process_rounds",m.sourceRounds},{"preroll_rounds",m.prerollRounds},{"drain_rounds",m.drainRounds},
            {"submissions",m.submissions},{"source_samples",m.sourceSamples},{"EOF_close_controls",m.closeControls},
            {"scheduled_rounds",sink.blocks.size()},{"queue_capacity",queueCapacity},{"queue_high_water",pipe->highWater()},
            {"snapshot_bytes",sizeof(Packet)},{"queue_usable_payload_bytes",sizeof(Packet)*queueCapacity},
            {"queue_object_bytes_including_reserved_slot",Pipeline::queueObjectBytes()},
            {"fake_plan_file",transcript.filename().string()},{"fake_plan_sha256",hashFile(transcript)},
            {"source_file","domain-"+std::to_string(domain)+"-source.bin"},
            {"source_sha256",hashFile(out/("domain-"+std::to_string(domain)+"-source.bin"))},
            {"encoder_opens",sink.opens},{"encoder_closes",sink.closes},{"encoder_completed",sink.completed}};
    }
};
int main(int argc,char** argv){
    fs::path out;bool created=false;
    try{
        check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg ABI mismatch");
        if(argc==2&&std::string(argv[1])=="--selftest"){check(runSelftests()==0&&runNative4Tests()==0,"bridge regressions");return runMatStreamTests();}
        check(argc==5||argc==6,"Usage: mat-stream-replay CAPTURE CONTENT_PROOF BRIDGE_ORACLE NEW_OUTPUT [--paced]");
        bool paced=argc==6;check(!paced||std::string(argv[5])=="--paced","unknown option");
        fs::path root=fs::absolute(argv[1]);Recording recording(root,fs::absolute(argv[2]));auto golden=load(fs::absolute(argv[3]));
        check(golden["offline_bridge_replay_passed"]==true,"frozen C++ bridge oracle required");
        out=fs::absolute(argv[4]);check(!fs::exists(out)&&fs::create_directories(out),"new output required");created=true;
        std::map<uint64_t,std::unique_ptr<State>> states;auto packet=std::make_unique<Packet>();
        for(const auto& item:recording.items){auto domain=item.header.domain;
            if(item.header.kind==q4::DomainBegin){check(!states.contains(domain),"duplicate domain");states[domain]=std::make_unique<State>(domain,static_cast<uint32_t>(recording.passes.at(domain)),recording.frequency,out,paced);continue;}
            auto& s=*states.at(domain);check(!s.closed,"event after domain end");
            if(item.header.kind==q4::DomainEnd){
                snapshot(*packet,domain,s.bridge.finish());
                // The source transcript includes original EOF, but the codec
                // observer deliberately does not treat EOF as a source Process.
                check(s.pipe->pushOffline(*packet),"close enqueue aborted");s.close(domain,out);
                // After join, main owns the file again. Append control EOF only.
                std::ofstream f(out/("domain-"+std::to_string(domain)+"-source.bin"),std::ios::binary|std::ios::app);writeSource(f,*packet);f.close();check(bool(f),"EOF export close");
                s.result["source_sha256"]=hashFile(out/("domain-"+std::to_string(domain)+"-source.bin"));continue;
            }
            snapshot(*packet,domain,s.bridge.consume(item.quantum,item.first,item.header.audio.qpc));
            check(s.pipe->pushOffline(*packet),"source enqueue aborted");
        }
        Json domains=Json::array();bool passed=true;
        for(auto& [domain,s]:states){check(s->closed,"missing domain end");bool same=false;
            for(const auto& d:golden["domains"])if(d["domain"]==domain)same=d["sha256"]==s->result["source_sha256"];
            s->result["source_export_matches_frozen_bridge"]=same;domains.push_back(s->result);passed=passed&&same&&s->result["completed"].get<bool>();}
        recording.unchanged(root);
        Json report={{"offline_threaded_fake_passed",passed},{"paced_worker",paced},{"domains",domains},{"source_files",recording.files},
            {"source_content_proof_sha256",hashFile(fs::absolute(argv[2]))},{"bridge_oracle_sha256",hashFile(fs::absolute(argv[3]))},
            {"source_unchanged",true},{"native_encoder_called",false},{"mat_encoded",false},{"admin_required",false},
            {"live_capture_verified",false},{"production_default_changed",false},
            {"policy","Per-domain 8-slot owned snapshots; single nonblocking producer; overflow aborts epoch. Offline feeder alone may wait. EOF controls drain, no extra source Process."},
            {"limitations",{"Fake sink records calls only; no Dolby ABI, audio process, driver or endpoint calls.","Recording preloaded; worker containers/file I/O allocate. Not an RT capture producer or hard-real-time benchmark.","Offline feeder backpressure can delay source publication, including 200ms warmup; no live startup/overflow-free claim.","Reset requires a fresh pipeline/encoder domain after prior worker exits; failed epoch output must not be played/concatenated."}}};
        save(out/"summary.json",report);std::cout<<report.dump(2)<<'\n';return passed?0:1;
    }catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';if(created)try{save(out/"failure.json",{{"failed",true},{"message",e.what()},{"native_encoder_called",false}});}catch(...){}return 1;}
}
