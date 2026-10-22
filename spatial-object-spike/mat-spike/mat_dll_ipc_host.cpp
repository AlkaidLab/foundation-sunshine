// Ordinary MSVC child host for the FROZEN DLL's FAKE export only.
#include "mat_ipc_receiver_api.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
using namespace receiver;using Json=nlohmann::json;namespace fs=std::filesystem;
static void require(bool okay,const char* text){if(!okay)throw std::runtime_error(text);}
struct Handle {HANDLE value{};explicit Handle(HANDLE h=nullptr):value(h){}~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
struct View {void* ptr{};View(HANDLE h,size_t bytes){ptr=MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,bytes);require(ptr!=nullptr,"own mapping view");}~View(){if(ptr)UnmapViewOfFile(ptr);}};
static HANDLE asHandle(const wchar_t* p){return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(p)));}
static void save(const fs::path& path,const Json& value){std::ofstream f(path);f<<value.dump(2)<<'\n';require(bool(f),"JSON output");}
template<class T>static void write(std::ofstream& f,const T& t){f.write(reinterpret_cast<const char*>(&t),sizeof(t));require(bool(f),"binary output");}
static uint64_t created(HANDLE h){FILETIME a,b,c,d;require(GetProcessTimes(h,&a,&b,&c,&d)!=0,"provided owner creation time");return (uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
struct Invocation {Args args{};FakeConfig config{sizeof(FakeConfig),apiVersion,0,-1,-1,0};FakeStart fake{};static DWORD WINAPI run(void* p){auto& call=*static_cast<Invocation*>(p);return call.fake(&call.args,&call.config);}};
int wmain(int argc,wchar_t** argv){try{
    require(argc==18&&!wcscmp(argv[1],L"--receive"),"ordinary DLL receiver arguments");
    Handle mapping(asHandle(argv[2])),data(asHandle(argv[3])),cancel(asHandle(argv[4])),ready(asHandle(argv[5])),owner(asHandle(argv[6]));
    uint64_t domain=std::stoull(argv[7]),epoch=std::stoull(argv[8]),n0=std::stoull(argv[9]),n1=std::stoull(argv[10]);DWORD ownerPid=static_cast<DWORD>(std::stoull(argv[11]));auto ownerCreated=std::stoull(argv[12]);
    auto out=fs::absolute(argv[13]);std::wstring mode=argv[14];uint32_t sourceRecords=static_cast<uint32_t>(std::stoull(argv[15]));auto sourceHz=std::stoull(argv[16]);auto dllPath=fs::absolute(argv[17]);
    require(sourceRecords>=2&&sourceRecords<=351&&sourceHz>0&&domain&&epoch,"bounded source/session dimensions");
    require(!fs::exists(out)&&fs::create_directory(out),"new DLL receiver output");View input(mapping.value,sizeof(Shared));auto& shared=*static_cast<Shared*>(input.ptr);
    Invocation call;call.args={sizeof(Args),apiVersion,mapping.value,data.value,cancel.value,owner.value,nullptr,domain,epoch,{n0,n1},ownerCreated,ownerPid,sourceRecords-1,10,0};
    // Structural and identity preflight is explicit; no invalid contract is
    // handed to the DLL. The DLL repeats its own checks on a valid session.
    Args structure=call.args;structure.domain=shared.contract.domain;structure.epoch=shared.contract.epoch;structure.nonce[0]=shared.contract.nonce[0];structure.nonce[1]=shared.contract.nonce[1];structure.ownerPid=shared.contract.producerPid;
    LONG guard=contract(shared,structure)?0:badContract;
    if(!guard&&(!contract(shared,call.args)||GetProcessId(owner.value)!=ownerPid||created(owner.value)!=ownerCreated))guard=identity;
    if(guard){fail(shared,guard);put(shared.common.consumerStatus,5);SetEvent(ready.value);save(out/L"summary.json",{{"completed",false},{"fault",guard},{"guard_passed",false},{"dll_loaded",false},{"dll_invoked",false},{"wrapper_fixture_only",false},{"native_encoder_called",false},{"vendor_loaded",false}});return 2;}
    if(mode==L"consumer-exit"||mode==L"overflow"||mode==L"consumer-stale"){
        // These are labelled wrapper-only liveness/capacity fixtures; they do
        // NOT count as evidence that a DLL codec thread was stalled or exited.
        require(InterlockedCompareExchange(&shared.common.consumerPid,GetCurrentProcessId(),0)==0,"fixture PID claim");put(shared.reader.heartbeat,qpc());put(shared.common.consumerStatus,1);SetEvent(ready.value);
        if(mode==L"consumer-exit"){save(out/L"abrupt-exit.json",{{"code",31},{"intentional_own_helper_exit",true},{"wrapper_fixture_only",true},{"dll_invoked",false},{"native_encoder_called",false}});ExitProcess(31);}
        HANDLE waits[]{cancel.value,owner.value};auto result=WaitForMultipleObjects(2,waits,FALSE,3000);if(result==WAIT_OBJECT_0+1)fail(shared,producerExited);if(result==WAIT_TIMEOUT)fail(shared,timeout);put(shared.common.consumerStatus,5);
        save(out/L"summary.json",{{"completed",false},{"fault",get(shared.common.fault)},{"guard_passed",true},{"dll_loaded",false},{"dll_invoked",false},{"wrapper_fixture_only",true},{"native_encoder_called",false},{"vendor_loaded",false}});return 2;
    }
    Handle outputMap(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(FakeOutput),nullptr));require(outputMap.value!=nullptr,"bounded fake diagnostics mapping");View output(outputMap.value,sizeof(FakeOutput));auto& state=*static_cast<FakeOutput*>(output.ptr);auto& o=state.header;
    o.magic=outputMagic;o.version=apiVersion;o.bytes=sizeof(FakeOutput);o.kind=2;o.createHr=o.mapperHr=o.initializeHr=o.submitHr=o.processHr=E_PENDING;call.args.output=outputMap.value;
    HMODULE dll=LoadLibraryExW(dllPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);require(dll!=nullptr,"load own frozen receiver DLL");
    call.fake=reinterpret_cast<FakeStart>(GetProcAddress(dll,"TestMatIpcReceiverFake"));require(call.fake&&GetProcAddress(dll,"StartMatIpcReceiver"),"own DLL exports; native export is NOT called");
    DWORD entryTid{};Handle thread(CreateThread(nullptr,0,Invocation::run,&call,0,&entryTid));require(thread.value!=nullptr,"own DLL entry thread");auto began=GetTickCount64();
    while(get(shared.common.consumerPid)!=LONG(GetCurrentProcessId())&&WaitForSingleObject(thread.value,2)!=WAIT_OBJECT_0&&GetTickCount64()-began<3000){}
    bool claimed=get(shared.common.consumerPid)==LONG(GetCurrentProcessId());
    if(claimed){put(shared.common.consumerStatus,1);SetEvent(ready.value);}else{if(!get(shared.common.fault))fail(shared,badContract);SetEvent(ready.value);SetEvent(cancel.value);SetEvent(data.value);}
    // Do not forge DLL heartbeats or detect owner exit on its behalf. The
    // actual DLL receiver must update heartbeat and observe the owner handle.
    auto until=GetTickCount64()+12000;
    while(WaitForSingleObject(thread.value,20)!=WAIT_OBJECT_0&&GetTickCount64()<until){if(get(shared.common.fault)){SetEvent(cancel.value);SetEvent(data.value);}}
    if(WaitForSingleObject(thread.value,0)!=WAIT_OBJECT_0){fail(shared,timeout);SetEvent(cancel.value);SetEvent(data.value);require(WaitForSingleObject(thread.value,2000)==WAIT_OBJECT_0,"own fake entry exceeded cooperative bound; no forced termination");}
    DWORD code{};GetExitCodeThread(thread.value,&code);require(!o.workerTimedOut,"fake codec worker still active; do not consume its diagnostics");
    bool complete=code==0&&o.status==2&&!o.error&&!get(shared.common.fault)&&o.workerFinished;
    if(!complete&&!get(shared.common.fault))fail(shared,codec);
    require(o.processes<=roundLimit,"bounded fake Process count");
    std::ofstream plan(out/L"fake-plan.bin",std::ios::binary),clock(out/L"process-clock.csv");clock<<"phase,source_index,start_qpc,finish_qpc\n";
    for(uint32_t n=0;n<o.processes;++n){auto& t=state.ticks[n];require(t.count<=64,"bounded fake call count");write(plan,t.phase);write(plan,t.sourceIndex);write(plan,t.count);for(uint32_t j=0;j<t.count;++j){auto& c=t.calls[j];write(plan,c.id);write(plan,c.flags);write(plan,c.xyz);write(plan,c.pcm);}clock<<t.phase<<','<<t.sourceIndex<<','<<t.startQpc<<','<<t.finishQpc<<'\n';}
    plan.close();clock.close();require(bool(plan)&&bool(clock),"close fake diagnostics");
    wchar_t loadedPath[32768]{};require(GetModuleFileNameW(dll,loadedPath,32768)>0,"loaded own DLL path");
    bool vendor=GetModuleHandleW(L"DolbyMatEnc.dll")||GetModuleHandleW(L"DolbyMatEngine.dll");require(!vendor&&!o.vendorLoads&&!o.activations&&!o.hostGuardPassed&&!o.helperPinned&&!o.records&&!o.outputBytes,"fake wrapper must never activate native path");
    put(shared.common.consumerStatus,complete?4:5);
    save(out/L"summary.json",{{"completed",complete},{"fault",get(shared.common.fault)},{"guard_passed",true},{"dll_loaded",true},{"dll_invoked",true},{"wrapper_fixture_only",false},{"dll_path",fs::path(loadedPath).string()},
        {"entry_return",code},{"dll_status",o.status},{"dll_error",o.error},{"consumer_pid",GetCurrentProcessId()},{"host_tid",GetCurrentThreadId()},{"entry_tid",entryTid},{"worker_tid",o.workerTid},{"worker_finished",bool(o.workerFinished)},{"worker_timed_out",bool(o.workerTimedOut)},
        {"domain",domain},{"epoch",epoch},{"messages_consumed",o.messages},{"eof_seen",bool(o.eofSeen)},{"source_process_rounds",o.sourceRounds},{"source_samples",o.sourceSamples},{"preroll_rounds",o.warmRounds},{"drain_rounds",o.drainRounds},{"submissions",o.submissions},{"scheduled_rounds",o.processes},
        {"fake_calls_verified",o.fakeCallsVerified},{"queue_capacity",8},{"queue_high_water",o.localHighWater},{"initializations",o.initializations},{"vendor_loads",o.vendorLoads},{"activations",o.activations},{"host_guard_passed",bool(o.hostGuardPassed)},
        {"records",o.records},{"output_bytes",o.outputBytes},{"endpoint_calls",o.endpointCalls},{"hooks_installed",o.hooksInstalled},{"helper_pinned",bool(o.helperPinned)},{"qpc_frequency",hz()},{"native_encoder_called",false},{"mat_encoded",false},{"vendor_loaded",false},{"live_capture_verified",false}});
    FreeLibrary(dll);return complete?0:2;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 3;}}
