// Prepared one-shot launcher. Default tests touch ONLY freshly spawned children.
// Native mode is explicit, never self-elevates, never restarts an audio engine.
#include "mat_ipc_receiver_api.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace receiver;using Json=nlohmann::json;namespace fs=std::filesystem;
static void check(bool okay,const char* message){if(!okay)throw std::runtime_error(std::string(message)+" win32="+std::to_string(GetLastError()));}
struct Handle {HANDLE h{};Handle()=default;explicit Handle(HANDLE p):h(p){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
struct View {void* p{};View(HANDLE h,size_t bytes){p=MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,bytes);check(p!=nullptr,"Map own diagnostic/shared view");}~View(){if(p)UnmapViewOfFile(p);}};
static void save(const fs::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2)<<'\n';check(bool(f),"Save diagnostic");}
static uint64_t created(HANDLE h){FILETIME a,b,c,d;check(GetProcessTimes(h,&a,&b,&c,&d)!=0,"Process creation identity");return uint64_t(a.dwHighDateTime)<<32|a.dwLowDateTime;}
static std::wstring image(HANDLE h){wchar_t path[32768];DWORD size=32768;check(QueryFullProcessImageNameW(h,0,path,&size)!=0,"Process image");return path;}
static bool equalPath(const fs::path& a,const fs::path& b){return _wcsicmp(fs::absolute(a).lexically_normal().c_str(),fs::absolute(b).lexically_normal().c_str())==0;}
static fs::path system(const wchar_t* file){wchar_t path[MAX_PATH];check(GetSystemDirectoryW(path,MAX_PATH)>0,"System directory");return fs::path(path)/file;}
static std::string hash(const fs::path& p){
    std::ifstream f(p,std::ios::binary);check(bool(f),"Open hashed input");BCRYPT_ALG_HANDLE a{};BCRYPT_HASH_HANDLE h{};
    check(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider");
    struct Cleanup {BCRYPT_ALG_HANDLE a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);}} cleanup{a,h};
    DWORD size{},n{};check(BCryptGetProperty(a,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&n,0)>=0,"Hash size");std::vector<BYTE> object(size);BYTE digest[32];
    check(BCryptCreateHash(a,&h,object.data(),size,nullptr,0,0)>=0,"Hash object");char bytes[65536];while(f){f.read(bytes,sizeof(bytes));auto count=f.gcount();if(count)check(BCryptHashData(h,reinterpret_cast<PUCHAR>(bytes),static_cast<ULONG>(count),0)>=0,"Hash bytes");}check(f.eof(),"Read hash source");check(BCryptFinishHash(h,digest,32,0)>=0,"Finish hash");
    constexpr char hex[]="0123456789abcdef";std::string out;for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
