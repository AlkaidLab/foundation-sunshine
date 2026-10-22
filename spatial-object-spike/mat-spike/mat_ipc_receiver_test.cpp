#include "mat_ipc_receiver_api.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>
#include <stdexcept>
#include <iostream>
using namespace receiver;using Json=nlohmann::json;namespace fs=std::filesystem;
static void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
static void save(const fs::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2)<<'\n';require(bool(f),"JSON write");}
template<class T>static void write(std::ofstream& f,const T& t){f.write(reinterpret_cast<const char*>(&t),sizeof(t));require(bool(f),"binary write");}
struct Timer {HANDLE h=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);Timer(){require(h!=nullptr,"local timer");}~Timer(){CloseHandle(h);}void until(int64_t target){for(;;){auto left=target-qpc();if(left<=0)return;LARGE_INTEGER due{};due.QuadPart=-((left*10000000+hz()-1)/hz());require(SetWaitableTimer(h,&due,0,nullptr,nullptr,FALSE)!=0&&WaitForSingleObject(h,1000)==WAIT_OBJECT_0,"timer wait");}}};
struct Source {
    SourceHeader header{};std::vector<BYTE> raw;std::vector<size_t> offsets;
    explicit Source(const fs::path& p){require(fs::file_size(p)<16*1024*1024,"source size");std::ifstream f(p,std::ios::binary);require(bool(f),"source read");raw.assign(std::istreambuf_iterator<char>(f),{});require(raw.size()>=32,"source header");memcpy(&header,raw.data(),32);require(!memcmp(header.magic,"MATBRG1\0",8)&&header.version==1&&header.rate==48000&&header.frames==480&&header.records>=2&&header.records<=351,"source format");size_t at=32;for(uint32_t n=0;n<header.records;++n){require(at+32<=raw.size(),"source frame");SourceFrame h{};memcpy(&h,raw.data()+at,32);require(h.index==n&&h.frames==480&&h.calls<=64&&h.flags==uint32_t(n+1==header.records),"source sequence");offsets.push_back(at);at+=32+h.calls*sizeof(Call);require(at<=raw.size(),"source PCM");}require(at==raw.size(),"source EOF");}
    void copy(uint32_t n,Slot& slot){auto* p=raw.data()+offsets.at(n);SourceFrame h{};memcpy(&h,p,32);slot.header={frameMagic,uint32_t(sizeof(Message)+h.calls*sizeof(Call)),n,1,1,n,480,h.calls,h.flags?2u:1u,h.firstQpc,h.lastQpc,qpc(),0,0};memcpy(slot.calls,p+32,h.calls*sizeof(Call));}
};
struct Harness {
    HANDLE inputMap{},outputMap{},data{},cancel{},owner{},thread{};Shared* shared{};void* output{};Args args{};FakeConfig config{sizeof(FakeConfig),apiVersion,0,-1,-1,0};
    Start native{};FakeStart fake{};bool real=false;DWORD entryTid{};
    static DWORD WINAPI invoke(void* p){auto& h=*static_cast<Harness*>(p);return h.real?h.native(&h.args):h.fake(&h.args,&h.config);}
    Harness(HMODULE dll,bool realMode=false):real(realMode){native=reinterpret_cast<Start>(GetProcAddress(dll,"StartMatIpcReceiver"));fake=reinterpret_cast<FakeStart>(GetProcAddress(dll,"TestMatIpcReceiverFake"));require(native&&fake,"receiver exports");
        inputMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),nullptr);require(inputMap!=nullptr,"input mapping");shared=static_cast<Shared*>(MapViewOfFile(inputMap,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));require(shared!=nullptr,"input view");memset(shared,0,sizeof(Shared));
        shared->contract={magic,version,320,sizeof(Shared),sizeof(Slot),32,124968,sizeof(Call),1,1,{11,22},GetCurrentProcessId(),48000,480,0,hz()};put(shared->writer.heartbeat,qpc());
        size_t bytes=real?sizeof(NativeOutput):sizeof(FakeOutput);outputMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(bytes),nullptr);require(outputMap!=nullptr,"output mapping");output=MapViewOfFile(outputMap,FILE_MAP_ALL_ACCESS,0,0,bytes);require(output!=nullptr,"output view");auto& o=out();o.magic=outputMagic;o.version=apiVersion;o.bytes=static_cast<uint32_t>(bytes);o.kind=real?1:2;o.createHr=o.mapperHr=o.initializeHr=o.submitHr=o.processHr=E_PENDING;
        data=CreateEventW(nullptr,FALSE,FALSE,nullptr);cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(data&&cancel,"test events");require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&owner,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)!=0,"own owner handle");FILETIME a,b,c,d;require(GetProcessTimes(owner,&a,&b,&c,&d)!=0,"own creation time");
        args={sizeof(Args),apiVersion,inputMap,data,cancel,owner,outputMap,1,1,{11,22},(uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime,GetCurrentProcessId(),350,10,0};
    }
    ~Harness(){if(thread&&WaitForSingleObject(thread,0)!=WAIT_OBJECT_0){put(shared->common.cancel,1);SetEvent(cancel);SetEvent(data);WaitForSingleObject(thread,2000);}if(thread)CloseHandle(thread);if(shared)UnmapViewOfFile(shared);if(output)UnmapViewOfFile(output);for(auto h:{inputMap,outputMap,data,cancel,owner})if(h)CloseHandle(h);}
    OutputHeader& out(){return *static_cast<OutputHeader*>(output);}
    void start(){thread=CreateThread(nullptr,0,invoke,this,0,&entryTid);require(thread!=nullptr,"own receiver entry thread");}
    bool publish(Slot& slot){auto w=get(shared->writer.write),r=get(shared->reader.read);if(get(shared->common.fault)||get(shared->common.cancel))return false;require(w-r<32,"test producer unexpectedly full");slot.header.sequence=w;slot.header.domain=args.domain;slot.header.epoch=args.epoch;slot.header.enqueuedQpc=qpc();slot.header.checksum=checksum(slot.header,slot.calls);auto& dest=shared->slots[w%32];memcpy(&dest,&slot,sizeof(Slot));put(shared->writer.heartbeat,qpc());put(shared->writer.write,w+1);if(slot.header.kind==2)put(shared->common.producerDone,1);SetEvent(data);return true;}
    DWORD join(){auto until=GetTickCount64()+12000;while(WaitForSingleObject(thread,20)!=WAIT_OBJECT_0&&GetTickCount64()<until)put(shared->writer.heartbeat,qpc());require(WaitForSingleObject(thread,0)==WAIT_OBJECT_0,"entry did not exit; no forced termination");DWORD code{};GetExitCodeThread(thread,&code);return code;}
    Json report(){auto& o=out();return {{"status",o.status},{"error",o.error},{"worker_tid",o.workerTid},{"entry_tid",entryTid},{"producer_tid",GetCurrentThreadId()},{"worker_finished",bool(o.workerFinished)},{"worker_timed_out",bool(o.workerTimedOut)},{"host_guard_passed",bool(o.hostGuardPassed)},
        {"vendor_loads",o.vendorLoads},{"activations",o.activations},{"initializations",o.initializations},{"submissions",o.submissions},{"processes",o.processes},{"source_rounds",o.sourceRounds},{"warm_rounds",o.warmRounds},{"drain_rounds",o.drainRounds},{"source_samples",o.sourceSamples},
        {"messages",o.messages},{"eof_seen",bool(o.eofSeen)},{"fake_calls_verified",o.fakeCallsVerified},{"create_hr",o.createHr},{"mapper_hr",o.mapperHr},{"initialize_hr",o.initializeHr},{"submit_hr",o.submitHr},{"process_hr",o.processHr},{"local_high_water",o.localHighWater},
        {"records",o.records},{"output_bytes",o.outputBytes},{"endpoint_calls",o.endpointCalls},{"hooks_installed",o.hooksInstalled},{"helper_pinned",bool(o.helperPinned)},{"native_encoder_called",false},{"mat_encoded",false}};}
    void plan(const fs::path& path){auto& o=out();require(!real&&o.processes<=400,"fake output bounds");auto* s=static_cast<FakeOutput*>(output);std::ofstream f(path,std::ios::binary);for(uint32_t n=0;n<o.processes;++n){auto& t=s->ticks[n];require(t.count<=64,"fake calls bound");write(f,t.phase);write(f,t.sourceIndex);write(f,t.count);for(uint32_t j=0;j<t.count;++j){auto& c=t.calls[j];write(f,c.id);write(f,c.flags);write(f,c.xyz);write(f,c.pcm);}}f.close();require(bool(f),"fake plan close");}
};
static std::unique_ptr<Slot> sampleSlot(uint32_t n,uint32_t kind=1,uint32_t flags=7){auto p=std::make_unique<Slot>();p->header={frameMagic,sizeof(Message)+sizeof(Call),n,1,1,n,480,1,kind,0,0,qpc(),0,0};p->calls[0]={1,0,1,flags,{.25f,.5f,.75f},480,{}};if(flags!=8)for(auto& v:p->calls[0].pcm)v=.125f;return p;}
static Json tests(HMODULE dll){std::vector<std::string> names;auto test=[&](const char* name,auto fn){fn();names.emplace_back(name);};
    test("standalone_wire_layout_matches_frozen_IPC_v1",[]{require(sizeof(Shared)==4002112&&sizeof(Slot)==125056&&sizeof(Call)==1952&&sizeof(Message)==80,"ABI drift");});
    test("native_entry_rejects_ordinary_host_before_vendor_calls",[&]{Harness h(dll,true);h.start();require(h.join()==ERROR_BAD_ENVIRONMENT&&h.out().status==-1&&!h.out().hostGuardPassed&&!h.out().vendorLoads&&!h.out().activations&&!h.out().initializations&&!h.out().workerTid&&!h.out().helperPinned,"host guard bypass");});
    test("fake_unity_path_warmup_source_EOF_drain_and_own_worker",[&]{Harness h(dll);auto p=sampleSlot(0),e=sampleSlot(1,2,8);h.start();require(h.publish(*p)&&h.publish(*e),"enqueue");require(h.join()==0&&h.out().status==2&&h.out().workerFinished&&h.out().workerTid!=GetCurrentThreadId()&&h.out().workerTid!=h.entryTid&&h.out().processes==51&&h.out().sourceRounds==1&&h.out().warmRounds==20&&h.out().drainRounds==30&&h.out().fakeCallsVerified==22&&h.out().sourceSamples==480&&!h.out().vendorLoads&&!h.out().records&&!h.out().outputBytes,"fake lifecycle");});
    test("20_distinct_PCM_buffers_borrowed_until_Process_returns",[&]{Harness h(dll);auto p=sampleSlot(0),e=sampleSlot(1,2,8);p->header.calls=e->header.calls=20;p->header.bytes=e->header.bytes=sizeof(Message)+20*sizeof(Call);for(uint32_t n=0;n<20;++n){p->calls[n]=p->calls[0];p->calls[n].id=n+1;for(auto& f:p->calls[n].pcm)f=(n+1)*.001f;e->calls[n]=p->calls[n];e->calls[n].flags=8;memset(e->calls[n].pcm,0,sizeof(e->calls[n].pcm));}h.start();require(h.publish(*p)&&h.publish(*e)&&h.join()==0&&h.out().fakeCallsVerified==440&&h.out().sourceSamples==9600,"PCM pointer alias/lifetime");});
    test("native_entry_cannot_use_fake_output_mapping",[&]{Harness h(dll);h.real=true;h.start();require(h.join()!=0&&!h.out().vendorLoads&&!h.out().workerTid,"native used fake output");});
    test("invalid_source_duration_rejected_before_worker",[&]{Harness h(dll);h.args.maxSourceRounds=351;h.start();require(h.join()==ERROR_INVALID_PARAMETER&&!h.out().workerTid&&!h.out().vendorLoads,"duration bounds");});
    test("fake_config_size_rejected_without_worker",[&]{Harness h(dll);--h.config.bytes;h.start();require(h.join()==ERROR_INVALID_PARAMETER&&!h.out().workerTid&&!h.out().vendorLoads,"fake config bounds");});
    test("owner_creation_time_mismatch_rejected_before_worker",[&]{Harness h(dll);++h.args.ownerCreationTime;h.start();require(h.join()!=0&&h.out().error==badContract&&!h.out().workerTid&&!h.out().vendorLoads,"owner identity");});
    test("non_waitable_data_handle_fails_without_spinning",[&]{Harness h(dll);h.args.data=h.inputMap;h.start();require(h.join()!=0&&h.out().error==badContract&&h.out().workerFinished&&!h.out().workerTimedOut,"bad event handle");});
    test("cancel_event_alone_propagates_cooperative_cancel",[&]{Harness h(dll);SetEvent(h.cancel);h.start();require(h.join()!=0&&h.out().error==cancelled&&!h.out().workerTimedOut,"event cancel ignored");});
    test("idle_input_deadline_exits_without_forced_termination",[&]{Harness h(dll);h.args.maxSeconds=1;h.start();require(h.join()!=0&&h.out().error==timeout&&h.out().workerFinished&&!h.out().workerTimedOut,"deadline ignored");});
    for(const auto& mode:std::vector<std::string>{"contract","checksum","duplicate","nonfinite","index_gap","missing_live","missing_EOF","initialize_S_FALSE","submit_E_ABORT","Process_S_FALSE","oversized_Process","cancel"}){
        test(mode.c_str(),[&]{Harness h(dll);auto p=sampleSlot(0),e=sampleSlot(1,2,8);
            if(mode=="contract")++h.shared->contract.version;
            if(mode=="duplicate"){p->header.calls=2;p->header.bytes=sizeof(Message)+2*sizeof(Call);p->calls[1]=p->calls[0];}
            if(mode=="nonfinite"){uint32_t nan=0x7fc00000;memcpy(&p->calls[0].xyz[0],&nan,4);}
            if(mode=="index_gap")p->header.index=1;
            if(mode=="initialize_S_FALSE")h.config.failInitialize=1;if(mode=="submit_E_ABORT")h.config.failSubmitAt=0;if(mode=="Process_S_FALSE")h.config.failProcessAt=0;if(mode=="oversized_Process")h.config.oversizedProcess=1;
            if(mode=="checksum"){auto& slot=h.shared->slots[0];slot=*p;slot.header.checksum=checksum(slot.header,slot.calls)^1;put(h.shared->writer.write,1);SetEvent(h.data);}
            h.start();if(mode!="checksum"&&mode!="contract"){h.publish(*p);if(mode=="missing_live"){e->header.kind=1;e->header.calls=0;e->header.bytes=sizeof(Message);h.publish(*e);put(h.shared->common.producerDone,1);SetEvent(h.data);}else if(mode=="missing_EOF"){put(h.shared->common.producerDone,1);SetEvent(h.data);}else if(mode=="cancel"){put(h.shared->common.cancel,1);SetEvent(h.cancel);}else h.publish(*e);}
            auto result=h.join();require(result!=0&&h.out().status==-1&&!h.out().vendorLoads&&!h.out().activations&&!h.out().helperPinned,"bad stream accepted or vendor called");
            if(mode=="initialize_S_FALSE")require(h.out().initializeHr==S_FALSE&&h.out().processes==0,"init HRESULT ignored");
            if(mode=="submit_E_ABORT")require(h.out().submitHr==E_ABORT&&h.out().submissions==0,"submit HRESULT ignored");
            if(mode=="Process_S_FALSE")require(h.out().processHr==S_FALSE&&h.out().processes==0,"Process HRESULT ignored");
        });
    }
    require(!GetModuleHandleW(L"DolbyMatEnc.dll")&&!GetModuleHandleW(L"DolbyMatEngine.dll"),"proprietary DLL unexpectedly loaded");
    return {{"passed",true},{"tests",names},{"selftests_passed",names.size()},{"map_bytes",sizeof(Shared)},{"slot_bytes",sizeof(Slot)},{"message_bytes",sizeof(Message)},{"call_bytes",sizeof(Call)},{"fake_output_bytes",sizeof(FakeOutput)},{"native_output_bytes",sizeof(NativeOutput)},{"native_encoder_called",false},{"vendor_loaded",false},{"process_attached",false}};
}
int wmain(int argc,wchar_t** argv){try{
    bool self=argc==4&&!wcscmp(argv[1],L"--selftest"),replay=argc==5&&!wcscmp(argv[1],L"--replay");require(self||replay,"Usage: --selftest OWN_DLL NEW_OUT | --replay OWN_DLL BRIDGE_EXPORT NEW_OUT");
    fs::path out=self?argv[3]:argv[4];require(!fs::exists(out)&&fs::create_directories(out),"new output required");auto dll=LoadLibraryW(argv[2]);require(dll!=nullptr,"load only own DLL");
    if(self){auto result=tests(dll);save(out/L"proof.json",result);std::cout<<result.dump(2)<<'\n';}
    else{std::ifstream f(fs::path(argv[3])/L"summary.json");Json golden;f>>golden;require(golden["offline_bridge_replay_passed"]==true,"bridge proof required");Json domains=Json::array();
        for(const auto& d:golden["domains"]){auto domain=d["domain"].get<uint64_t>();auto name=L"domain-"+std::to_wstring(domain);auto dir=out/name;require(fs::create_directory(dir),"domain output");Source source(fs::path(argv[3])/d["file"].get<std::string>());Harness h(dll);h.args.domain=domain;h.args.epoch=100+domain;h.args.maxSourceRounds=source.header.records-1;h.shared->contract.domain=domain;h.shared->contract.epoch=h.args.epoch;h.start();auto p=std::make_unique<Slot>();Timer timer;auto origin=qpc()+hz()/200;
            for(uint32_t n=0;n<source.header.records;++n){timer.until(origin+int64_t(n)*hz()/100);source.copy(n,*p);require(h.publish(*p),"producer rejected, no retry");}
            auto code=h.join();auto result=h.report();result["domain"]=domain;result["epoch"]=h.args.epoch;result["entry_return"]=code;result["completed"]=code==0&&h.out().status==2;result["producer_backpressure"]=false;require(result["completed"].get<bool>(),"fake replay failed");h.plan(dir/L"fake-plan.bin");save(dir/L"summary.json",result);domains.push_back(result);
        }require(!GetModuleHandleW(L"DolbyMatEnc.dll")&&!GetModuleHandleW(L"DolbyMatEngine.dll"),"vendor loaded in fake replay");save(out/L"proof.json",{{"passed",true},{"domains",domains},{"native_encoder_called",false},{"vendor_loaded",false},{"process_attached",false}});
    }
    FreeLibrary(dll);return 0;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 1;}}
