#define NOMINMAX
#include "quantum_assembler.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <regex>
#include <map>
#include <iostream>
namespace fs=std::filesystem;
using Json=nlohmann::json;
using namespace quantum_capture;
class Sha {
    BCRYPT_ALG_HANDLE algorithm_{};BCRYPT_HASH_HANDLE hash_{};std::vector<UCHAR> work_;
public:
    Sha() {
        Require(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"SHA provider");
        DWORD size=0,got=0;Require(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&got,0)==0,"SHA size");
        work_.resize(size);Require(BCryptCreateHash(algorithm_,&hash_,work_.data(),size,nullptr,0,0)==0,"SHA create");
    }
    ~Sha(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    void add(const void* data,size_t bytes){Require(bytes<=ULONG_MAX,"SHA input bounds");if(bytes)Require(BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(data)),static_cast<ULONG>(bytes),0)==0,"SHA data");}
    template<class T>void add(const T& value){add(&value,sizeof(value));}
    std::string finish(){std::array<UCHAR,32> bytes{};Require(BCryptFinishHash(hash_,bytes.data(),32,0)==0,"SHA finish");std::ostringstream out;for(auto b:bytes)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return out.str();}
};
static Json readJson(const fs::path& path){std::ifstream in(path);Require(bool(in),"JSON open");Json result;in>>result;return result;}
static void writeJson(const fs::path& path,const Json& data){std::ofstream out(path);Require(bool(out),"JSON output");out<<data.dump(2)<<'\n';Require(bool(out),"JSON write");}
static std::string fileHash(const fs::path& path){std::ifstream in(path,std::ios::binary);Require(bool(in),"source open");Sha sha;std::array<char,65536> data{};while(in){in.read(data.data(),data.size());sha.add(data.data(),size_t(in.gcount()));}Require(in.eof(),"source read");return sha.finish();}
static void stateHash(Sha& sha,const StateRecord& s) {
    sha.add(s.audio.host);sha.add(s.audio.objectId);sha.add(s.audio.type);sha.add(s.audio.frames);
    sha.add(&s.audio.x,3*sizeof(float));sha.add(s.audio.volume);sha.add(s.audio.callGain);
    sha.add(s.positionState);sha.add(s.volumeKnown);sha.add(s.generation);sha.add(s.lossEpoch);sha.add(s.propertyFlags);
}
int wmain(int argc,wchar_t** argv) {
    fs::path out;bool created=false;
    try {
        Require(argc==3,"Usage: quantum-verify SOURCE NEW_OUTPUT (v3 only; no process access)");
        fs::path root=fs::absolute(argv[1]);out=fs::absolute(argv[2]);
        auto format=readJson(root/L"format.json");auto source=readJson(root/L"summary.json");
        Require(format["version"]==3 && format["header_bytes"]==112,"requires real v3 markers; old v2 recordings cannot acquire missing boundaries");
        std::string kind=format.at("source_kind");
        Require(kind=="native-process-marker-probe" || kind=="synthetic-process-markers-over-recorded-pcm","unknown source provenance");
        Require(source["status"]==2 && source["error"]==0 && source["original_bytes_restored"]==true
            && source["dropped_full"]==0 && source["dropped_busy"]==0 && source["invalid"]==0,"capture integrity failed");
        if(kind=="native-process-marker-probe")Require(readJson(root/L"backend.json")["extended_calls"]==0,"unsupported extended metadata");
        Require(!fs::exists(out)&&fs::create_directories(out),"output must be new");created=true;
        std::vector<fs::path> paths{root/L"global-objects.bin"};std::map<unsigned,fs::path> chunks;std::regex pattern("global-objects-([0-9]+)[.]bin");
        for(auto& entry:fs::directory_iterator(root)){std::smatch match;auto name=entry.path().filename().string();if(entry.is_regular_file()&&std::regex_match(name,match,pattern))Require(chunks.emplace(std::stoul(match[1]),entry.path()).second,"duplicate source chunk");}
        unsigned next=1;for(auto& item:chunks){Require(item.first==next++,"missing source chunk");paths.push_back(item.second);}
        Json manifest=Json::array();uint64_t bytes=0;
        for(auto& path:paths){auto size=fs::file_size(path);bytes+=size;manifest.push_back({{"file",path.filename().string()},{"bytes",size},{"sha256",fileHash(path)}});}
        Require(bytes==source["bytes"],"source byte counter");
        Assembler assembler;Quantum q;Sha allPcm,emittedPcm,states;uint64_t records=0,samples=0,emittedSamples=0,clockEnd=0;
        std::map<unsigned,uint64_t> widths,frameSizes,positionStates;
        std::ofstream csv(out/L"quanta.csv");Require(bool(csv),"quantum CSV");
        csv<<"quantum,sample_start,frames,objects,full_span_layout_ready,first_qpc,last_qpc\n";
        for(auto& path:paths) {
            std::ifstream in(path,std::ios::binary);Require(bool(in),"chunk open");
            for(;;) {
                QuantumRecord event{};in.read(reinterpret_cast<char*>(&event),sizeof(event));auto got=in.gcount();
                if(!got){Require(in.eof(),"chunk I/O error");break;}
                Require(got==sizeof(event)&&event.audio.frames<=kSamples,"truncated v3 header/PCM bound");
                std::array<float,kSamples> pcm{};in.read(reinterpret_cast<char*>(pcm.data()),event.audio.frames*4);
                Require(in.gcount()==event.audio.frames*4,"truncated v3 PCM");++records;
                if(event.kind==kQuantumObject){samples+=event.audio.frames;allPcm.add(pcm.data(),event.audio.frames*4);}
                if(assembler.push(event,pcm.data(),q)) {
                    Require(q.sampleStart==clockEnd,"noncontiguous marker sample clock");clockEnd+=q.frames;
                    ++widths[static_cast<unsigned>(q.objects.size())];++frameSizes[q.frames];
                    csv<<q.index<<','<<q.sampleStart<<','<<q.frames<<','<<q.objects.size()<<','<<q.fullSpanLayoutReady<<','<<q.firstQpc<<','<<q.lastQpc<<'\n';
                    for(const auto& object:q.objects){emittedSamples+=object.pcm.size();emittedPcm.add(object.pcm.data(),object.pcm.size()*4);stateHash(states,object.state);++positionStates[object.state.positionState];}
                }
            }
        }
        assembler.finish();csv.close();Require(bool(csv),"quantum CSV close");
        Require(records==source["blocks"]&&samples==source["frames"],"source record/sample counters");
        Require(assembler.markersSeen>=2&&assembler.completed>0,"not enough Process boundaries to anchor a complete pass");
        for(size_t i=0;i<paths.size();++i)Require(fs::file_size(paths[i])==manifest[i]["bytes"]&&fileHash(paths[i])==manifest[i]["sha256"],"source changed while reading");
        Json result={{"passed",true},{"source_kind",kind},{"source_files",manifest},{"source_unchanged",true},{"records",records},{"object_records",assembler.objectsSeen},
            {"process_markers",assembler.markersSeen},{"completed_quanta",assembler.completed},{"sample_clock_frames",clockEnd},
            {"raw_object_samples",samples},{"sealed_object_samples",emittedSamples},{"discarded_attach_objects",assembler.discardedAttachObjects},{"trailing_unsealed_objects",assembler.trailingObjects},
            {"empty_quanta",assembler.empty},{"unknown_offset_objects",assembler.unknownOffsetObjects},{"not_full_span_layout_quanta",assembler.notBridgeReady},
            {"objects_per_quantum_histogram",widths},{"input_frame_histogram",frameSizes},{"position_state_histogram",positionStates},
            {"all_object_pcm_sha256",allPcm.finish()},{"sealed_object_pcm_sha256",emittedPcm.finish()},{"state_snapshot_sha256",states.finish()},
            {"native_process_markers_observed",kind=="native-process-marker-probe"},{"sample_rate_verified",false},{"live_boundary_contract_verified",false},
            {"mat_encoded",false},{"scope","Marker grouping + relative integer sample cursor only. No 48kHz assumption, bridge submission, MAT execution or decoder claim."}};
        writeJson(out/L"summary.json",result);std::cout<<result.dump(2)<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';if(created){try{writeJson(out/L"failure.json",{{"failed",true},{"message",e.what()},{"live_boundary_contract_verified",false}});}catch(...){}}return 1;}
}
