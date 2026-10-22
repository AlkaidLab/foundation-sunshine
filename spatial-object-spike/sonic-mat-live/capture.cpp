// Explicit native Sonic object -> dedicated native MAT -> private live pipe.
// No recorded input, source synthesis, default endpoint change or elevation.
#define MAT_LIVE_PUBLISHER_HELPERS_ONLY
#define NOMINMAX
#include "bridge.h"
#include "native_queue.h"
#include "../../src/spatial_audio/stream_ring_source.h"
#include "../mat-live/capture.cpp"
#include <memory>
static uintptr_t LoadReviewed(HANDLE engine,DWORD pid,const fs::path& path,const std::string& hash,bool reattach){
    require(path.is_absolute()&&hash.size()==64&&native_mat_guard::sha256(path)==hash,"Reviewed native DLL hash");
    auto prior=LoadedTap(pid,path);require(reattach||!prior,"Native DLL already loaded; explicit reattach required");
    if(prior)return prior;
    auto loaded=LoadTap(engine,pid,path);require(LoadedTap(pid,path)==loaded,"Native loaded DLL path");return loaded;
}
static uintptr_t Export(const fs::path& path,uintptr_t loaded,const char* name){
    auto local=LoadLibraryExW(path.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local,"Read native exports");
    auto entry=GetProcAddress(local,name);auto address=entry?loaded+(uintptr_t)entry-(uintptr_t)local:0;FreeLibrary(local);require(address!=0,"Native export");return address;
}
static HANDLE DuplicateTo(HANDLE engine,HANDLE source,DWORD access=0){
    HANDLE remote{};require(DuplicateHandle(GetCurrentProcess(),source,engine,&remote,access,FALSE,access?0:DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate native session handle");return remote;
}
static void CloseRemote(HANDLE engine,HANDLE remote){if(!remote)return;HANDLE copy{};if(DuplicateHandle(engine,remote,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_CLOSE_SOURCE))CloseHandle(copy);}
static HANDLE Map(size_t size){auto h=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,(DWORD)size,nullptr);require(h,"Create bounded native session mapping");return h;}
static bool OriginalSonicBytes(HANDLE engine){
    const DWORD rvas[]={0xf7880,0x433f0,0x79e20};
    const BYTE expected[][8]={{0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x44},{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48},{0x40,0x53,0x48,0x83,0xec,0x40,0x80,0x79}};
    auto base=Module(GetProcessId(engine),L"audioeng.dll");if(!base)return false;
    for(unsigned n=0;n<3;++n){BYTE bytes[8];SIZE_T got=0;if(!ReadProcessMemory(engine,(void*)(base+rvas[n]),bytes,8,&got)||got!=8||memcmp(bytes,expected[n],8))return false;}return true;
}
class NativeSession {
    HANDLE engine_,input_map_{},output_map_{},data_{},cancel_{},thread_{};
    receiver::Shared* input_{};spatial_stream::Output* output_{};void* arguments_{};
    std::vector<HANDLE> remote_;std::thread consumer_;Shared& pipe_;uint64_t domain_,epoch_;bool finished_=false,reported_=false;
    void report(){
        if(!output_||reported_)return;reported_=true;const auto& s=output_->header;
        printf("NATIVE_WORKER sourceRounds=%llu sourceSamples=%llu processes=%llu outputBytes=%llu warm=%llu drain=%llu status=%ld error=%ld vendorLoads=%llu activations=%llu initialize=%llu createHR=0x%08x mapperHR=0x%08x initializeHR=0x%08x submitHR=0x%08x processHR=0x%08x\n",s.sourceRounds,s.sourceSamples,s.processes,s.outputBytes,s.warmRounds,s.drainRounds,s.status,s.error,s.vendorLoads,s.activations,s.initializations,(unsigned)s.createHr,(unsigned)s.mapperHr,(unsigned)s.initializeHr,(unsigned)s.submitHr,(unsigned)s.processHr);
    }
    void consume(){
        try {
            spatial_audio::StreamRingSource source(*output_,epoch_,domain_);require(source.start(),"Native MAT output contract");
            auto progress=GetTickCount64();spatial_audio::Frame frame;
            for(;;){auto status=source.read(frame);
                if(status==spatial_audio::Read::pending){require(GetTickCount64()-progress<2000,"Native Sonic/MAT worker output stalled");Sleep(1);continue;}
                require(status!=spatial_audio::Read::failed,source.error().c_str());
                if(status==spatial_audio::Read::ended){
                    require(WaitForSingleObject(thread_,2000)==WAIT_OBJECT_0,"Native MAT final status timeout");
                    require(output_->header.eofSeen&&output_->header.workerFinished&&!output_->header.error&&output_->header.status==2,"Native worker did not finish cleanly");
                    std::lock_guard lock(pipe_.mutex);pipe_.ended=true;pipe_.changed.notify_all();break;
                }
                auto qpc=source.capture_qpc();require(qpc>0,"Native MAT completion clock");progress=GetTickCount64();
                std::lock_guard lock(pipe_.mutex);bool first=!pipe_.ready;pipe_.ready=true;pipe_.encoder=domain_;pipe_.last_cycle=progress;++pipe_.cycles;
                if(first)printf("NATIVE_MAT_READY source=sonic-reencode carrierBytes=61440 inputRenderer=0x%llx fileInput=0\n",domain_);
                if(pipe_.playing&&qpc>=pipe_.play_qpc){require(pipe_.queue.size()<4,"Live consumer overflow; no drop/replay");Cycle cycle{};memcpy(cycle.bytes.data(),frame.bytes.data(),frame.bytes.size());cycle.qpc=qpc;pipe_.queue.push_back(std::move(cycle));pipe_.changed.notify_all();}
            }
        }catch(const std::exception& e){Broken(pipe_,e.what());if(input_)receiver::fail(*input_,receiver::codec);if(cancel_)SetEvent(cancel_);}
    }
    void cleanup(){
        if(input_&&!finished_)receiver::fail(*input_,receiver::cancelled);if(cancel_&&!finished_)SetEvent(cancel_);
        if(consumer_.joinable())consumer_.join();
        bool done=!thread_||WaitForSingleObject(thread_,10000)==WAIT_OBJECT_0;
        if(done)report();else puts("NATIVE_WORKER blocked-call; retained remote state; no forced unload");
        // No forced unload or thread termination under a blocked vendor call.
        if(done){for(auto h:remote_)CloseRemote(engine_,h);remote_.clear();if(arguments_)VirtualFreeEx(engine_,arguments_,0,MEM_RELEASE);}
        if(thread_)CloseHandle(thread_);if(input_)UnmapViewOfFile(input_);if(output_)UnmapViewOfFile(output_);
        for(auto h:{input_map_,output_map_,data_,cancel_})if(h)CloseHandle(h);
    }
public:
    NativeSession(HANDLE engine,const fs::path& codec,uintptr_t loaded,uint64_t domain,uint64_t epoch,Shared& pipe):engine_(engine),pipe_(pipe),domain_(domain),epoch_(epoch){
        try {
            input_map_=Map(sizeof(receiver::Shared));output_map_=Map(sizeof(spatial_stream::Output));
            input_=(receiver::Shared*)MapViewOfFile(input_map_,FILE_MAP_ALL_ACCESS,0,0,sizeof(receiver::Shared));
            output_=(spatial_stream::Output*)MapViewOfFile(output_map_,FILE_MAP_ALL_ACCESS,0,0,sizeof(spatial_stream::Output));require(input_&&output_,"Map native queues");
            uint64_t nonce[2];
            require(BCryptGenRandom(nullptr,(PUCHAR)nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Native session nonce");
            sonic_live::initialize_native_queue(*input_,*output_,domain,epoch,nonce);
            data_=CreateEventW(nullptr,FALSE,FALSE,nullptr);cancel_=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(data_&&cancel_,"Native queue events");
            auto dup=[&](HANDLE h,DWORD access=0){auto copy=DuplicateTo(engine_,h,access);remote_.push_back(copy);return copy;};
            spatial_stream::Args args{sizeof(args),spatial_stream::streamVersion,dup(input_map_),dup(data_),dup(cancel_),dup(GetCurrentProcess(),SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION),dup(output_map_),domain,epoch,{nonce[0],nonce[1]},native_mat_guard::created(GetCurrentProcess()),GetCurrentProcessId(),0,0,0};
            arguments_=RemoteCopy(engine_,&args,sizeof(args));thread_=RemoteThread(engine_,Export(codec,loaded,"StartMatStreamReceiver"),arguments_);
            consumer_=std::thread([this]{consume();});
        }catch(...){cleanup();throw;}
    }
    ~NativeSession(){cleanup();}
    bool send(const receiver::Slot& packet){
        return sonic_live::enqueue_native(*input_,packet,data_);
    }
    void heartbeat(){receiver::put(input_->writer.heartbeat,receiver::qpc());}
    void finish(){
        auto until=GetTickCount64()+2000;while(WaitForSingleObject(thread_,10)==WAIT_TIMEOUT&&GetTickCount64()<until)heartbeat();
        require(WaitForSingleObject(thread_,0)==WAIT_OBJECT_0,"Native MAT drain timeout");if(consumer_.joinable())consumer_.join();
        const auto& s=output_->header;report();
        require(s.status==2&&!s.error&&s.workerFinished&&!s.workerTimedOut&&s.eofSeen&&s.vendorLoads==1&&s.activations==1&&s.initializations==1&&s.hostGuardPassed&&s.createHr==S_OK&&s.mapperHr==S_OK&&s.initializeHr==S_OK&&s.submitHr==S_OK&&s.processHr==S_OK,"Native MAT worker result");finished_=true;
    }
};
int wmain(int argc,wchar_t** argv){
    if((argc!=10&&argc!=12)||(std::wstring(argv[1])!=L"--attach"&&std::wstring(argv[1])!=L"--reattach")){puts("sonic-mat-capture --attach/--reattach PID CREATED TAP_ABSOLUTE TAP_SHA256 CODEC_ABSOLUTE CODEC_SHA256 LIVE_PIPE STOP_EVENT [--max-seconds 1..60]");return 2;}
    HANDLE engine{},map{},thread{},stop{};void* arguments{};sonic_live::Ring* ring{};std::thread controller;Shared shared;
    std::unique_ptr<NativeSession> native;HANDLE remoteMap{},remoteOwner{};bool handed=false;int result=1;
    setvbuf(stdout,nullptr,_IONBF,0);SetConsoleCtrlHandler(Ctrl,TRUE);
    try {
        uint64_t limit=0;if(argc==12){require(std::wstring(argv[10])==L"--max-seconds","Bounded capture option");limit=std::stoull(argv[11]);require(limit&&limit<=60,"Capture duration 1..60 seconds");}
        uint64_t pid64=std::stoull(argv[2]),creation=std::stoull(argv[3]);require(pid64&&pid64<=MAXDWORD&&creation,"Explicit audiodg identity");DWORD pid=(DWORD)pid64;
        engine=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid);
        require(engine&&native_mat_guard::created(engine)==creation&&native_mat_guard::system_engine(engine),"Open pinned audiodg; no privilege adjustment/retry");
        require(OriginalSonicBytes(engine),"Original pinned Sonic entry points");
        fs::path tap=fs::canonical(argv[4]),codec=fs::canonical(argv[6]);std::wstring th=argv[5],ch=argv[7];
        HANDLE tapLock=CreateFileW(tap.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr),codecLock=CreateFileW(codec.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        struct Unlock {HANDLE a,b;~Unlock(){if(a!=INVALID_HANDLE_VALUE)CloseHandle(a);if(b!=INVALID_HANDLE_VALUE)CloseHandle(b);}} unlock{tapLock,codecLock};
        require(tapLock!=INVALID_HANDLE_VALUE&&codecLock!=INVALID_HANDLE_VALUE,"Lock reviewed native DLLs");
        require(native_mat_guard::sha256(tap)==std::string(th.begin(),th.end())&&native_mat_guard::sha256(codec)==std::string(ch.begin(),ch.end()),"Both native DLL hashes before loading");
        std::wstring pipe=argv[8],event=argv[9];require(pipe.starts_with(L"\\\\.\\pipe\\SunshineMatLive-")&&pipe.find_first_of(L"\r\n")==std::wstring::npos&&event.starts_with(L"Local\\SunshineMatLiveStop-"),"Explicit private pipe/stop event");
        auto acl=PipeAcl();SECURITY_ATTRIBUTES sa{sizeof(sa),acl,FALSE};stop=CreateEventW(&sa,TRUE,FALSE,event.c_str());auto code=GetLastError();LocalFree(acl);require(stop&&code!=ERROR_ALREADY_EXISTS,"Fresh native stop event");
        const bool reattach=std::wstring(argv[1])==L"--reattach";
        auto tapBase=LoadReviewed(engine,pid,tap,std::string(th.begin(),th.end()),reattach);auto codecBase=LoadReviewed(engine,pid,codec,std::string(ch.begin(),ch.end()),reattach);
        map=Map(sizeof(sonic_live::Ring));ring=(sonic_live::Ring*)MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(*ring));require(ring,"Map Sonic capture queue");memset(ring,0,sizeof(*ring));ring->magic=sonic_live::magic;ring->version=sonic_live::version;InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        remoteMap=DuplicateTo(engine,map);remoteOwner=DuplicateTo(engine,GetCurrentProcess(),SYNCHRONIZE);
        StartArgs args{remoteMap,remoteOwner};arguments=RemoteCopy(engine,&args,sizeof(args));thread=RemoteThread(engine,Export(tap,tapBase,"StartCapture"),arguments);handed=true;
        auto began=GetTickCount64();while(!ring->status&&GetTickCount64()-began<2000&&WaitForSingleObject(thread,10)==WAIT_TIMEOUT)InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        require(ring->status==1,"Sonic capture hook readiness");auto epoch=(Qpc()&INT64_MAX)|1;
        controller=std::thread([&,codec,codecBase,epoch,began,limit]{sonic_live::Bridge bridge;bool observed=false;auto last=GetTickCount64();try {
            for(;;){auto now=GetTickCount64();InterlockedExchange64(&ring->heartbeat,now);if(native)native->heartbeat();
                require(!Read64(&ring->dropped)&&!Read64(&ring->invalid)&&!Read64(&ring->unbound)&&!ring->error,"Sonic callback loss/ABI violation");
                require(WaitForSingleObject(engine,0)==WAIT_TIMEOUT,"Pinned engine exited; no reattach");
                auto r=Read64(&ring->read),w=Read64(&ring->write);require(r>=0&&w>=r&&w-r<=sonic_live::capacity,"Sonic ring bounds");
                while(r<w){auto& p=ring->slots[r%sonic_live::capacity];
                    if(p.count&&!observed){observed=true;const auto& o=p.objects[0];printf("SONIC_OBSERVED renderer=0x%llx rate=%u deviceFrames=%u requestedFrames=%u objects=%u firstOffset=%u firstFrames=%u firstHR=0x%08x\n",p.renderer,p.rate,p.deviceFrames,p.requestedFrames,p.count,o.offset,o.frames,(unsigned)o.result);}
                    bridge.push(p,[&](const receiver::Slot& packet){if(!native){printf("SONIC_SOURCE renderer=0x%llx rate=%u deviceFrames=%u requestedFrames=%u objects=%u\n",bridge.renderer(),p.rate,p.deviceFrames,p.requestedFrames,p.count);native=std::make_unique<NativeSession>(engine,codec,codecBase,bridge.renderer(),epoch,shared);}return native->send(packet);});
                    InterlockedExchange64(&ring->read,++r);last=GetTickCount64();
                }
                {std::lock_guard lock(shared.mutex);require(!shared.failed,"Native MAT output/consumer failed");}
                if(limit&&GetTickCount64()-began>=limit*1000)SetEvent(stop);
                if(interrupted||WaitForSingleObject(stop,0)==WAIT_OBJECT_0)InterlockedExchange(&ring->stop,1);
                if(WaitForSingleObject(thread,0)==WAIT_OBJECT_0){if(Read64(&ring->read)!=Read64(&ring->write))continue;
                    require(ring->status==2&&!ring->error&&(interrupted||WaitForSingleObject(stop,0)==WAIT_OBJECT_0),"Unclean Sonic capture end");
                    require(bool(native),"No usable Sonic object source observed");bridge.finish([&](const receiver::Slot& packet){return native->send(packet);});native->finish();
                    printf("SONIC_STOP passes=%llu inputSamples=%llu emptyPasses=%llu restored=1\n",bridge.sourcePasses,bridge.inputSamples,bridge.emptyPasses);break;
                }
                if(native)require(GetTickCount64()-last<2000,"Sonic renderer stopped advancing");Sleep(1);
            }
        }catch(const std::exception& e){Broken(shared,e.what());InterlockedExchange(&ring->stop,1);}});
        mat_capture::Descriptor descriptor;descriptor.kind=mat_capture::native_sonic_source;descriptor.publisher_pid=GetCurrentProcessId();descriptor.publisher_created=native_mat_guard::created(GetCurrentProcess());descriptor.engine_pid=pid;descriptor.engine_created=creation;descriptor.epoch=epoch;descriptor.qpc_hz=receiver::hz();
        printf("ATTACHED source=sonic-objects-to-native-MAT engine=%lu fileInput=0 endpointChange=0\n",pid);Serve(shared,pipe,descriptor);
        InterlockedExchange(&ring->stop,1);controller.join();require(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0&&ring->status==2&&!ring->error,"Sonic hooks did not restore cleanly");
        printf("STOP cycles=%llu failed=%d error=%s restored=1 calls=%lld passes=%lld unbound=%lld dropped=%lld invalid=%lld\n",shared.cycles,shared.failed,shared.error.c_str(),Read64(&ring->monoCalls),Read64(&ring->passes),Read64(&ring->unbound),Read64(&ring->dropped),Read64(&ring->invalid));result=shared.failed?1:0;
    }catch(const std::exception& e){printf("FAILED %s win32=%lu\n",e.what(),GetLastError());if(ring)InterlockedExchange(&ring->stop,1);InterlockedExchange(&interrupted,1);if(controller.joinable())controller.join();}
    native.reset();bool done=!thread||WaitForSingleObject(thread,10000)==WAIT_OBJECT_0;
    if(engine&&ring){bool restored=done&&OriginalSonicBytes(engine);printf("HOOK_STOP restored=%d nativeStatus=%ld nativeError=%ld calls=%lld passes=%lld unbound=%lld dropped=%lld invalid=%lld\n",restored,ring->status,ring->error,Read64(&ring->monoCalls),Read64(&ring->passes),Read64(&ring->unbound),Read64(&ring->dropped),Read64(&ring->invalid));if(!restored)result=1;}
    if(!handed&&engine){CloseRemote(engine,remoteMap);CloseRemote(engine,remoteOwner);}if(done&&arguments)VirtualFreeEx(engine,arguments,0,MEM_RELEASE);
    if(ring)UnmapViewOfFile(ring);if(thread)CloseHandle(thread);if(map)CloseHandle(map);if(stop)CloseHandle(stop);if(engine)CloseHandle(engine);return result;
}
