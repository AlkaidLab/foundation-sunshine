#include "cpp_mat_ipc_support.hpp"
#include <bcrypt.h>
using namespace mat_ipc;
int runSelftests();int runNative4Tests();int runMatStreamTests();int runMatStartupTests();int runIpcTests();
static HANDLE handle(const char* s){return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(s)));}
static uint64_t num(const char* s){return std::stoull(s);}
static bool corruptedFirstPublish(Shared& s,const Packet& p,int64_t when,const std::string& mode){
    // Negative fixture builds the wrong message BEFORE publishing write=1.
    // Never race a legitimate receiver by mutating a published payload.
    check(get(s.writer.write)==0&&get(s.reader.read)==0&&p.header.calls<=64,"negative fixture initial state");
    auto& slot=s.slots[0];Message m{frameMagic,static_cast<uint32_t>(sizeof(Message)+p.header.calls*sizeof(Call)),0,s.contract.domain,s.contract.epoch,
        p.header.index,480,p.header.calls,1,p.header.firstQpc,p.header.lastQpc,when,0,0};
    if(m.calls)memcpy(slot.calls,p.calls.data(),m.calls*sizeof(Call));
    if(mode=="contract-change")++s.contract.version;
    if(mode=="frame-epoch")++m.epoch;if(mode=="frame-length")m.bytes=1;if(mode=="sequence")++m.sequence;
    m.checksum=checksum(m,slot.calls);if(mode=="checksum")m.checksum^=1;slot.header=m;put(s.writer.heartbeat,when);put(s.writer.write,1);return true;
}
struct Monitor {
    std::atomic<bool> stop{false};std::thread worker;
    ~Monitor(){stop.store(true);if(worker.joinable())worker.join();}
};
static int receiver(int argc,char** argv){
    check(argc==17,"receiver argument count");
    Handle map(handle(argv[2])),data(handle(argv[3])),cancel(handle(argv[4])),ready(handle(argv[5])),owner(handle(argv[6]));
    uint64_t domain=num(argv[7]),epoch=num(argv[8]),n0=num(argv[9]),n1=num(argv[10]);DWORD ownerPid=static_cast<DWORD>(num(argv[11]));auto ownerCreated=num(argv[12]);
    auto out=fs::absolute(argv[13]);std::string mode=argv[14];unsigned records=static_cast<unsigned>(num(argv[15]));auto sourceHz=num(argv[16]);
    check(records>=2&&records<=1000&&sourceHz>0,"receiver source dimensions");check(!fs::exists(out)&&fs::create_directory(out),"new receiver output");
    View view(map.get());auto& s=view.get();auto guard=validate(s.contract,domain,epoch,n0,n1,ownerPid);
    if(guard==Error::none&&(GetProcessId(owner.get())!=ownerPid||creationTime(owner.get())!=ownerCreated))guard=Error::identity;
    if(guard!=Error::none){fail(s,guard);put(s.common.consumerStatus,static_cast<LONG>(Status::failed));SetEvent(ready.get());save(out/"summary.json",{{"completed",false},{"fault",static_cast<LONG>(guard)},{"guard_passed",false},{"encoder_opens",0},{"native_encoder_called",false},{"consumer_pid",GetCurrentProcessId()}});return 2;}
    check(InterlockedCompareExchange(&s.common.consumerPid,static_cast<LONG>(GetCurrentProcessId()),0)==0,"consumer already claimed");
    put(s.reader.heartbeat,qpc());put(s.common.consumerStatus,static_cast<LONG>(Status::attached));SetEvent(ready.get());
    if(mode=="consumer-exit"){
        save(out/"abrupt-exit.json",{{"intentional_own_helper_exit",true},{"code",31},{"consumer_pid",GetCurrentProcessId()},{"native_encoder_called",false}});ExitProcess(31);
    }
    if(mode=="overflow"||mode=="consumer-stale"){
        // Deliberately parked OWN fixture, not a hung third-party process.
        HANDLE waits[]{cancel.get(),owner.get()};auto result=WaitForMultipleObjects(2,waits,FALSE,3000);
        if(result==WAIT_OBJECT_0+1)fail(s,Error::producerExited);
        if(result==WAIT_TIMEOUT)fail(s,Error::timeout);
        put(s.common.consumerStatus,static_cast<LONG>(Status::failed));
        save(out/"summary.json",{{"completed",false},{"fault",static_cast<LONG>(error(s))},{"guard_passed",true},{"encoder_opens",0},{"native_encoder_called",false},{"consumer_pid",GetCurrentProcessId()}});return 2;
    }
    std::ofstream exported(out/"source.bin",std::ios::binary);write(exported,fileHeader("MATBRG1",records,sourceHz));
    TimedFake encoder(mode=="delayed");
    auto pipe=std::make_unique<Pipeline>(domain,encoder,Options{false,20,30},[&](const Packet& p){writePacket(exported,p);});
    Monitor monitor;monitor.worker=std::thread([&]{
        while(!monitor.stop.load()){
            put(s.reader.heartbeat,qpc());
            if(WaitForSingleObject(owner.get(),0)==WAIT_OBJECT_0)fail(s,Error::producerExited);
            else if(qpc()-get(s.writer.heartbeat)>hz())fail(s,Error::producerStale);
            if(error(s)!=Error::none||get(s.common.cancel)){pipe->cancel();SetEvent(cancel.get());SetEvent(data.get());}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    auto packet=std::make_unique<Packet>();bool eof=false;uint64_t consumed=0;Metrics metrics;std::string detail;
    std::ofstream ingress(out/"ingress.csv");ingress<<"index,kind,enqueued_qpc,received_qpc\n";
    try{
        put(s.common.consumerStatus,static_cast<LONG>(Status::streaming));
        for(;;){
            auto currentGuard=validate(s.contract,domain,epoch,n0,n1,ownerPid);
            if(currentGuard!=Error::none){fail(s,currentGuard);throw std::runtime_error("contract changed after attach");}
            int64_t enqueued=0;auto state=receive(s,*packet,enqueued);
            if(state==Poll::failed)throw std::runtime_error("IPC fault/cancel");
            if(state==Poll::empty){
                // Re-read publication after observing done: a final push can
                // race the first empty observation, exactly as in local SPSC.
                if(get(s.common.producerDone)&&get(s.writer.write)==get(s.reader.read)){fail(s,Error::missingEOF);throw std::runtime_error("missing EOF");}
                HANDLE waits[]{data.get(),cancel.get(),owner.get()};auto result=WaitForMultipleObjects(3,waits,FALSE,100);
                if(result==WAIT_OBJECT_0+2)fail(s,Error::producerExited);
                check(result!=WAIT_FAILED,"IPC event wait");continue;
            }
            ingress<<packet->header.index<<','<<(packet->header.flags?2:1)<<','<<enqueued<<','<<qpc()<<'\n';++consumed;
            check(pipe->pushOffline(*packet),"encoder worker unavailable");
            if(packet->header.flags){eof=true;put(s.common.consumerStatus,static_cast<LONG>(Status::draining));break;}
        }
    }catch(const std::exception& e){detail=e.what();if(error(s)==Error::none)fail(s,Error::codec);pipe->cancel();}
    metrics=pipe->join();monitor.stop.store(true);monitor.worker.join();
    bool complete=eof&&metrics.completed&&error(s)==Error::none;
    if(!complete&&error(s)==Error::none)fail(s,Error::codec);
    if(complete)writePacket(exported,*packet);exported.close();ingress.close();check(bool(exported)&&bool(ingress),"receiver outputs");
    savePlan(out/"fake-plan.bin",encoder);
    std::ofstream clock(out/"process-clock.csv");clock<<"phase,source_index,start_qpc,finish_qpc\n";for(const auto& t:encoder.times)clock<<static_cast<unsigned>(t.tick.phase)<<','<<t.tick.sourceIndex<<','<<t.start<<','<<t.finish<<'\n';clock.close();
    put(s.common.consumerStatus,static_cast<LONG>(complete?Status::completed:Status::failed));
    save(out/"summary.json",{{"completed",complete},{"fault",static_cast<LONG>(error(s))},{"detail",detail},{"guard_passed",true},{"consumer_pid",GetCurrentProcessId()},
        {"domain",domain},{"epoch",epoch},{"eof_seen",eof},{"messages_consumed",consumed},{"source_process_rounds",metrics.sourceRounds},{"source_samples",metrics.sourceSamples},
        {"preroll_rounds",metrics.prerollRounds},{"drain_rounds",metrics.drainRounds},{"submissions",metrics.submissions},{"scheduled_rounds",encoder.fake.blocks.size()},
        {"encoder_opens",encoder.fake.opens},{"encoder_closes",encoder.fake.closes},{"encoder_completed",encoder.fake.completed},{"queue_capacity",8},{"queue_high_water",pipe->highWater()},
        {"fake_plan_sha256",hashFile(out/"fake-plan.bin")},{"source_sha256",hashFile(out/"source.bin")},{"qpc_frequency",hz()},{"process_clock_rebases",encoder.rebases},
        {"native_encoder_called",false},{"mat_encoded",false},{"live_capture_verified",false}});
    return complete?0:2;
}
static int producer(int argc,char** argv){
    check(argc==7,"Usage: --produce SOURCE DOMAIN EPOCH NEW_OUTPUT MODE");
    Source source(fs::absolute(argv[2]));uint64_t domain=num(argv[3]),epoch=num(argv[4]);auto out=fs::absolute(argv[5]);std::string mode=argv[6];
    const std::set<std::string> allowed{"normal","delayed","header-version","contract-change","identity","frame-epoch","frame-length","checksum","sequence","overflow","cancel","owner-exit","consumer-exit","owner-stale","consumer-stale","missing-eof"};
    check(allowed.contains(mode)&&domain&&epoch,"producer config");check(!fs::exists(out)&&fs::create_directories(out),"new producer output");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle map(CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(Shared),nullptr));check(map.get()!=nullptr,"unnamed mapping create");View view(map.get());auto& s=view.get();
    uint64_t nonce[2];check(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"nonce generation");initialize(s,domain,epoch,nonce[0],nonce[1]);
    Handle data(CreateEventW(&sa,FALSE,FALSE,nullptr)),cancel(CreateEventW(&sa,TRUE,FALSE,nullptr)),ready(CreateEventW(&sa,TRUE,FALSE,nullptr));check(data.get()&&cancel.get()&&ready.get(),"IPC events");
    HANDLE duplicate{};check(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&duplicate,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)!=0,"owner liveness handle");Handle owner(duplicate);
    auto ownerCreated=creationTime(GetCurrentProcess());
    if(mode=="header-version")++s.contract.version;
    auto argHandle=[](HANDLE h){return number(reinterpret_cast<uintptr_t>(h));};
    std::vector<std::wstring> args{L"--receive",argHandle(map.get()),argHandle(data.get()),argHandle(cancel.get()),argHandle(ready.get()),argHandle(owner.get()),number(domain),number(epoch),number(nonce[0]+(mode=="identity")),number(nonce[1]),number(GetCurrentProcessId()),number(ownerCreated),(out/"receiver").wstring(),widen(mode),number(source.header.records),number(source.header.qpcHz)};
    auto child=spawn(args,{map.get(),data.get(),cancel.get(),ready.get(),owner.get()});
    save(out/"launch.json",{{"producer_pid",GetCurrentProcessId()},{"producer_creation_time",ownerCreated},{"consumer_pid",child.pid},{"consumer_creation_time",child.created},{"mode",mode},
        {"domain",domain},{"epoch",epoch},{"map_bytes",sizeof(Shared)},{"contract_bytes",sizeof(Contract)},{"header_bytes",offsetof(Shared,slots)},{"slot_bytes",sizeof(mat_ipc::Slot)},{"message_bytes",sizeof(Message)},{"capacity",capacity},
        {"protocol_version",version},{"source_sha256",hashFile(fs::absolute(argv[2]))},{"source_records",source.header.records},{"qpc_frequency",hz()},
        {"explicit_inherited_handle_count",5},{"named_global_objects",false},{"security_settings_changed",false},{"native_encoder_called",false}});
    auto signalFault=[&](Error why){fail(s,why);SetEvent(cancel.get());SetEvent(data.get());};
    auto pollChild=[&]{if(WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0&&get(s.common.consumerStatus)!=static_cast<LONG>(Status::completed)&&error(s)==Error::none)signalFault(Error::consumerExited);};
    auto started=qpc();
    while(WaitForSingleObject(ready.get(),20)!=WAIT_OBJECT_0){put(s.writer.heartbeat,qpc());pollChild();if(error(s)!=Error::none)break;if(qpc()-started>3*hz()){signalFault(Error::timeout);break;}}
    if(mode=="owner-exit"&&error(s)==Error::none){save(out/"abrupt-exit.json",{{"intentional_own_helper_exit",true},{"code",23},{"native_encoder_called",false}});ExitProcess(23);}
    unsigned accepted=0,rejected=0,high=0;std::ofstream timing(out/"producer-clock.csv");timing<<"index,target_qpc,enqueued_qpc,send_finish_qpc\n";
    try{
        if(error(s)==Error::none){
            check(static_cast<DWORD>(get(s.common.consumerPid))==child.pid,"consumer PID binding");
            put(s.common.producerStatus,static_cast<LONG>(Status::streaming));
            if(mode=="owner-stale"||mode=="consumer-stale"){
                auto until=qpc()+2*hz();while(qpc()<until&&error(s)==Error::none){if(mode!="owner-stale")put(s.writer.heartbeat,qpc());pollChild();if(mode=="consumer-stale"&&qpc()-get(s.reader.heartbeat)>hz())signalFault(Error::consumerStale);WaitForSingleObject(child.process.get(),20);}
            }else if(mode!="consumer-exit"){
                Timer timer;auto packet=std::make_unique<Packet>();auto origin=qpc()+hz()/200;
                for(unsigned n=0;n<source.header.records&&error(s)==Error::none;++n){
                    bool burst=mode=="overflow";auto target=origin+static_cast<int64_t>(n)*hz()/100;if(!burst)timer.until(target);
                    pollChild();if(error(s)!=Error::none)break;source.copy(*packet,n,domain);
                    if(mode=="cancel"&&n==10){signalFault(Error::cancelled);break;}
                    bool malformed=mode=="contract-change"||mode=="frame-epoch"||mode=="frame-length"||mode=="checksum"||mode=="sequence";
                    auto before=qpc();bool ok=malformed?corruptedFirstPublish(s,*packet,before,mode):send(s,*packet,before);auto after=qpc();timing<<n<<','<<target<<','<<before<<','<<after<<'\n';
                    if(!ok){++rejected;break;}++accepted;int64_t w,r;if(counts(s,w,r))high=std::max<unsigned>(high,static_cast<unsigned>(w-r));
                    SetEvent(data.get());
                    if(mode=="missing-eof"&&n==0){put(s.common.producerDone,1);SetEvent(data.get());break;}
                    if(malformed)break;
                }
            }
        }
    }catch(const std::exception& e){save(out/"producer-error.json",{{"error",e.what()}});signalFault(Error::badFrame);}
    if(error(s)!=Error::none){SetEvent(cancel.get());SetEvent(data.get());}
    // Parent continues heartbeats while its source is closed and codec drains.
    auto waitStart=qpc();
    while(WaitForSingleObject(child.process.get(),20)!=WAIT_OBJECT_0){
        if(mode!="owner-stale")put(s.writer.heartbeat,qpc());
        if(error(s)==Error::none&&qpc()-get(s.reader.heartbeat)>hz())signalFault(Error::consumerStale);
        if(qpc()-waitStart>5*hz()){signalFault(Error::timeout);break;}
    }
    bool childExited=WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0;DWORD code=STILL_ACTIVE;if(childExited)GetExitCodeProcess(child.process.get(),&code);
    pollChild();timing.close();bool complete=childExited&&code==0&&error(s)==Error::none&&get(s.common.consumerStatus)==static_cast<LONG>(Status::completed)&&get(s.common.producerDone);
    put(s.common.producerStatus,static_cast<LONG>(complete?Status::completed:Status::failed));
    save(out/"summary.json",{{"completed",complete},{"fault",static_cast<LONG>(error(s))},{"mode",mode},{"domain",domain},{"epoch",epoch},{"producer_pid",GetCurrentProcessId()},{"consumer_pid",child.pid},
        {"consumer_exited",childExited},{"consumer_exit",code},{"accepted",accepted},{"rejected",rejected},{"ring_high_water",high},{"write",get(s.writer.write)},{"read",get(s.reader.read)},
        {"producer_backpressure",false},{"producer_capacity_retries",0},{"native_encoder_called",false},{"mat_encoded",false},{"forced_termination",false}});
    return complete?0:2;
}
int main(int argc,char** argv){try{check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg ABI mismatch");if(argc==2&&std::string(argv[1])=="--selftest"){check(!runSelftests()&&!runNative4Tests()&&!runMatStreamTests()&&!runMatStartupTests(),"old regressions");return runIpcTests();}check(argc>1,"role required");if(std::string(argv[1])=="--receive")return receiver(argc,argv);if(std::string(argv[1])=="--produce")return producer(argc,argv);throw std::runtime_error("unknown role");}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 3;}}
