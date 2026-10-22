// Isolated integration driver. Frozen IPC/DLL sources remain unchanged.
#include "cpp_mat_ipc_support.hpp"
#include <bcrypt.h>
using namespace mat_ipc;
int runIpcTests();
static Child spawnHost(const fs::path& path,const std::vector<std::wstring>& args,std::vector<HANDLE> handles){
    auto exe=fs::absolute(path).wstring();std::wstring cmd=quote(exe);for(auto& a:args)cmd+=L" "+quote(a);
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<BYTE> storage(size);auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(list,1,0,&size)!=0,"attribute init");
    struct Cleanup {LPPROC_THREAD_ATTRIBUTE_LIST p;~Cleanup(){DeleteProcThreadAttributeList(p);}} cleanup{list};
    check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),handles.size()*sizeof(HANDLE),nullptr,nullptr)!=0,"explicit handle list");
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=list;PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=0,"spawn own DLL host");
    CloseHandle(pi.hThread);Child child{Handle(pi.hProcess),pi.dwProcessId,0};child.created=creationTime(child.process.get());return child;
}
static bool malformed(Shared& s,const Packet& p,int64_t when,const std::string& mode){
    check(get(s.writer.write)==0&&get(s.reader.read)==0,"fixture first message");
    auto& slot=s.slots[0];Message m{frameMagic,uint32_t(sizeof(Message)+p.header.calls*sizeof(Call)),0,s.contract.domain,s.contract.epoch,p.header.index,480,p.header.calls,1,p.header.firstQpc,p.header.lastQpc,when,0,0};
    if(m.calls)memcpy(slot.calls,p.calls.data(),m.calls*sizeof(Call));
    if(mode=="contract-change")++s.contract.version;if(mode=="frame-epoch")++m.epoch;if(mode=="frame-length")m.bytes=1;if(mode=="sequence")++m.sequence;
    m.checksum=checksum(m,slot.calls);if(mode=="checksum")m.checksum^=1;slot.header=m;put(s.writer.heartbeat,when);put(s.writer.write,1);return true;
}
int main(int argc,char** argv){try{
    check(avutil_version()==LIBAVUTIL_VERSION_INT,"FFmpeg ABI");
    if(argc==2&&std::string(argv[1])=="--selftest")return runIpcTests();
    check(argc==9&&std::string(argv[1])=="--produce","Usage: --produce SOURCE DOMAIN EPOCH NEW_OUT MODE OWN_HOST OWN_DLL");
    Source source(fs::absolute(argv[2]));uint64_t domain=std::stoull(argv[3]),epoch=std::stoull(argv[4]);auto out=fs::absolute(argv[5]);std::string mode=argv[6];auto host=fs::absolute(argv[7]),dll=fs::absolute(argv[8]);
    const std::set<std::string> allowed{"normal","burst4","header-version","contract-change","identity","frame-epoch","frame-length","checksum","sequence","overflow","cancel","owner-exit","consumer-exit","owner-stale","consumer-stale","missing-eof"};
    check(allowed.contains(mode)&&domain&&epoch&&source.header.records<=351&&fs::is_regular_file(host)&&fs::is_regular_file(dll),"bounded own integration config");
    check(!fs::exists(out)&&fs::create_directories(out),"new output");
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle map(CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(Shared),nullptr));check(map.get()!=nullptr,"mapping");View view(map.get());auto& s=view.get();
    uint64_t nonce[2];check(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"nonce");initialize(s,domain,epoch,nonce[0],nonce[1]);
    Handle data(CreateEventW(&sa,FALSE,FALSE,nullptr)),cancel(CreateEventW(&sa,TRUE,FALSE,nullptr)),ready(CreateEventW(&sa,TRUE,FALSE,nullptr));check(data.get()&&cancel.get()&&ready.get(),"events");HANDLE duplicate{};
    check(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&duplicate,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)!=0,"owner handle");Handle owner(duplicate);auto created=creationTime(GetCurrentProcess());
    if(mode=="header-version")++s.contract.version;
    auto h=[](HANDLE v){return number(reinterpret_cast<uintptr_t>(v));};
    std::vector<std::wstring> args{L"--receive",h(map.get()),h(data.get()),h(cancel.get()),h(ready.get()),h(owner.get()),number(domain),number(epoch),number(nonce[0]+(mode=="identity")),number(nonce[1]),number(GetCurrentProcessId()),number(created),(out/"receiver").wstring(),widen(mode),number(source.header.records),number(source.header.qpcHz),dll.wstring()};
    auto child=spawnHost(host,args,{map.get(),data.get(),cancel.get(),ready.get(),owner.get()});
    save(out/"launch.json",{{"producer_pid",GetCurrentProcessId()},{"producer_creation_time",created},{"consumer_pid",child.pid},{"consumer_creation_time",child.created},{"mode",mode},{"domain",domain},{"epoch",epoch},
        {"map_bytes",sizeof(Shared)},{"capacity",capacity},{"protocol_version",version},{"qpc_frequency",hz()},{"source_records",source.header.records},{"source_sha256",hashFile(fs::absolute(argv[2]))},
        {"host_path",host.string()},{"host_sha256",hashFile(host)},{"receiver_dll_path",dll.string()},{"receiver_dll_sha256",hashFile(dll)},
        {"explicit_inherited_handle_count",5},{"named_global_objects",false},{"native_encoder_called",false},{"security_settings_changed",false}});
    auto fault=[&](Error e){fail(s,e);SetEvent(cancel.get());SetEvent(data.get());};
    auto poll=[&]{if(WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0&&get(s.common.consumerStatus)!=LONG(Status::completed)&&error(s)==Error::none)fault(Error::consumerExited);};
    auto began=qpc();while(WaitForSingleObject(ready.get(),20)!=WAIT_OBJECT_0){put(s.writer.heartbeat,qpc());poll();if(error(s)!=Error::none)break;if(qpc()-began>3*hz()){fault(Error::timeout);break;}}
    if(mode=="owner-exit"&&error(s)==Error::none){save(out/"abrupt-exit.json",{{"intentional_own_helper_exit",true},{"code",23},{"native_encoder_called",false}});ExitProcess(23);}
    unsigned accepted=0,rejected=0,high=0;std::ofstream timing(out/"producer-clock.csv");timing<<"index,target_qpc,enqueued_qpc,send_finish_qpc\n";
    try{
        if(error(s)==Error::none){check(DWORD(get(s.common.consumerPid))==child.pid,"child PID binding");put(s.common.producerStatus,LONG(Status::streaming));
            if(mode=="owner-stale"||mode=="consumer-stale"){
                auto until=qpc()+2*hz();while(qpc()<until&&error(s)==Error::none){if(mode!="owner-stale")put(s.writer.heartbeat,qpc());poll();if(mode=="consumer-stale"&&qpc()-get(s.reader.heartbeat)>hz())fault(Error::consumerStale);WaitForSingleObject(child.process.get(),20);}
            }else if(mode!="consumer-exit"){
                Timer timer;auto packet=std::make_unique<Packet>();auto origin=qpc()+hz()/200;
                for(unsigned n=0;n<source.header.records&&error(s)==Error::none;++n){
                    auto scheduled=mode=="burst4"?(n/4)*4:n;auto target=origin+int64_t(scheduled)*hz()/100;if(mode!="overflow")timer.until(target);poll();if(error(s)!=Error::none)break;
                    source.copy(*packet,n,domain);if(mode=="cancel"&&n==10){fault(Error::cancelled);break;}
                    bool bad=mode=="contract-change"||mode=="frame-epoch"||mode=="frame-length"||mode=="checksum"||mode=="sequence";
                    auto before=qpc();bool okay=bad?malformed(s,*packet,before,mode):send(s,*packet,before);auto after=qpc();timing<<n<<','<<target<<','<<before<<','<<after<<'\n';
                    if(!okay){++rejected;break;}++accepted;int64_t w,r;if(counts(s,w,r))high=std::max<unsigned>(high,unsigned(w-r));SetEvent(data.get());
                    if(mode=="missing-eof"&&n==0){put(s.common.producerDone,1);SetEvent(data.get());break;}if(bad)break;
                }
            }
        }
    }catch(const std::exception& e){save(out/"producer-error.json",{{"error",e.what()}});fault(Error::badFrame);}
    if(error(s)!=Error::none){SetEvent(cancel.get());SetEvent(data.get());}
    auto waiting=qpc();while(WaitForSingleObject(child.process.get(),20)!=WAIT_OBJECT_0){if(mode!="owner-stale")put(s.writer.heartbeat,qpc());if(error(s)==Error::none&&qpc()-get(s.reader.heartbeat)>hz())fault(Error::consumerStale);if(qpc()-waiting>5*hz()){fault(Error::timeout);break;}}
    bool exited=WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0;DWORD code=STILL_ACTIVE;if(exited)GetExitCodeProcess(child.process.get(),&code);poll();timing.close();
    bool complete=exited&&code==0&&error(s)==Error::none&&get(s.common.consumerStatus)==LONG(Status::completed)&&get(s.common.producerDone);put(s.common.producerStatus,LONG(complete?Status::completed:Status::failed));
    save(out/"summary.json",{{"completed",complete},{"fault",LONG(error(s))},{"domain",domain},{"epoch",epoch},{"mode",mode},{"consumer_pid",child.pid},{"consumer_exited",exited},{"consumer_exit",code},
        {"accepted",accepted},{"rejected",rejected},{"write",get(s.writer.write)},{"read",get(s.reader.read)},{"ring_high_water",high},{"producer_capacity_retries",0},{"producer_backpressure",false},{"forced_termination",false},{"native_encoder_called",false},{"mat_encoded",false}});
    return complete?0:2;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 3;}}
