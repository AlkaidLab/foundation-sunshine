#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "../resident/collector.cpp"
#include "../../src/spatial_audio/native_mat_guard.h"
#include "../../src/spatial_audio/native_mat_assembler.h"
#include <sddl.h>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"psapi.lib")
using spatial_audio::require;
struct Cycle {std::array<uint8_t,MAT_WIRE_CYCLE> bytes;uint64_t qpc;};
struct Shared {
    std::mutex mutex;std::condition_variable changed;std::deque<Cycle> queue;
    bool failed=false,ended=false,playing=false,ready=false;
    uint64_t encoder=0,play_qpc=0,last_cycle=0,cycles=0,skipped=0;
    mat_capture::Observations observations;
    std::string error;
};
static uint64_t Qpc(){LARGE_INTEGER n{};require(QueryPerformanceCounter(&n)!=FALSE,"QPC");return n.QuadPart;}
static void Broken(Shared& s,const char* message){std::lock_guard lock(s.mutex);s.failed=true;s.error=message;s.queue.clear();s.changed.notify_all();}
static uintptr_t LoadedTap(DWORD pid,const fs::path& expected) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);
    require(snap!=INVALID_HANDLE_VALUE,"Enumerate loaded tap identity");
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);uintptr_t address=0;
    try {
        require(Module32FirstW(snap,&entry)!=FALSE,"Read loaded module list");
        do {
            if(_wcsicmp(entry.szModule,expected.filename().c_str()))continue;
            auto actual=fs::canonical(entry.szExePath);
            require(!address && _wcsicmp(expected.c_str(),actual.c_str())==0,"Loaded tap path differs; no reuse");
            address=reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        }while(Module32NextW(snap,&entry));
    }catch(...){CloseHandle(snap);throw;}
    CloseHandle(snap);return address;
}
static bool Io(HANDLE pipe,void* buffer,DWORD size,bool write,DWORD timeout) {
    auto* b=static_cast<BYTE*>(buffer);DWORD done=0;auto began=GetTickCount64();
    while(done<size && !interrupted) {
        OVERLAPPED o{};o.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!o.hEvent)return false;
        DWORD n=0;BOOL okay=write?WriteFile(pipe,b+done,size-done,&n,&o):ReadFile(pipe,b+done,size-done,&n,&o);
        if(!okay && GetLastError()==ERROR_IO_PENDING) {
            while(WaitForSingleObject(o.hEvent,20)==WAIT_TIMEOUT && !interrupted && GetTickCount64()-began<timeout){}
            if(interrupted || GetTickCount64()-began>=timeout){CancelIoEx(pipe,&o);GetOverlappedResult(pipe,&o,&n,TRUE);CloseHandle(o.hEvent);return false;}
            okay=GetOverlappedResult(pipe,&o,&n,FALSE);
        }
        CloseHandle(o.hEvent);if(!okay||!n)return false;done+=n;
    }
    return done==size;
}
static PSECURITY_DESCRIPTOR PipeAcl() {
    HANDLE token{};require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=FALSE,"Pipe token");
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
    bool okay=GetTokenInformation(token,TokenUser,bytes.data(),size,&size)!=FALSE;CloseHandle(token);require(okay,"Pipe token user");
    LPWSTR sid{};require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)!=FALSE,"Pipe SID");
    std::wstring acl=L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR result{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&result,nullptr)!=FALSE,"Private pipe ACL");return result;
}
static void Serve(Shared& shared,const std::wstring& name,mat_capture::Descriptor descriptor) {
    auto acl=PipeAcl();SECURITY_ATTRIBUTES attributes{sizeof(attributes),acl,FALSE};
    try {
        while(!interrupted) {
            {std::lock_guard lock(shared.mutex);if(shared.failed||shared.ended)break;}
            HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,131072,4096,0,&attributes);
            require(pipe!=INVALID_HANDLE_VALUE,"Exclusive local MAT pipe");
            try {
                OVERLAPPED o{};o.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(o.hEvent!=nullptr,"Pipe connect event");
                BOOL connected=ConnectNamedPipe(pipe,&o);DWORD code=connected?0:GetLastError();
                if(!connected && code==ERROR_PIPE_CONNECTED)connected=TRUE;
                if(!connected && code==ERROR_IO_PENDING){
                    while(WaitForSingleObject(o.hEvent,20)==WAIT_TIMEOUT && !interrupted){std::lock_guard lock(shared.mutex);if(shared.failed||shared.ended)break;}
                    DWORD n=0;if(interrupted){CancelIoEx(pipe,&o);}else {std::lock_guard lock(shared.mutex);if(shared.failed||shared.ended)CancelIoEx(pipe,&o);}
                    connected=GetOverlappedResult(pipe,&o,&n,TRUE);
                }
                CloseHandle(o.hEvent);
                char command[8];require(connected && Io(pipe,command,8,false,2000) && !memcmp(command,"MCAPOPEN",8),"Live source lease open");
                {
                    std::lock_guard lock(shared.mutex);
                    require(shared.ready && !shared.failed && GetTickCount64()-shared.last_cycle<2000,"Native MAT not ready");
                    descriptor.encoder=shared.encoder;
                }
                uint8_t header[mat_capture::descriptor_bytes];mat_capture::encode_descriptor(header,descriptor);
                require(Io(pipe,header,sizeof(header),true,2000),"Live source descriptor");
                require(Io(pipe,command,8,false,30000) && !memcmp(command,"MCAPPLAY",8),"Live source lease play timeout/disconnect");
                {std::lock_guard lock(shared.mutex);require(!shared.failed && shared.ready && GetTickCount64()-shared.last_cycle<2000,"Source stale at PLAY");
                    shared.queue.clear();shared.play_qpc=Qpc();shared.playing=true;}
                uint64_t sequence=0;
                while(!interrupted) {
                    Cycle cycle{};bool end=false;
                    {
                        std::unique_lock lock(shared.mutex);
                        shared.changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return shared.failed||shared.ended||!shared.queue.empty();});
                        require(!shared.failed,"Native capture failed (no EOS)");
                        if(shared.queue.empty()){if(shared.ended)end=true;else {require(GetTickCount64()-shared.last_cycle<2000,"Native capture stalled");continue;}}
                        else {cycle=std::move(shared.queue.front());shared.queue.pop_front();}
                    }
                    uint8_t record[mat_capture::record_bytes];
                    mat_capture::encode_record(record,end?mat_capture::end_kind:mat_capture::cycle_kind,sequence,end?Qpc():cycle.qpc,
                        end?nullptr:cycle.bytes.data(),end?0:cycle.bytes.size());
                    require(Io(pipe,record,sizeof(record),true,2000) && (end||Io(pipe,cycle.bytes.data(),(DWORD)cycle.bytes.size(),true,2000)),"Live source consumer disconnected/backpressure");
                    if(end)break;++sequence;
                }
            }catch(const std::exception& e){printf("LEASE_CLOSED %s\n",e.what());}
            {std::lock_guard lock(shared.mutex);shared.playing=false;shared.queue.clear();}
            CancelIoEx(pipe,nullptr);DisconnectNamedPipe(pipe);CloseHandle(pipe);
        }
    }catch(...){LocalFree(acl);throw;}LocalFree(acl);
}
int wmain(int argc,wchar_t** argv) {
    // Read-only preflight by default. No debug-privilege adjustment, UAC, engine restart or endpoint change.
    if(argc!=4 && argc!=8){puts("sunshine-mat-capture --preflight PID CREATION_TIME | --attach/--reattach PID CREATION_TIME TAP_ABSOLUTE SHA256 \\\\.\\pipe\\SunshineMatLive-NAME STOP_EVENT_NAME");return 2;}
    const bool reuse=argc==8 && std::wstring(argv[1])==L"--reattach";
    const bool attach=argc==8 && (std::wstring(argv[1])==L"--attach" || reuse);
    if(!attach && (argc!=4 || std::wstring(argv[1])!=L"--preflight"))return 2;
    HANDLE process{},mapping{},thread{},stop_event{};EncodedRing* ring{};void* arguments{};bool handed=false;
    HANDLE remote_mapping{},remote_collector{};Shared shared;std::thread collector;int result=1;
    SetConsoleCtrlHandler(Ctrl,TRUE);setvbuf(stdout,nullptr,_IONBF,0);
    try {
        uint64_t pid64=std::stoull(argv[2]),creation=std::stoull(argv[3]);require(pid64 && pid64<=MAXDWORD && creation,"Explicit process identity");DWORD pid=(DWORD)pid64;
        process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);require(process,"Read engine identity");
        require(native_mat_guard::created(process)==creation && native_mat_guard::system_engine(process),"Pinned audiodg identity / audioeng SHA256");
        printf("PREFLIGHT identity=PASS pid=%lu created=%llu nativeMATReady=unknown attach=%d\n",pid,creation,attach);
        if(!attach){CloseHandle(process);return 0;}
        fs::path tap=fs::canonical(argv[4]);std::wstring expected=argv[5];std::string expected_hash(expected.begin(),expected.end());
        require(tap.is_absolute() && expected_hash.size()==64 && native_mat_guard::sha256(tap)==expected_hash,"Pinned tap absolute path/hash");
        std::wstring name=argv[6];require(name.starts_with(L"\\\\.\\pipe\\SunshineMatLive-") && name.find_first_of(L"\r\n")==std::wstring::npos,"Local live pipe name");
        std::wstring event_name=argv[7];require(event_name.starts_with(L"Local\\SunshineMatLiveStop-"),"Explicit local stop event");
        // Hold the file open without write/delete sharing through remote loading, avoiding a file replacement race.
        HANDLE tap_lock=CreateFileW(tap.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(tap_lock!=INVALID_HANDLE_VALUE,"Lock tap file");
        struct Unlock{HANDLE h;~Unlock(){CloseHandle(h);}} unlock{tap_lock};
        require(native_mat_guard::sha256(tap)==expected_hash,"Tap changed before load");
        CloseHandle(process);process=nullptr;
        process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid);
        require(process,"Open exact engine for explicit attach (no elevation retry)");
        require(native_mat_guard::created(process)==creation && native_mat_guard::system_engine(process),"Engine identity changed");
        auto base=Module(pid,L"audioeng.dll");BYTE prologue[8]{};SIZE_T got=0;const BYTE expected_bytes[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x49,0x08};
        require(base && ReadProcessMemory(process,(void*)(base+0x31e00),prologue,8,&got) && got==8 && !memcmp(prologue,expected_bytes,8),"Original pinned encoder prologue");
        auto loaded=LoadedTap(pid,tap);
        require(reuse?loaded!=0:loaded==0,reuse?"Explicit reattach requires the exact loaded tap":"Tap already loaded; explicit --reattach required");
        auto acl=PipeAcl();SECURITY_ATTRIBUTES sa{sizeof(sa),acl,FALSE};stop_event=CreateEventW(&sa,TRUE,FALSE,event_name.c_str());DWORD event_error=GetLastError();LocalFree(acl);
        require(stop_event && event_error!=ERROR_ALREADY_EXISTS,"Fresh explicit stop event");
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(EncodedRing),nullptr);require(mapping,"Encoded ring");
        ring=(EncodedRing*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(EncodedRing));require(ring,"Map encoded ring");memset(ring,0,sizeof(*ring));ring->magic=kByteMagic;ring->version=1;
        InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        if(!reuse)loaded=LoadTap(process,pid,tap);
        require(LoadedTap(pid,tap)==loaded,"Loaded tap identity changed");
        auto local=LoadLibraryExW(tap.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local,"Read pinned tap exports");
        auto start=GetProcAddress(local,"StartCapture");require(start,"Tap StartCapture export");auto entry=loaded+(uintptr_t)start-(uintptr_t)local;FreeLibrary(local);
        require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remote_mapping,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate ring");
        require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&remote_collector,SYNCHRONIZE,FALSE,0)!=FALSE,"Duplicate collector watchdog");
        StartArgs args{remote_mapping,remote_collector};arguments=RemoteCopy(process,&args,sizeof(args));
        thread=RemoteThread(process,entry,arguments);handed=true;auto began=GetTickCount64();
        while(!ring->status && GetTickCount64()-began<2000 && WaitForSingleObject(thread,10)==WAIT_TIMEOUT)InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        require(ring->status==1,"Native MAT hook READY");
        mat_capture::Descriptor descriptor;descriptor.kind=mat_capture::native_source;descriptor.publisher_pid=GetCurrentProcessId();descriptor.publisher_created=native_mat_guard::created(GetCurrentProcess());
        descriptor.engine_pid=pid;descriptor.engine_created=creation;descriptor.epoch=(Qpc()&INT64_MAX)|1;LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);descriptor.qpc_hz=hz.QuadPart;
        collector=std::thread([&]{
            mat_capture::Assembler assembler;
            uint64_t stop_requested=0;
            try {
                while(true) {
                    auto now=GetTickCount64();InterlockedExchange64(&ring->heartbeat,now);
                    require(!Read64(&ring->dropped) && !Read64(&ring->invalid) && !ring->error,"Native ring loss/failure; no concealment");
                    require(WaitForSingleObject(process,0)==WAIT_TIMEOUT,"Engine exited; no automatic reattach");
                    auto read=Read64(&ring->read),write=Read64(&ring->write);require(write>=read && write-read<=kByteSlots,"Native ring bounds");
                    while(read<write) {
                        auto& slot=ring->slots[read%kByteSlots];assembler.push(slot.record,slot.data,[&](const auto& bytes,uint64_t qpc){
                            std::lock_guard lock(shared.mutex);bool first=!shared.ready;shared.ready=true;shared.encoder=assembler.encoder();shared.last_cycle=GetTickCount64();++shared.cycles;shared.skipped=assembler.startup_skipped();
                            if(first)printf("NATIVE_MAT_READY carrierBytes=61440 logicalRate=48000 durationFrames=960 startupSkippedRecords=%llu\n",shared.skipped);
                            if(shared.playing && qpc>=shared.play_qpc){require(shared.queue.size()<4,"Live consumer overflow; no drop/replay");shared.queue.push_back({bytes,qpc});shared.changed.notify_all();}
                        });
                        {std::lock_guard lock(shared.mutex);shared.skipped=assembler.startup_skipped();shared.observations=assembler.observations();}
                        InterlockedExchange64(&ring->read,++read);
                    }
                    require(!Read64(&ring->failures),"Native Process HRESULT failed; no concealment");
                    if(interrupted || WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0){
                        if(!stop_requested)stop_requested=now;
                        // An operator stop closes at a whole carrier boundary.
                        // Never pad/truncate a half-cycle or forge a clean EOS.
                        if(!assembler.partial())InterlockedExchange(&ring->stop,1);
                        else require(now-stop_requested<2000,"Native stop stalled mid-cycle; no EOS");
                    }
                    if(WaitForSingleObject(thread,0)==WAIT_OBJECT_0) {
                        if(Read64(&ring->read)!=Read64(&ring->write))continue;
                        require(ring->status==2 && !ring->error && (interrupted||WaitForSingleObject(stop_event,0)==WAIT_OBJECT_0) && !assembler.partial(),"Unclean/truncated native stop; no EOS");
                        std::lock_guard lock(shared.mutex);shared.ended=true;shared.changed.notify_all();break;
                    }
                    // Collection may have just advanced last_cycle past the
                    // loop's earlier `now`. Sample after draining to avoid an
                    // unsigned underflow that falsely rejects fresh output.
                    {std::lock_guard lock(shared.mutex);if(shared.ready)require(GetTickCount64()-shared.last_cycle<2000,"Native MAT stalled");}
                    Sleep(1);
                }
            }catch(const std::exception& e){
                {std::lock_guard lock(shared.mutex);shared.skipped=assembler.startup_skipped();shared.observations=assembler.observations();}
                Broken(shared,e.what());InterlockedExchange(&ring->stop,1);
            }
        });
        printf("ATTACHED publisher=%lu engine=%lu source=native-Process-output no-file-input=1\n",GetCurrentProcessId(),pid);
        Serve(shared,name,descriptor);InterlockedExchange(&ring->stop,1);collector.join();
        require(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0,"Native hook stop timeout");
        require(ReadProcessMemory(process,(void*)(base+0x31e00),prologue,8,&got) && got==8 && !memcmp(prologue,expected_bytes,8),"Original encoder prologue restored");
        const auto& o=shared.observations;
        printf("OBSERVED calls=%lld records=%llu bytes=%lld empty=%llu wrongResult=%llu wrongFrames=%llu wrongBytes=%llu nonMat=%llu acceptedHalves=%llu dropped=%lld invalid=%lld failures=%lld lastFrames=%u lastCapacity=%u lastBytes=%u lastHR=0x%08X\n",
            Read64(&ring->calls),o.records,Read64(&ring->bytes),o.empty,o.wrong_result,o.wrong_frames,o.wrong_bytes,o.non_mat,o.accepted_halves,
            Read64(&ring->dropped),Read64(&ring->invalid),Read64(&ring->failures),o.last_frames,o.last_capacity,o.last_bytes,(unsigned)o.last_result);
        printf("SOURCE_DIAGNOSIS %s\n",shared.cycles?"native-MAT-observed":Read64(&ring->calls)==0?"pinned-wrapper-Process-not-invoked":
            o.empty==o.records && o.records?"encoder-output-empty":"encoder-output-not-usable-MAT; inspect-OBSERVED");
        printf("STOP cycles=%llu startupSkippedRecords=%llu failed=%d error=%s restored=1\n",shared.cycles,shared.skipped,shared.failed,shared.error.c_str());result=shared.failed?1:0;
    }catch(const std::exception& e){printf("FAILED %s win32=%lu\n",e.what(),GetLastError());if(ring)InterlockedExchange(&ring->stop,1);InterlockedExchange(&interrupted,1);if(collector.joinable())collector.join();}
    bool done=!thread || WaitForSingleObject(thread,10000)==WAIT_OBJECT_0;
    if(!handed && process){for(HANDLE remote:{remote_mapping,remote_collector})if(remote){HANDLE copy{};if(DuplicateHandle(process,remote,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_CLOSE_SOURCE))CloseHandle(copy);}}
    if(done && arguments)VirtualFreeEx(process,arguments,0,MEM_RELEASE);
    if(done && ring)UnmapViewOfFile(ring); // Keep pages mapped if a timed-out hook might still use them.
    if(thread)CloseHandle(thread);if(mapping)CloseHandle(mapping);if(process)CloseHandle(process);if(stop_event)CloseHandle(stop_event);return result;
}
