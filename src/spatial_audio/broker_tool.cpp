// Opt-in offline broker harness. No deployment, elevation, hooks or audiodg.
#include "../../spatial-object-spike/mat-diagnostics/mat_replay_source.h"
#include "stream_ring_source.h"
#include "capture_reader_api.h"
#include "object_broker.h"
#include "../../spatial-object-spike/mat-spike/cpp_mat_ipc_support.hpp"
#include <bcrypt.h>
#include <thread>
#include <atomic>
#include <iostream>
namespace fs=std::filesystem;using Json=nlohmann::json;using Handle=mat_ipc::Handle;
static void require(bool b,const char* why){spatial_audio::require(b,why);}
static void save(const fs::path& p,const Json& j){mat_ipc::save(p,j);}
struct View {void* p{};View(HANDLE h,size_t n){p=MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,n);require(p!=nullptr,"map own session view");}~View(){if(p)UnmapViewOfFile(p);}};
static HANDLE mapping(size_t n){auto h=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(n),nullptr);require(h!=nullptr,"own mapping");return h;}
static uint64_t born(HANDLE p){return mat_ipc::creationTime(p);}
template<class T>static T symbol(HMODULE dll,const char* name){auto p=GetProcAddress(dll,name);static_assert(sizeof(p)==sizeof(T));T f{};memcpy(&f,&p,sizeof(f));return f;}
static void packetFile(std::ofstream& f,const mat_stream::Packet& p){mat_ipc::writePacket(f,p);}
class Session {
    Handle inputMap_{mapping(sizeof(receiver::Shared))},outputMap_{mapping(sizeof(spatial_stream::Output))};
    View iv_{inputMap_.get(),sizeof(receiver::Shared)},ov_{outputMap_.get(),sizeof(spatial_stream::Output)};
    receiver::Shared& input_=*static_cast<receiver::Shared*>(iv_.p);spatial_stream::Output& output_=*static_cast<spatial_stream::Output*>(ov_.p);
    Handle data_{CreateEventW(nullptr,FALSE,FALSE,nullptr)},cancel_{CreateEventW(nullptr,TRUE,FALSE,nullptr)},owner_;
    spatial_stream::Args args_{};spatial_stream::FakeConfig config_{sizeof(config_),spatial_stream::streamVersion,0,-1,-1,0,0};
    HMODULE dll_{};std::thread control_,consumer_;std::atomic<bool> returned_=false;DWORD code_=STILL_ACTIVE;
    bool joined_=false;std::string consumerError_;uint64_t consumed_=0;fs::path out_;uint64_t domain_;int scenario_=0;
public:
    Session(const fs::path& dll,uint64_t domain,uint64_t epoch,const fs::path& out,bool paced=false,int scenario=0):out_(out),domain_(domain),scenario_(scenario){
        memset(&input_,0,sizeof(input_));memset(&output_,0,sizeof(output_));HANDLE owner{};require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&owner,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)!=0,"own owner handle");owner_=Handle(owner);
        uint64_t nonce[2];require(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"session nonce");
        mat_ipc::initialize(*reinterpret_cast<mat_ipc::Shared*>(&input_),domain,epoch,nonce[0],nonce[1]);
        output_.magic=spatial_stream::streamMagic;output_.version=spatial_stream::streamVersion;output_.bytes=sizeof(output_);output_.capacity=spatial_stream::slots;output_.payloadBytes=spatial_stream::payloadCapacity;output_.format=spatial_stream::fakeFormat;output_.ownerPid=GetCurrentProcessId();output_.domain=domain;output_.epoch=epoch;output_.nonce[0]=nonce[0];output_.nonce[1]=nonce[1];
        auto& h=output_.header;h.createHr=h.mapperHr=h.initializeHr=h.submitHr=h.processHr=E_PENDING;
        args_={sizeof(args_),spatial_stream::streamVersion,inputMap_.get(),data_.get(),cancel_.get(),owner_.get(),outputMap_.get(),domain,epoch,{nonce[0],nonce[1]},born(GetCurrentProcess()),GetCurrentProcessId(),0,0,0};config_.paceMs=paced?10:0;
        if(scenario==1)config_.failInitialize=1;if(scenario==2)config_.failSubmitAt=3;if(scenario==3)config_.failProcessAt=4;if(scenario==4)config_.oversizedProcess=1;if(scenario==9)output_.format=spatial_stream::nativeFormat;
        dll_=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);require(dll_!=nullptr,"load own streaming receiver DLL");auto start=symbol<spatial_stream::FakeStart>(dll_,"TestMatStreamReceiverFake");auto native=symbol<spatial_stream::Start>(dll_,"StartMatStreamReceiver");require(start&&native,"separate fake/native streaming exports");
        // Diagnostics are streamed to a test file, never retained in the DLL.
        consumer_=std::thread([this]{try{std::ofstream f(out_/ ("domain-"+std::to_string(domain_)+".plan"),std::ios::binary);require(bool(f),"plan output");auto slot=std::make_unique<spatial_stream::OutputSlot>();auto deadline=GetTickCount64()+15000;
            for(;;){if(scenario_==5){require(GetTickCount64()<deadline,"parked output consumer bound");if(returned_.load()){consumerError_="intentional undrained output fixture";break;}Sleep(1);continue;}auto p=spatial_stream::next(output_,*slot,consumed_);if(p==spatial_stream::Poll::item){require(slot->header.format==spatial_stream::fakeFormat,"fake payload must stay diagnostic");require(slot->header.sampleStart==consumed_*480,"encoded carrier clock");f.write(reinterpret_cast<const char*>(slot->payload),slot->header.bytes);require(bool(f),"streamed transcript write");++consumed_;if(scenario_==6&&consumed_==3){receiver::put(output_.cancel,1);SetEvent(cancel_.get());}continue;}if(p==spatial_stream::Poll::ended)break;require(p!=spatial_stream::Poll::failed,"output ring failed");require(!returned_.load()||receiver::get(output_.completed),"receiver returned without completion");require(GetTickCount64()<deadline,"own consumer bound");Sleep(1);}f.close();require(bool(f),"close plan");}
            catch(const std::exception& e){consumerError_=e.what();receiver::put(output_.cancel,1);SetEvent(cancel_.get());SetEvent(data_.get());}});
        control_=std::thread([this,start,native]{code_=scenario_==9?native(&args_):start(&args_,&config_);returned_.store(true);});
    }
    ~Session(){if(!joined_){receiver::put(output_.cancel,1);SetEvent(cancel_.get());SetEvent(data_.get());if(control_.joinable())control_.join();if(consumer_.joinable())consumer_.join();}if(dll_)FreeLibrary(dll_);}
    bool push(const mat_stream::Packet& packet){
        auto deadline=GetTickCount64()+5000;while(receiver::get(input_.writer.write)-receiver::get(input_.reader.read)>=32){if(returned_.load()||receiver::get(input_.common.fault)||GetTickCount64()>deadline)return false;receiver::put(input_.writer.heartbeat,receiver::qpc());Sleep(1);}
        if(returned_.load()||receiver::get(input_.common.fault))return false;
        mat_ipc::Packet p=packet;bool okay=mat_ipc::send(*reinterpret_cast<mat_ipc::Shared*>(&input_),p,receiver::qpc());SetEvent(data_.get());return okay;
    }
    Json finish(bool expectComplete=true){require(!joined_,"single session finish");
        while(!returned_.load()){receiver::put(input_.writer.heartbeat,receiver::qpc());Sleep(1);}control_.join();consumer_.join();joined_=true;auto& h=output_.header;
        bool okay=code_==0&&h.status==2&&!h.error&&!receiver::get(input_.common.fault)&&consumerError_.empty()&&h.workerFinished&&!h.workerTimedOut&&h.records==consumed_&&receiver::get(output_.write)==receiver::get(output_.read)&&receiver::get(output_.completed)&&!receiver::get(output_.error);
        require(!h.vendorLoads&&!h.activations&&!h.hostGuardPassed&&!h.helperPinned&&!h.endpointCalls&&!h.hooksInstalled,"offline fake must not enter native path");
        Json result={{"completed",okay},{"domain",domain_},{"entry_return",code_},{"error",h.error},{"consumer_error",consumerError_},{"source_rounds",h.sourceRounds},{"warm_rounds",h.warmRounds},{"drain_rounds",h.drainRounds},{"submissions",h.submissions},{"source_samples",h.sourceSamples},{"output_blocks",h.records},{"consumed_blocks",consumed_},{"output_payload_bytes",h.outputBytes},{"ring_capacity",spatial_stream::slots},{"ring_mapping_bytes",sizeof(output_)},{"ring_wraps",consumed_/spatial_stream::slots},{"worker_finished",bool(h.workerFinished)},{"worker_timed_out",bool(h.workerTimedOut)},{"fake_calls_verified",h.fakeCallsVerified},{"native_encoder_called",false},{"mat_encoded",false}};save(out_/("domain-"+std::to_string(domain_)+".json"),result);require(okay==expectComplete,"stream session expected completion/failure");return result;
    }
};
static Json replay(const fs::path& input,const fs::path& out){
    spatial_audio::MatReplaySource source(input,101,1);require(source.start(),source.error().c_str());std::ofstream f(out/"replayed.raw",std::ios::binary);uint64_t count=0;
    for(;;){spatial_audio::Frame frame;auto status=source.read(frame);if(status==spatial_audio::Read::ended)break;require(status==spatial_audio::Read::frame,source.error().c_str());require(frame.epoch==101&&frame.domain==1&&frame.sequence==count&&frame.sample_start==count*960&&frame.duration_frames==960&&frame.clock==spatial_audio::Clock::carrier_48000,"M1 replay contract");f.write(reinterpret_cast<const char*>(frame.bytes.data()),frame.bytes.size());++count;}f.close();require(bool(f),"replay output");
    Json j={{"passed",true},{"frames",count},{"bytes",count*spatial_audio::carrier_bytes},{"sha256",mat_ipc::hashFile(out/"replayed.raw")},{"clock","carrier_48000_not_calibrated_source_PTS"},{"native_encoder_called",false}};save(out/"proof.json",j);return j;
}
static Json captureChild(int argc,char** argv){
    require(argc==13,"capture child args");auto adapter=fs::absolute(argv[2]),receiverDll=fs::absolute(argv[3]);Handle map(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[4])))),owner(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[5])))),ready(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[6]))));DWORD ownerPid=std::stoul(argv[7]);uint64_t ownerCreated=std::stoull(argv[8]);auto out=fs::absolute(argv[9]);bool paced=std::stoul(argv[10])!=0;std::string mode=argv[11];std::set<uint64_t> excludes;if(mode=="self-excluded")excludes.insert(std::stoull(argv[12]));
    require(!fs::exists(out)&&fs::create_directories(out),"new child output");require(WaitForSingleObject(ready.get(),5000)==WAIT_OBJECT_0,"own fixture ready");HMODULE dll=LoadLibraryExW(adapter.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);require(dll!=nullptr,"own MSVC capture adapter");
    auto create=symbol<spatial_capture::Create>(dll,"SpatialCaptureCreate");auto next=symbol<spatial_capture::Next>(dll,"SpatialCaptureNext");auto destroy=symbol<spatial_capture::Destroy>(dll,"SpatialCaptureDestroy");require(create&&next&&destroy,"fixed C ABI exports");spatial_capture::Args args{sizeof(args),1,map.get(),owner.get(),ownerPid,GetCurrentProcessId(),ownerCreated};void* reader{};require(create(&args,&reader)==0&&reader,"capture reader identity/layout");
    std::map<uint64_t,std::unique_ptr<Session>> sessions;std::map<uint64_t,std::ofstream> frames;Json results=Json::array();
    spatial_audio::ObjectBroker broker([&](const mat_stream::Packet& p){auto domain=p.domain;if(!sessions.contains(domain)){sessions[domain]=std::make_unique<Session>(receiverDll,domain,10000+domain,out,paced);frames[domain].open(out/("domain-"+std::to_string(domain)+".frames"),std::ios::binary);require(bool(frames[domain]),"bridge frame test tap");}packetFile(frames.at(domain),p);if(!sessions.at(domain)->push(p))return false;if(p.header.flags){results.push_back(sessions.at(domain)->finish());sessions.erase(domain);frames.at(domain).close();frames.erase(domain);}return true;},mode=="selected-none"?999:0,excludes);
    try{spatial_capture::Event event{sizeof(event),1,0,0,{}};auto deadline=GetTickCount64()+20000;
        for(;;){auto status=next(reader,&event);if(status==spatial_capture::ended)break;require(status!=spatial_capture::failed,"capture adapter failed");require(GetTickCount64()<deadline,"bounded own fixture test");if(status==spatial_capture::pending){Sleep(1);continue;}require(event.size>=sizeof(q4::Header)&&event.size<=spatial_capture::event_capacity,"capture C event bytes");q4::Header h;memcpy(&h,event.data,sizeof(h));require(event.size==sizeof(h)+h.audio.frames*4,"capture event exact size");std::array<float,kSamples> pcm{};if(h.audio.frames)memcpy(pcm.data(),event.data+sizeof(h),h.audio.frames*4);broker.push(h,pcm.data());}
        broker.finish();require(sessions.empty(),"all domains closed");destroy(reader);reader=nullptr;FreeLibrary(dll);
    }catch(const std::exception& e){save(out/"failure.json",{{"failed",true},{"message",e.what()}});if(reader)destroy(reader);FreeLibrary(dll);throw;}
    Json result={{"passed",true},{"capture_events",broker.events},{"source_rounds",broker.source_rounds},{"closed_domains",broker.closes},{"excluded_domains",broker.excluded_domains},{"domains",results},{"capture_args_bytes",sizeof(spatial_capture::Args)},{"capture_event_bytes",sizeof(spatial_capture::Event)},{"output_block_bytes",sizeof(spatial_stream::Block)},{"output_mapping_bytes",sizeof(spatial_stream::Output)},{"native_encoder_called",false},{"audiodg_accessed",false},{"capture_hook_installed",false},{"scope","Own MSVC ring fixture -> MSVC C ABI DLL in GCC broker -> incremental assembler/bridge -> MSVC streaming FAKE codec worker"}};save(out/"proof.json",result);return result;
}
static Json selftest(const fs::path& out){
    std::vector<std::string> tests;auto test=[&](const char* name,auto fn){fn();tests.push_back(name);};
    auto ring=std::make_unique<spatial_stream::Output>();auto reset=[&](uint32_t format=spatial_stream::fakeFormat){memset(ring.get(),0,sizeof(*ring));ring->magic=spatial_stream::streamMagic;ring->version=2;ring->bytes=sizeof(*ring);ring->capacity=8;ring->payloadBytes=spatial_stream::payloadCapacity;ring->format=format;ring->epoch=100;ring->domain=1;ring->ownerPid=GetCurrentProcessId();};reset();
    auto slot=std::make_unique<spatial_stream::OutputSlot>();spatial_stream::Block b{};b.phase=2;b.frames=480;b.bytes=3;BYTE p[]{1,2,3};
    test("output_ring_100000_wraps_byte_exact",[&]{for(uint64_t n=0;n<100000;++n){b.sampleStart=n*480;require(spatial_stream::publish(*ring,b,p),"ring publish");require(spatial_stream::next(*ring,*slot,n)==spatial_stream::Poll::item&&slot->header.sequence==n&&slot->header.sampleStart==n*480&&!memcmp(slot->payload,p,3),"ring copy/reclaim/wrap");}receiver::put(ring->completed,1);require(spatial_stream::next(*ring,*slot,100000)==spatial_stream::Poll::ended,"drained EOF");});
    test("full_output_ring_fails_without_overwrite",[&]{reset();for(int n=0;n<8;++n)require(spatial_stream::publish(*ring,b,p),"fill output");require(!spatial_stream::publish(*ring,b,p)&&receiver::get(ring->write)==8&&ring->slots[0].payload[0]==1,"full terminal/no overwrite");});
    test("bad_output_checksum_rejected",[&]{reset();require(spatial_stream::publish(*ring,b,p),"checksum fixture");ring->slots[0].payload[0]^=1;require(spatial_stream::next(*ring,*slot,0)==spatial_stream::Poll::failed&&receiver::get(ring->read)==0,"bad checksum not reclaimed");});
    test("diagnostic_ring_never_advertised_as_MAT",[&]{reset();spatial_audio::StreamRingSource source(*ring,100,1);require(!source.start(),"fake is not native MAT");});
    test("native_ring_two_halves_to_carrier_and_EOF",[&]{reset(1);std::vector<BYTE> bytes(61440);bytes[0]=0x72;bytes[1]=0xf8;bytes[2]=0x1f;bytes[3]=0x4e;bytes[4]=0x16;bytes[6]=2;b.bytes=30720;b.sampleStart=0;require(spatial_stream::publish(*ring,b,bytes.data()),"first half");b.sampleStart=480;require(spatial_stream::publish(*ring,b,bytes.data()+30720),"second half");receiver::put(ring->completed,1);spatial_audio::StreamRingSource source(*ring,100,1);require(source.start(),"native source start");spatial_audio::Frame f;require(source.read(f)==spatial_audio::Read::frame&&f.bytes==bytes&&f.sample_start==0&&f.duration_frames==960,"full native carrier");require(source.read(f)==spatial_audio::Read::ended,"full EOF");});
    test("partial_carrier_EOF_rejected",[&]{reset(1);std::vector<BYTE> bytes(30720);b.bytes=30720;b.sampleStart=0;require(spatial_stream::publish(*ring,b,bytes.data()),"half fixture");receiver::put(ring->completed,1);spatial_audio::StreamRingSource source(*ring,100,1);require(source.start(),"partial source start");spatial_audio::Frame f;require(source.read(f)==spatial_audio::Read::failed,"partial EOF fail");});
    test("source_epoch_mismatch_rejected",[&]{reset(1);spatial_audio::StreamRingSource source(*ring,101,1);require(!source.start(),"source epoch identity");});
    test("consumer_cancel_terminal",[&]{reset();receiver::put(ring->cancel,1);b.bytes=3;require(!spatial_stream::publish(*ring,b,p)&&receiver::get(ring->error)==receiver::cancelled,"cancel stops publication with cancellation, not overflow");});
    test("native_odd_length_flush_preserves_whole_carrier",[&]{require(spatial_stream::drainCount(326,false)==30&&spatial_stream::drainCount(327,false)==31&&spatial_stream::drainCount(327,true)==30,"native half-carrier padding is drain, never extra source Process");});
    test("output_counters_and_byte_totals_above_uint32",[&]{reset();uint64_t seed=uint64_t(1)<<32;receiver::put(ring->read,seed);receiver::put(ring->write,seed);ring->header.outputBytes=seed;b.bytes=3;b.sampleStart=seed*480;require(spatial_stream::publish(*ring,b,p)&&spatial_stream::next(*ring,*slot,seed)==spatial_stream::Poll::item&&slot->header.sequence==seed,"64-bit sequence across UINT32 boundary");ring->header.outputBytes+=3;require(ring->header.outputBytes==seed+3,"64-bit output byte statistics");});
    Json j={{"passed",true},{"checks",tests.size()},{"tests",tests},{"native_encoder_called",false},{"audiodg_accessed",false},{"output_mapping_bytes",sizeof(*ring)}};save(out/"proof.json",j);return j;
}
static Json streamTests(const fs::path& dll,const fs::path& out){
    Json cases=Json::array();
    for(int scenario: {0,1,2,3,4,5,6,9}){
        auto dir=out/("case-"+std::to_string(scenario));require(fs::create_directory(dir),"new streaming case");Session session(dll,1,20000+scenario,dir,false,scenario);auto p=std::make_unique<mat_stream::Packet>();
        unsigned count=scenario==0?1200:50;
        for(unsigned n=0;n<=count;++n){*p={};p->domain=1;p->header={n,480,n?1u:0u,n==count?1u:0u,receiver::qpc(),receiver::qpc()};
            if(n){auto& c=p->calls[0];c.header={1,0,1,n==count?8u:(n==1?7u:6u),{float(n%100)/100.f,0,0},480};if(n!=count){for(unsigned j=0;j<480;++j)c.pcm[j]=float((n+j)%100)/1000.f;}}
            if(!session.push(*p))break;
        }
        auto result=session.finish(scenario==0);if(scenario==0)require(result["source_rounds"]==1200&&result["output_blocks"]==1250&&result["submissions"]==1220&&result["source_samples"]==1199*480,"continuous receiver past old 350/400 cap");
        else{unsigned expected=scenario==5?6:(scenario==6?7:(scenario==9?10:13));require(result["error"]==expected&&!result["worker_timed_out"].get<bool>()&&(scenario==9||result["worker_finished"].get<bool>()),"streaming fault classification and worker return");}
        result["scenario"]=scenario;cases.push_back(result);
    }
    Json proof={{"passed",true},{"cases",cases},{"native_encoder_called",false},{"audiodg_accessed",false},{"guard_only_native_entry_in_own_process",true},{"long_case_source_rounds",1200}};save(out/"proof.json",proof);return proof;
}
int main(int argc,char** argv){try{
    require(avutil_version()==LIBAVUTIL_VERSION_INT,"existing FFmpeg ABI");Json result;
    if(argc==4&&std::string(argv[1])=="--replay"){fs::path out=fs::absolute(argv[3]);require(!fs::exists(out)&&fs::create_directories(out),"new replay output");result=replay(fs::absolute(argv[2]),out);}
    else if(argc==3&&std::string(argv[1])=="--selftest"){fs::path out=fs::absolute(argv[2]);require(!fs::exists(out)&&fs::create_directories(out),"new selftest output");result=selftest(out);}
    else if(argc==4&&std::string(argv[1])=="--stream-tests"){fs::path out=fs::absolute(argv[3]);require(!fs::exists(out)&&fs::create_directories(out),"new streaming test output");result=streamTests(fs::absolute(argv[2]),out);}
    else if(argc>1&&std::string(argv[1])=="--capture-child")result=captureChild(argc,argv);
    else throw std::runtime_error("Usage: --replay MAT_RAW NEW_OUT | --selftest NEW_OUT | --capture-child [own fixture only]");
    std::cout<<result.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 2;}}
