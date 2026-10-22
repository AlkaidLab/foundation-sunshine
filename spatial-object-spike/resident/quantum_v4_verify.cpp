#define NOMINMAX
#include "quantum_v4_assembler.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <regex>
#include <iostream>
using Json4=nlohmann::json;
namespace fs=std::filesystem;
static void check4(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
class Sha4 {
    BCRYPT_ALG_HANDLE algorithm_{};BCRYPT_HASH_HANDLE hash_{};std::vector<UCHAR> work_;
public:
    Sha4(){check4(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"SHA provider");DWORD size=0,got=0;check4(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&got,0)==0,"SHA size");work_.resize(size);check4(BCryptCreateHash(algorithm_,&hash_,work_.data(),size,nullptr,0,0)==0,"SHA create");}
    ~Sha4(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    void add(const void* data,size_t count){check4(count<=ULONG_MAX,"SHA bounds");if(count)check4(BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(data)),static_cast<ULONG>(count),0)==0,"SHA input");}
    std::string finish(){UCHAR hash[32]{};check4(BCryptFinishHash(hash_,hash,32,0)==0,"SHA finish");std::ostringstream text;for(auto b:hash)text<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return text.str();}
};
static std::string hash4(const fs::path& path){std::ifstream in(path,std::ios::binary);check4(bool(in),"hash open");Sha4 sha;char bytes[65536];while(in){in.read(bytes,sizeof(bytes));sha.add(bytes,static_cast<size_t>(in.gcount()));}check4(in.eof(),"hash I/O");return sha.finish();}
static Json4 read4(const fs::path& path){std::ifstream in(path);check4(bool(in),"JSON input");Json4 value;in>>value;return value;}
static void write4(const fs::path& path,const Json4& value){std::ofstream out(path);check4(bool(out),"JSON output");out<<value.dump(2)<<'\n';check4(bool(out),"JSON write");}
int wmain(int argc,wchar_t** argv) {
    fs::path out;bool created=false;
    try {
        check4(argc==3,"Usage: quantum-v4-verify SOURCE_DIRECTORY NEW_OUTPUT (read-only; no process access)");
        fs::path root=fs::absolute(argv[1]);out=fs::absolute(argv[2]);auto format=read4(root/L"format.json");
        check4(format["version"]==4&&format["header_bytes"]==160&&format["pcm_stage"]=="before-original-submit","requires v4 pre-submit events; never promote v3 post-gain PCM");
        std::string kind=format.at("source_kind");bool native=kind=="native-pre-submit-paired";
        check4(native||kind=="synthetic-mutating-originals","unknown v4 source provenance");
        auto source=read4(root/(native?L"summary.json":L"expected.json"));
        if(native)check4(source["status"]==2&&source["error"]==0&&source["original_bytes_restored"]==true&&source["dropped_full"]==0
            &&source["dropped_busy"]==0&&source["invalid"]==0&&source["extended_calls"]==0&&source["unbound_after_arming"]==0,"native integrity/restoration failed");
        check4(!fs::exists(out)&&fs::create_directories(out),"output must be new");created=true;
        std::vector<fs::path> paths;
        if(!native)paths.push_back(root/L"events.bin");
        else {
            paths.push_back(root/L"global-objects.bin");std::map<unsigned,fs::path> chunks;std::regex pattern("global-objects-([0-9]+)[.]bin");
            for(auto& entry:fs::directory_iterator(root)){std::smatch match;auto name=entry.path().filename().string();if(entry.is_regular_file()&&std::regex_match(name,match,pattern))check4(chunks.emplace(std::stoul(match[1]),entry.path()).second,"duplicate source chunk");}
            unsigned next=1;for(const auto& entry:chunks){check4(entry.first==next++,"missing source chunk");paths.push_back(entry.second);}
        }
        Json4 manifest=Json4::array();uint64_t bytes=0;for(auto& path:paths){auto count=fs::file_size(path);bytes+=count;manifest.push_back({{"file",path.filename().string()},{"bytes",count},{"sha256",hash4(path)}});}
        q4::Assembler parser;Sha4 before,sealed;uint64_t records=0,samples=0;std::map<uint64_t,uint64_t> domainFrames;
        std::ofstream quanta(out/L"quanta.csv"),objects(out/L"objects.csv");check4(bool(quanta)&&bool(objects),"CSV open");
        quanta<<"domain,pass,sample_start,frames,objects,full_span_layout_ready\n";
        objects<<"domain,pass,call,host,object_id,position_state,generation,x,y,z,volume,call_gain,offset\n"<<std::setprecision(9);
        for(auto& path:paths){std::ifstream in(path,std::ios::binary);check4(bool(in),"event open");for(;;){q4::Header header{};in.read(reinterpret_cast<char*>(&header),sizeof(header));auto got=in.gcount();if(!got){check4(in.eof(),"event I/O");break;}check4(got==sizeof(header)&&header.audio.frames<=kSamples,"v4 short header/bounds");
            std::array<float,kSamples> pcm{};in.read(reinterpret_cast<char*>(pcm.data()),header.audio.frames*4);check4(in.gcount()==header.audio.frames*4,"v4 short PCM");++records;
            if(header.kind==q4::ObjectBefore){before.add(pcm.data(),header.audio.frames*4);samples+=header.audio.frames;}
            q4::Quantum q;if(parser.push(header,pcm.data(),q)){
                check4(domainFrames[q.domain]==q.sampleStart,"v4 per-domain clock gap");domainFrames[q.domain]+=q.frames;
                quanta<<q.domain<<','<<q.pass<<','<<q.sampleStart<<','<<q.frames<<','<<q.objects.size()<<','<<q.fullSpanLayoutReady<<'\n';
                for(const auto& object:q.objects){const auto& s=object.state;sealed.add(object.pcm.data(),object.pcm.size()*4);
                    objects<<q.domain<<','<<q.pass<<','<<object.source.call<<','<<s.audio.host<<','<<s.audio.objectId<<','<<s.positionState<<','<<s.generation<<','<<s.audio.x<<','<<s.audio.y<<','<<s.audio.z<<','<<s.audio.volume<<','<<s.audio.callGain<<','<<object.offset<<'\n';}
            }
        }}
        parser.finish();check4(parser.quanta>0,"no completed passes");quanta.close();objects.close();check4(bool(quanta)&&bool(objects),"CSV close");
        if(native)check4(source["blocks"]==records&&source["frames"]==samples&&source["bytes"]==bytes&&source["domains_opened"]==parser.opened&&source["domains_closed"]==parser.ended&&source["submit_results"]==parser.results&&source["process_results"]==parser.markers,"native footer counters");
        else check4(source["records"]==records&&source["domains"]==parser.opened&&source["quanta"]==parser.quanta&&source["objects"]==parser.objects,"fixture expected counters");
        for(size_t i=0;i<paths.size();++i)check4(fs::file_size(paths[i])==manifest[i]["bytes"]&&hash4(paths[i])==manifest[i]["sha256"],"source changed");
        Json4 report={{"passed",true},{"source_kind",kind},{"source_files",manifest},{"source_unchanged",true},{"records",records},{"object_samples",samples},
            {"domains_opened",parser.opened},{"domains_closed",parser.ended},{"submit_pairs",parser.results},{"process_markers",parser.markers},{"completed_quanta",parser.quanta},
            {"domain_sample_frames",domainFrames},{"empty_quanta",parser.empty},{"unknown_offset_objects",parser.unknownOffsets},{"not_full_span_layout_quanta",parser.unreadyQuanta},{"domain_end_reasons",parser.endReasons},
            {"pcm_before_in_event_order_sha256",before.finish()},{"pcm_sealed_in_completion_order_sha256",sealed.finish()},
            {"native_source_observed",native},{"native_ungained_source_verified",false},{"native_v4_verified",false},{"cross_domain_sample_sync_verified",false},{"mat_encoded",false},
            {"scope","Paired pre-call snapshots, explicit domain lifecycle, and per-domain input clocks only. Separate native source/gain proof still required."}};
        write4(out/L"summary.json",report);std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';if(created){try{write4(out/L"failure.json",{{"failed",true},{"message",e.what()}});}catch(...){}}return 1;}
}