static constexpr const char* helperHash="a88652ffe2a24c41644548263b4b9accce575a91820afe708aec4fbbc3556991";
struct Module {uintptr_t base{};fs::path path;};
static Module module(DWORD pid,const wchar_t* name){Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));check(snap.h!=INVALID_HANDLE_VALUE,"Module snapshot");MODULEENTRY32W m{};m.dwSize=sizeof(m);if(Module32FirstW(snap.h,&m))do{if(!_wcsicmp(m.szModule,name))return {reinterpret_cast<uintptr_t>(m.modBaseAddr),m.szExePath};}while(Module32NextW(snap.h,&m));return {};}
static std::vector<DWORD> engines(){Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));check(snap.h!=INVALID_HANDLE_VALUE,"Process inventory");PROCESSENTRY32W p{};p.dwSize=sizeof(p);std::vector<DWORD> ids;if(Process32FirstW(snap.h,&p))do{if(!_wcsicmp(p.szExeFile,L"audiodg.exe"))ids.push_back(p.th32ProcessID);}while(Process32NextW(snap.h,&p));return ids;}
static bool elevated(){Handle token;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h)!=0,"Own token");TOKEN_ELEVATION e{};DWORD n{};check(GetTokenInformation(token.h,TokenElevation,&e,sizeof(e),&n)!=0,"Own elevation");return e.TokenIsElevated!=0;}
static void components(){
    check(hash(system(L"audioeng.dll"))=="3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3","Version-locked audioeng hash");
    fs::path folder=L"C:/Program Files/WindowsApps/DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220/DLLs/x64";
    check(hash(folder/L"DolbyMatEnc.dll")=="963697fb61ee302699b294b1b6cb59ab42e5eae3a555f6674ed54fc3a3cfacea","Version-locked encoder hash");
    check(hash(folder/L"DolbyMatEngine.dll")=="342cbc58a19afa2b5b58cef408ef91dcc06c68829520286acabb7178eda7f569","Version-locked engine hash");
}
struct Packet {SourceFrame frame{};std::vector<Call> calls;};
static std::vector<Packet> source(const fs::path& path){
    std::ifstream f(path,std::ios::binary);check(bool(f),"Open bridge source");SourceHeader h{};f.read(reinterpret_cast<char*>(&h),sizeof(h));
    check(bool(f)&&!memcmp(h.magic,"MATBRG1",8)&&h.version==1&&h.rate==48000&&h.frames==480&&h.records>=2&&h.records<=351&&h.hz>0,"Bounded MATBRG1 source header");
    std::vector<Packet> out;std::vector<uint32_t> active;
    for(uint32_t n=0;n<h.records;++n){Packet p;f.read(reinterpret_cast<char*>(&p.frame),sizeof(p.frame));auto& v=p.frame;bool eof=n+1==h.records;
        check(bool(f)&&v.index==n&&v.frames==480&&v.calls<=64&&v.flags==(eof?1u:0u),"Source frame/EOF bounds");p.calls.resize(v.calls);f.read(reinterpret_cast<char*>(p.calls.data()),v.calls*sizeof(Call));check(bool(f),"Complete source payload");
        std::vector<uint32_t> seen;for(auto& c:p.calls){check(c.id&&c.type==1&&c.frames==480&&(c.flags==6||c.flags==7||c.flags==8)&&(!eof||c.flags==8),"Object dimensions/lifecycle flags");for(auto id:seen)check(id!=c.id,"Duplicate object");seen.push_back(c.id);
            for(auto x:c.xyz)check(std::isfinite(x)&&fabsf(x)<=10000,"Finite XYZ");for(auto x:c.pcm)check(std::isfinite(x)&&fabsf(x)<=1&&(c.flags!=8||x==0),"Bounded mono PCM; unity already applied");
            auto at=std::find(active.begin(),active.end(),c.id);if(c.flags==7){check(at==active.end()&&active.size()<20,"Unique create with live object bound");active.push_back(c.id);}else{check(at!=active.end(),"Known update/end");if(c.flags==8)active.erase(at);}}
        if(eof)check(active.empty(),"EOF ends all objects");else for(auto id:active)check(std::find(seen.begin(),seen.end(),id)!=seen.end(),"Source does not omit live object");out.push_back(std::move(p));}
    check(f.peek()==EOF,"No trailing source bytes");return out;
}
// Validate ALL output before creating either output file. Not a MAT decoder.
static void validateOutput(const NativeOutput& s){
    auto& h=s.header;check(h.magic==outputMagic&&h.version==apiVersion&&h.bytes==sizeof(NativeOutput)&&h.kind==1,"Native output type/size");
    check(h.records<=roundLimit&&h.outputBytes<=byteLimit&&h.records==h.processes,"Native record/process bounds");uint64_t offset=0;int64_t last=0;
    for(uint32_t n=0;n<h.records;++n){auto& r=s.records[n];check(r.magic==0x42595445&&r.sequence==n&&r.inputFrames==480&&r.capacity==request&&r.produced==r.bytes&&r.bytes<=request&&r.result==S_OK&&!r.flags&&!r.reserved&&r.encoder&&r.threadId==h.workerTid&&r.qpc>0&&r.qpc>=last,"Native record metadata");check(offset+r.bytes<=h.outputBytes,"Native record payload bound");offset+=r.bytes;last=r.qpc;}
    check(offset==h.outputBytes,"No missing/extra native payload");
}
static void exportOutput(const fs::path& out,const NativeOutput& s){validateOutput(s);std::ofstream records(out/L"encoder-output.bin",std::ios::binary),payload(out/L"encoder-payload.raw",std::ios::binary);check(bool(records)&&bool(payload),"Create new native output files");size_t offset=0;for(uint32_t n=0;n<s.header.records;++n){auto& r=s.records[n];records.write(reinterpret_cast<const char*>(&r),sizeof(r));records.write(reinterpret_cast<const char*>(s.bytes+offset),r.bytes);payload.write(reinterpret_cast<const char*>(s.bytes+offset),r.bytes);offset+=r.bytes;}records.close();payload.close();check(bool(records)&&bool(payload),"Flush native output");}
// Own all remotely duplicated originals. The DLL duplicates these again into
// its private Session. Originals/argument memory cannot be freed until entry
// has returned. A blocked entry retains them, never closes a possibly-live use.
struct Remote {
    HANDLE process{};Handle loader,entry;void* path{},*args{};std::vector<HANDLE> handles;Json audit=Json::object();bool done=false;
    explicit Remote(HANDLE p):process(p){}
    void* copy(const void* data,size_t size){void* p=VirtualAllocEx(process,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);check(p!=nullptr,"Remote argument allocation");SIZE_T written{};if(!WriteProcessMemory(process,p,data,size,&written)||written!=size){VirtualFreeEx(process,p,0,MEM_RELEASE);check(false,"Complete remote argument copy");}return p;}
    HANDLE duplicate(HANDLE local){HANDLE remote{};check(DuplicateHandle(GetCurrentProcess(),local,process,&remote,0,FALSE,DUPLICATE_SAME_ACCESS)!=0,"Remote handle handoff");handles.push_back(remote);return remote;}
    uintptr_t load(DWORD pid,const fs::path& dll){auto found=module(pid,dll.filename().c_str());if(found.base){check(equalPath(found.path,dll),"Reject same-name foreign helper");return found.base;}
        auto fn=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");check(fn!=nullptr,"Local loader export");HMODULE owner{};check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(fn),&owner)!=0,"Loader owner");wchar_t ownerPath[MAX_PATH];check(GetModuleFileNameW(owner,ownerPath,MAX_PATH)>0,"Loader owner path");auto target=module(pid,fs::path(ownerPath).filename().c_str());check(target.base&&equalPath(target.path,ownerPath),"Remote loader owner path");
        auto text=dll.wstring();path=copy(text.c_str(),(text.size()+1)*sizeof(wchar_t));loader.h=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(target.base+reinterpret_cast<uintptr_t>(fn)-reinterpret_cast<uintptr_t>(owner)),path,0,nullptr);check(loader.h!=nullptr,"Start helper loader");check(WaitForSingleObject(loader.h,5000)==WAIT_OBJECT_0,"Loader timeout: retain argument, no retry");check(VirtualFreeEx(process,path,0,MEM_RELEASE)!=0,"Free returned loader argument");path=nullptr;
        found=module(pid,dll.filename().c_str());check(found.base&&equalPath(found.path,dll),"Loaded helper identity");return found.base;}
    void start(uintptr_t address,const Args& a){args=copy(&a,sizeof(a));entry.h=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(address),args,0,nullptr);check(entry.h!=nullptr,"Start dedicated receiver control entry");}
    void cleanup(){
        bool loaderDone=!loader.h||WaitForSingleObject(loader.h,0)==WAIT_OBJECT_0;
        bool entryDone=!entry.h||WaitForSingleObject(entry.h,0)==WAIT_OBJECT_0;done=loaderDone&&entryDone;
        size_t closed=0;DWORD closeError=0;
        if(done){if(path){VirtualFreeEx(process,path,0,MEM_RELEASE);path=nullptr;}if(args){VirtualFreeEx(process,args,0,MEM_RELEASE);args=nullptr;}
            for(auto h:handles){HANDLE own{};if(DuplicateHandle(process,h,GetCurrentProcess(),&own,0,FALSE,DUPLICATE_SAME_ACCESS|DUPLICATE_CLOSE_SOURCE)){CloseHandle(own);++closed;}else if(WaitForSingleObject(process,0)==WAIT_OBJECT_0){++closed;}else closeError=GetLastError();}}
        audit={{"remote_original_handles",handles.size()},{"remote_original_handles_released",closed},{"close_error",closeError},{"entry_finished",entryDone},{"loader_finished",loaderDone},{"argument_retained",args!=nullptr||path!=nullptr},{"retained_handle_count",done?handles.size()-closed:handles.size()},{"forced_termination",false},{"remote_unload_attempted",false}};handles.clear();
    }
    ~Remote(){if(!audit.contains("entry_finished"))cleanup();}
};
static Json session(HANDLE process,DWORD pid,const fs::path& dll,const std::vector<Packet>& packets,uint64_t domain,uint64_t epoch,const fs::path& out,int failAfter=-1,bool ownGuard=false){
    check(fs::create_directory(out),"New session output");Handle input(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),nullptr)),output(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(NativeOutput),nullptr));check(input.h&&output.h,"Session mappings");View iv(input.h,sizeof(Shared)),ov(output.h,sizeof(NativeOutput));auto& s=*static_cast<Shared*>(iv.p);auto& o=*static_cast<NativeOutput*>(ov.p);memset(iv.p,0,sizeof(Shared));memset(ov.p,0,sizeof(NativeOutput));
    uint64_t nonce[2];check(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Session nonce");s.contract={magic,version,320,sizeof(Shared),sizeof(Slot),capacity,124968,sizeof(Call),domain,epoch,{nonce[0],nonce[1]},GetCurrentProcessId(),48000,480,0,hz()};put(s.writer.heartbeat,qpc());o.header.magic=outputMagic;o.header.version=apiVersion;o.header.bytes=sizeof(o);o.header.kind=1;o.header.createHr=o.header.mapperHr=o.header.initializeHr=o.header.submitHr=o.header.processHr=E_PENDING;
    Handle data(CreateEventW(nullptr,FALSE,FALSE,nullptr)),cancel(CreateEventW(nullptr,TRUE,FALSE,nullptr)),owner;check(data.h&&cancel.h,"Session events");check(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&owner.h,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)!=0,"Least-right owner handle");Remote remote(process);Json result;std::string error;DWORD entryCode=STILL_ACTIVE;uint32_t accepted=0;bool timed=false;
    try{
        Args a{sizeof(Args),apiVersion,nullptr,nullptr,nullptr,nullptr,nullptr,domain,epoch,{nonce[0],nonce[1]},created(GetCurrentProcess()),GetCurrentProcessId(),static_cast<uint32_t>(packets.size()-1),10,0};HANDLE originals[]{input.h,data.h,cancel.h,owner.h,output.h};HANDLE* destinations[]{&a.input,&a.data,&a.cancel,&a.owner,&a.output};
        for(int n=0;n<5;++n){if(n==failAfter)throw std::runtime_error("intentional pre-entry handle-handoff failure");*destinations[n]=remote.duplicate(originals[n]);}
        auto base=remote.load(pid,dll);HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);check(local!=nullptr,"Map own export without initialization");auto start=GetProcAddress(local,"StartMatIpcReceiver");auto rva=reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);check(start!=nullptr,"Fixed native entry export");remote.start(base+rva,a);
        auto began=GetTickCount64();while(get(s.common.consumerPid)!=LONG(pid)&&WaitForSingleObject(remote.entry.h,2)!=WAIT_OBJECT_0&&GetTickCount64()-began<3000)put(s.writer.heartbeat,qpc());
        if(!ownGuard){check(get(s.common.consumerPid)==LONG(pid),"Native receiver PID claim before feeding");Handle timer(CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS));check(timer.h!=nullptr,"Producer timer");auto origin=qpc()+hz()/200;
            for(uint32_t n=0;n<packets.size();++n){auto target=origin+n*hz()/100;while(qpc()<target){put(s.writer.heartbeat,qpc());check(!get(s.common.fault)&&WaitForSingleObject(process,0)==WAIT_TIMEOUT&&WaitForSingleObject(remote.entry.h,0)==WAIT_TIMEOUT,"Receiver stopped before source publication");LARGE_INTEGER due{};due.QuadPart=-10000;check(SetWaitableTimer(timer.h,&due,0,nullptr,nullptr,FALSE)&&WaitForSingleObject(timer.h,20)==WAIT_OBJECT_0,"Bounded producer cadence wait");}
                auto w=get(s.writer.write),r=get(s.reader.read);check(w>=r&&w-r<32&&!get(s.common.fault),"No producer backpressure/retry; fail on full ring");auto& slot=s.slots[w%32];auto& p=packets[n];Message m{frameMagic,static_cast<uint32_t>(sizeof(Message)+p.calls.size()*sizeof(Call)),static_cast<uint64_t>(w),domain,epoch,n,480,static_cast<uint32_t>(p.calls.size()),n+1==packets.size()?2u:1u,p.frame.firstQpc,p.frame.lastQpc,qpc(),0,0};if(!p.calls.empty())memcpy(slot.calls,p.calls.data(),p.calls.size()*sizeof(Call));m.checksum=checksum(m,slot.calls);slot.header=m;put(s.writer.heartbeat,qpc());put(s.writer.write,w+1);++accepted;if(m.kind==2)put(s.common.producerDone,1);SetEvent(data.h);}}
        auto end=GetTickCount64()+12000;while(WaitForSingleObject(remote.entry.h,20)!=WAIT_OBJECT_0&&GetTickCount64()<end){put(s.writer.heartbeat,qpc());if(get(s.common.fault)){SetEvent(cancel.h);SetEvent(data.h);}}
        if(WaitForSingleObject(remote.entry.h,0)!=WAIT_OBJECT_0){timed=true;fail(s,timeout);SetEvent(cancel.h);SetEvent(data.h);check(WaitForSingleObject(remote.entry.h,2000)==WAIT_OBJECT_0,"Native entry blocked: retain live state, never terminate");}check(GetExitCodeThread(remote.entry.h,&entryCode)!=0,"Entry return code");
    }catch(const std::exception& e){error=e.what();fail(s,cancelled);SetEvent(cancel.h);SetEvent(data.h);if(remote.entry.h&&WaitForSingleObject(remote.entry.h,2000)==WAIT_OBJECT_0)GetExitCodeThread(remote.entry.h,&entryCode);}
    remote.cleanup();bool stable=remote.done&&!o.header.workerTimedOut;auto& h=o.header;
    result={{"completed",false},{"error",error},{"target_pid",pid},{"target_creation_time",created(process)},{"own_guard_test",ownGuard},{"accepted",accepted},{"entry_return",entryCode},{"shared_fault",get(s.common.fault)},{"entry_timed_out",timed},{"diagnostics_stable",stable},{"cleanup",remote.audit},{"endpoint_changes",false},{"driver_changes",false},{"hooks_installed",false},{"mat_verified",false},{"automatic_retry",false}};
    if(stable){result["output"]={{"status",h.status},{"error",h.error},{"host_pid",h.hostPid},{"host_guard_passed",bool(h.hostGuardPassed)},{"worker_tid",h.workerTid},{"worker_finished",bool(h.workerFinished)},{"worker_timed_out",bool(h.workerTimedOut)},{"helper_pinned",bool(h.helperPinned)},{"vendor_loads",h.vendorLoads},{"activations",h.activations},{"initializations",h.initializations},{"processes",h.processes},{"records",h.records},{"bytes",h.outputBytes},{"source_rounds",h.sourceRounds},{"warm_rounds",h.warmRounds},{"drain_rounds",h.drainRounds},{"submissions",h.submissions},{"source_samples",h.sourceSamples},{"messages",h.messages},{"eof_seen",bool(h.eofSeen)},{"create_hr",h.createHr},{"mapper_hr",h.mapperHr},{"initialize_hr",h.initializeHr},{"submit_hr",h.submitHr},{"process_hr",h.processHr}};
        if(!ownGuard&&remote.entry.h){try{validateOutput(o);exportOutput(out,o);uint64_t expectedSamples=0,expectedSubmits=0;size_t firstCount=0;for(size_t n=0;n<packets.size();++n){if(!firstCount&&n+1<packets.size())firstCount=packets[n].calls.size();for(auto& c:packets[n].calls){++expectedSubmits;if(n+1<packets.size()&&c.flags!=8)expectedSamples+=480;}}expectedSubmits+=firstCount*20;
            result["output_exported"]=true;result["completed"]=error.empty()&&entryCode==0&&h.status==2&&!h.error&&!get(s.common.fault)&&h.workerFinished&&h.eofSeen&&h.hostGuardPassed&&h.hostPid==pid&&h.helperPinned&&h.vendorLoads==1&&h.activations==1&&h.initializations==1&&h.createHr==S_OK&&h.mapperHr==S_OK&&h.initializeHr==S_OK&&h.submitHr==S_OK&&h.processHr==S_OK&&h.sourceRounds+1==packets.size()&&h.warmRounds==20&&h.drainRounds==30&&h.processes==h.sourceRounds+h.warmRounds+h.drainRounds&&h.messages==packets.size()&&get(s.writer.write)==get(s.reader.read)&&h.records==h.processes&&h.outputBytes>0&&h.sourceSamples==expectedSamples&&h.submissions==expectedSubmits&&!h.endpointCalls&&!h.hooksInstalled&&remote.audit["close_error"]==0&&remote.audit["retained_handle_count"]==0;
        }catch(const std::exception& e){result["output_validation_error"]=e.what();}}}
    else result["warning"]="Entry/worker unfinished: do not read or export concurrently changing diagnostics. Retained remote resources until natural host exit; no forced kill/unload.";
    save(out/L"summary.json",result);return result;
}
static std::wstring quote(const std::wstring& s){check(s.find(L'"')==std::wstring::npos,"No quote in generated path");return L"\""+s+L"\"";}
static Json selftest(const fs::path& dll,const fs::path& src,const fs::path& out){
    check(hash(dll)==helperHash,"Frozen helper hash");auto packets=source(src);std::vector<std::string> tests;auto test=[&](const char* name,auto fn){fn();tests.push_back(name);};
    auto o=std::make_unique<NativeOutput>();memset(o.get(),0,sizeof(*o));o->header.magic=outputMagic;o->header.version=1;o->header.bytes=sizeof(*o);o->header.kind=1;o->header.workerTid=123;o->header.records=o->header.processes=2;o->header.outputBytes=5;o->records[0]={0x42595445,0,0,10,1234,480,request,0,123,S_OK,0,0};o->records[1]={0x42595445,5,1,20,1234,480,request,5,123,S_OK,0,0};memcpy(o->bytes,"abcde",5);
    test("zero_byte_Process_and_nonzero_record_export",[&]{exportOutput(out,*o);check(fs::file_size(out/L"encoder-output.bin")==133&&fs::file_size(out/L"encoder-payload.raw")==5,"Byte-exact output dimensions");});
    auto rejects=[&]{bool rejected=false;try{validateOutput(*o);}catch(...){rejected=true;}check(rejected,"Bad output must be rejected");};
    test("reject_record_count_overflow",[&]{o->header.records=401;rejects();o->header.records=2;});
    test("reject_payload_sum_mismatch",[&]{++o->header.outputBytes;rejects();--o->header.outputBytes;});
    test("reject_sequence_gap",[&]{++o->records[1].sequence;rejects();--o->records[1].sequence;});
    test("reject_S_FALSE_Process",[&]{o->records[1].result=S_FALSE;rejects();o->records[1].result=S_OK;});
    test("reject_request_overflow",[&]{o->records[1].bytes=o->records[1].produced=request+1;rejects();o->records[1].bytes=o->records[1].produced=5;});
    test("reject_worker_identity_mismatch",[&]{++o->records[1].threadId;rejects();--o->records[1].threadId;});
    test("reject_time_reversal",[&]{o->records[1].qpc=9;rejects();o->records[1].qpc=20;});
    test("reject_native_output_kind_mismatch",[&]{o->header.kind=2;rejects();o->header.kind=1;});
    for(int failAfter: {0,1,2,3,4,-1}){
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle stop(CreateEventW(&sa,TRUE,FALSE,nullptr)),ready(CreateEventW(&sa,TRUE,FALSE,nullptr));check(stop.h&&ready.h,"Own child control events");wchar_t exe[32768];check(GetModuleFileNameW(nullptr,exe,32768)>0,"Own executable");std::wstring cmd=quote(exe)+L" --own-idle "+std::to_wstring(reinterpret_cast<uintptr_t>(stop.h))+L" "+std::to_wstring(reinterpret_cast<uintptr_t>(ready.h));
        SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<BYTE> storage(size);auto attrs=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());check(InitializeProcThreadAttributeList(attrs,1,0,&size)!=0,"Child attributes");HANDLE inherited[]{stop.h,ready.h};check(UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=0,"Two explicit inherited child control handles");STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=attrs;PROCESS_INFORMATION pi{};BOOL launched=CreateProcessW(exe,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi);DeleteProcThreadAttributeList(attrs);check(launched!=FALSE,"Launch own ordinary test child");Handle child(pi.hProcess),mainThread(pi.hThread);check(WaitForSingleObject(ready.h,5000)==WAIT_OBJECT_0,"Own child initialized before handle baseline");DWORD handlesBefore{},handlesAfter{};check(GetProcessHandleCount(child.h,&handlesBefore)!=0,"Own child initial handle count");auto path=out/(L"handoff-"+std::to_wstring(failAfter+1));Json r;
        DWORD handlesBeforeLoad=handlesBefore;Json bootstrapAudit;
        try{
            // A first Windows DLL load initializes runtime dependencies which
            // own persistent handles. Isolate that from per-entry ownership.
            if(failAfter==-1){Remote bootstrap(child.h);bootstrap.load(pi.dwProcessId,dll);bootstrap.cleanup();bootstrapAudit=bootstrap.audit;check(GetProcessHandleCount(child.h,&handlesBefore)!=0,"Own child post-loader baseline");}
            r=session(child.h,pi.dwProcessId,dll,packets,1,5000+failAfter+1,path,failAfter,true);
        }catch(...){SetEvent(stop.h);WaitForSingleObject(child.h,5000);throw;}
        auto& c=r["cleanup"];check(c["entry_finished"]&&c["loader_finished"]&&!c["argument_retained"].get<bool>()&&c["retained_handle_count"]==0&&c["close_error"]==0,"All own remote originals/arguments released");
        check(GetProcessHandleCount(child.h,&handlesAfter)!=0,"Own child final handle count");SetEvent(stop.h);check(WaitForSingleObject(child.h,5000)==WAIT_OBJECT_0,"Own child exits without kill");DWORD childExit{};check(GetExitCodeProcess(child.h,&childExit)&&childExit==0,"Own child normal exit");save(path/L"child-exit.json",{{"pid",pi.dwProcessId},{"creation_time",created(child.h)},{"exit_code",childExit},{"handles_before_DLL_load",handlesBeforeLoad},{"handles_before_entry",handlesBefore},{"handles_after_entry",handlesAfter},{"bootstrap_cleanup",bootstrapAudit},{"forced_termination",false}});check(handlesBefore==handlesAfter,"Own child entry handle count returned to post-loader baseline");
        if(failAfter>=0){check(c["remote_original_handles"]==failAfter&&r["entry_return"]==STILL_ACTIVE,"Partial handoff failure before remote entry");tests.push_back("partial_handoff_"+std::to_string(failAfter)+"_cleanup");}
        else{auto& h=r["output"];check(r["entry_return"]==ERROR_BAD_ENVIRONMENT&&h["status"]==-1&&h["error"]==ERROR_BAD_ENVIRONMENT&&!h["host_guard_passed"].get<bool>()&&h["vendor_loads"]==0&&h["activations"]==0&&h["initializations"]==0&&h["worker_tid"]==0&&!h["helper_pinned"].get<bool>(),"Ordinary child rejected before vendor/worker");tests.push_back("actual_cross_process_native_entry_guard_and_five_handle_cleanup");}}
    Json proof={{"passed",true},{"selftests_passed",tests.size()},{"tests",tests},{"helper_sha256",helperHash},{"source_sha256",hash(src)},{"ordinary_own_children",6},{"foreign_process_accessed",false},{"audiodg_accessed",false},{"native_encoder_called",false},{"vendor_loaded",false},{"native_entry_guard_called_in_own_child",true},{"output_fixture_is_synthetic",true},{"mat_verified",false},{"forced_termination",false}};save(out/L"proof.json",proof);return proof;
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==4&&!wcscmp(argv[1],L"--own-idle")){Handle stop(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[2])))),ready(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(argv[3]))));check(SetEvent(ready.h)!=0,"Own child ready");return WaitForSingleObject(stop.h,30000)==WAIT_OBJECT_0?0:4;}
    if(argc==4&&!wcscmp(argv[1],L"--validate-source")){auto out=fs::absolute(argv[3]);check(!fs::exists(out)&&fs::create_directories(out),"New source validation output");auto packets=source(argv[2]);uint64_t samples=0,calls=0;for(size_t n=0;n<packets.size();++n)for(auto& c:packets[n].calls){++calls;if(n+1<packets.size()&&c.flags!=8)samples+=480;}Json j={{"passed",true},{"source_sha256",hash(argv[2])},{"source_rounds",packets.size()-1},{"messages",packets.size()},{"source_samples",samples},{"calls",calls},{"native_encoder_called",false},{"process_accessed",false}};save(out/L"source.json",j);std::cout<<j.dump(2)<<'\n';return 0;}
    if(argc==3&&!wcscmp(argv[1],L"--inspect")){fs::path out=fs::absolute(argv[2]);check(!fs::exists(out)&&fs::create_directories(out),"New inspection directory");auto ids=engines();Json j={{"audiodg_pids",ids},{"exactly_one_existing_engine",ids.size()==1},{"current_process_elevated",elevated()},{"foreign_process_opened",false},{"module_loaded",false},{"native_encoder_called",false},{"security_changed",false},{"engine_started",false}};try{components();j["component_hashes_match"]=true;}catch(const std::exception& e){j["component_hashes_match"]=false;j["component_error"]=e.what();}save(out/L"inspection.json",j);std::cout<<j.dump(2)<<'\n';return 0;}
    if(argc==5&&!wcscmp(argv[1],L"--selftest")){auto out=fs::absolute(argv[4]);check(!fs::exists(out)&&fs::create_directories(out),"New own-process selftest output");std::cout<<selftest(fs::absolute(argv[2]),fs::absolute(argv[3]),out).dump(2)<<'\n';return 0;}
    check(argc==10&&!wcscmp(argv[1],L"--authorized-native-once"),"Usage: --inspect NEW_OUT | --selftest FROZEN_DLL SOURCE NEW_OUT | --authorized-native-once DEPLOYED_DLL SOURCE DOMAIN EPOCH PID CREATION NEW_OUT");
    // Human approval is required OUTSIDE this program. The explicit switch is
    // not evidence of approval. No RunAs, privilege adjustment or endpoint API.
    check(elevated(),"Fresh separately authorized elevation required");auto dll=fs::absolute(argv[2]);auto src=fs::absolute(argv[3]);uint64_t domain=std::stoull(argv[4]),epoch=std::stoull(argv[5]);DWORD pid=static_cast<DWORD>(std::stoul(argv[6]));uint64_t expected=std::stoull(argv[7]);auto out=fs::absolute(argv[8]);
    // Reserved final marker is deliberate: prevent accidentally invoking a
    // native run with arguments copied from a diagnostic/selftest command.
    check(!wcscmp(argv[9],L"NO_RETRY_NO_ENDPOINT_CHANGES")&&domain>=1&&domain<=2&&epoch&&pid&&expected&&!fs::exists(out),"Explicit native session identity and fresh output");
    check(equalPath(dll,L"C:/ProgramData/SpatialObjectSpike/mat_ipc_receiver.dll")&&hash(dll)==helperHash,"Previously staged immutable helper path/hash");components();auto ids=engines();check(ids.size()==1&&ids[0]==pid,"Exactly one existing approved engine; no restart");auto packets=source(src);
    check(hash(src)==(domain==1?"aa345cc683f3daa98c680a9a25a73e04ec3272a74f86c028a535ae347fb729a8":"1ff71d6cd0e27d8ecbdc5af6adf769df51001fdd0f99d449f49c67bec360fafc"),"Approved frozen source for selected domain");
    Handle target(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid));check(target.h!=nullptr,"Open approved engine; stop on access denied");check(created(target.h)==expected&&equalPath(image(target.h),system(L"audiodg.exe")),"Approved engine creation time/system image");auto audio=module(pid,L"audioeng.dll");check(audio.base&&equalPath(audio.path,system(L"audioeng.dll")),"Already loaded system audioeng");auto r=session(target.h,pid,dll,packets,domain,epoch,out);std::cout<<r.dump(2)<<'\n';return r["completed"].get<bool>()?0:2;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 3;}}
