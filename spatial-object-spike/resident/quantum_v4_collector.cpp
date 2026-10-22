#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "collector.cpp"
#include "quantum_v4_protocol.h"
#include <nlohmann/json.hpp>
using Json4=nlohmann::json;
static void json4(const fs::path& path,const Json4& value){FILE* file=nullptr;_wfopen_s(&file,path.c_str(),L"wb");Require(file!=nullptr,"JSON output");auto text=value.dump(2);bool okay=fwrite(text.data(),1,text.size(),file)==text.size();auto closed=fclose(file);Require(okay&&closed==0,"JSON write");}
int wmain(int argc,wchar_t** argv) {
    fs::path out;int seconds=10;DWORD stopPid=0;
    for(int i=1;i<argc;++i){std::wstring arg=argv[i];if(arg==L"--output"&&i+1<argc)out=argv[++i];else if(arg==L"--seconds"&&i+1<argc)seconds=_wtoi(argv[++i]);else if(arg==L"--stop"&&i+1<argc)stopPid=_wtol(argv[++i]);else return 2;}
    wchar_t eventName[96]{};swprintf_s(eventName,L"Local\\SpatialQ4Stop_%lu",stopPid?stopPid:GetCurrentProcessId());
    if(stopPid){HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName);if(!event)return 1;bool okay=SetEvent(event)!=FALSE;CloseHandle(event);return okay?0:1;}
    if(out.empty()||seconds<1||seconds>10)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);SetConsoleCtrlHandler(Ctrl,TRUE);
    HANDLE process=nullptr,mapping=nullptr,thread=nullptr,stopEvent=nullptr;q4::Ring* ring=nullptr;void* remoteArgs=nullptr;FILE* binary=nullptr;
    DWORD engine=0;bool created=false;int exit=1;uint64_t bytes=0,chunkBytes=0;unsigned chunks=0;
    try {
        Require(!fs::exists(out)&&fs::create_directories(out),"Create NEW v4 output directory");created=true;
        HANDLE token=nullptr;
        if(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)){
            TOKEN_PRIVILEGES privilege{};privilege.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,L"SeDebugPrivilege",&privilege.Privileges[0].Luid);privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token,FALSE,&privilege,0,nullptr,nullptr);CloseHandle(token);
        }
        engine=EnginePid();Require(engine!=0,"Expected one audiodg; no automatic restart");
        process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,engine);
        Require(process!=nullptr,"Open audiodg requires a separately approved elevation");Require(OriginalBytes(process,engine),"Original renderer prologue");
        stopEvent=CreateEventW(nullptr,TRUE,FALSE,eventName);Require(stopEvent!=nullptr,"Stop event");
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(q4::Ring),nullptr);Require(mapping!=nullptr,"Create v4 mapping");
        ring=static_cast<q4::Ring*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(q4::Ring)));Require(ring!=nullptr,"Map v4 ring");memset(ring,0,sizeof(q4::Ring));
        ring->magic=kMagic;ring->version=q4::version;ring->collectorPid=GetCurrentProcessId();LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);ring->qpcFrequency=hz.QuadPart;
        InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        wchar_t programData[MAX_PATH]{};GetEnvironmentVariableW(L"ProgramData",programData,MAX_PATH);
        fs::path dll=fs::path(programData)/L"SpatialObjectSpike"/L"engine_tap_quantum_v4.dll";
        Require(fs::exists(dll),"v4 DLL not deployed; collector never deploys or elevates itself");
        auto remoteDll=LoadTap(process,engine,dll);HMODULE local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);Require(local!=nullptr,"Local export map");
        auto start=GetProcAddress(local,"StartCapture");Require(start!=nullptr,"v4 export");auto entry=remoteDll+reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);
        HANDLE remoteMapping=nullptr,remoteCollector=nullptr;
        Require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate mapping");
        Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&remoteCollector,SYNCHRONIZE,FALSE,0)!=FALSE,"Duplicate watchdog handle");
        StartArgs args{remoteMapping,remoteCollector};remoteArgs=RemoteCopy(process,&args,sizeof(args));
        json4(out/L"format.json",{{"version",4},{"header_bytes",160},{"source_kind","native-pre-submit-paired"},{"pcm_stage","before-original-submit"},
            {"native_ungained_source_verified",false},{"sample_rate_verified",false},{"domain_identity","capture-session + monotonic observed domain; renderer/encoder/implementation addresses"},
            {"warning","Never infer a shared sample clock across domains. Pre-call PCM is not yet proven to equal ungained source on a native device."}});
        auto began=GetTickCount64();thread=RemoteThread(process,entry,remoteArgs);
        while(!ring->status&&GetTickCount64()-began<5000&&WaitForSingleObject(thread,10)==WAIT_TIMEOUT)InterlockedExchange64(&ring->heartbeat,GetTickCount64());
        Require(ring->status==1,"v4 hooks READY");json4(out/L"ready.json",{{"collector_pid",GetCurrentProcessId()},{"audiodg_pid",engine}});puts("READY v4 paired pre-submit capture");
        for(;;) {
            auto time=GetTickCount64();InterlockedExchange64(&ring->heartbeat,time);
            if(interrupted||WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0||time-began>=static_cast<uint64_t>(seconds)*1000||bytes>=64ull*1024*1024)InterlockedExchange(&ring->stop,1);
            auto read=Read64(&ring->read),write=Read64(&ring->write);
            while(read<write) {
                const auto& slot=ring->slots[read%kSlots];const auto& h=slot.header;
                Require(h.version==4&&h.audio.magic==q4::magic&&h.audio.sequence==uint64_t(read)&&h.audio.frames<=kSamples,"v4 ring identity/length");
                if(!binary||chunkBytes>=1024*1024){if(binary){Require(fclose(binary)==0,"Flush v4 chunk");binary=nullptr;}wchar_t file[64]{};if(!chunks)wcscpy_s(file,L"global-objects.bin");else swprintf_s(file,L"global-objects-%04u.bin",chunks);++chunks;_wfopen_s(&binary,(out/file).c_str(),L"wb");Require(binary!=nullptr,"New v4 chunk");setvbuf(binary,nullptr,_IOFBF,1024*1024);chunkBytes=0;}
                Require(fwrite(&h,sizeof(h),1,binary)==1&&fwrite(slot.pcm,4,h.audio.frames,binary)==h.audio.frames,"Write v4 event");
                auto size=sizeof(h)+h.audio.frames*4;bytes+=size;chunkBytes+=size;InterlockedExchange64(&ring->read,++read);
            }
            if(WaitForSingleObject(thread,0)!=WAIT_TIMEOUT&&Read64(&ring->read)==Read64(&ring->write))break;
            Require(WaitForSingleObject(process,0)!=WAIT_OBJECT_0,"audiodg exited; no automatic reattach");Sleep(2);
        }
        if(binary){Require(fclose(binary)==0,"Flush final v4 file");binary=nullptr;}
        bool restored=OriginalBytes(process,engine);
        Json4 report={{"version",4},{"status",static_cast<LONG>(ring->status)},{"error",static_cast<LONG>(ring->error)},{"original_bytes_restored",restored},{"audiodg_pid",engine},{"collector_pid",GetCurrentProcessId()},
            {"elapsed_seconds",(GetTickCount64()-began)/1000.},{"qpc_frequency",hz.QuadPart},{"blocks",Read64(&ring->write)},{"frames",Read64(&ring->frames)},{"bytes",bytes},{"chunks",chunks},
            {"dropped_full",Read64(&ring->droppedFull)},{"dropped_busy",Read64(&ring->droppedBusy)},{"invalid",Read64(&ring->invalid)},{"extended_calls",Read64(&ring->extendedCalls)},
            {"render_calls",Read64(&ring->renderCalls)},{"domains_opened",Read64(&ring->domainsOpened)},{"domains_closed",Read64(&ring->domainsClosed)},
            {"submit_results",Read64(&ring->submitResults)},{"process_results",Read64(&ring->processResults)},{"ignored_before_first_render",Read64(&ring->ignoredBeforeFirstRender)},{"unbound_after_arming",Read64(&ring->unboundAfterArming)},
            {"dll_pinned_until_engine_exit",true},{"native_ungained_source_verified",false},{"mat_encoded",false}};
        json4(out/L"summary.json",report);puts(report.dump(2).c_str());
        exit=ring->status==2&&!ring->error&&restored&&!ring->invalid&&!ring->droppedFull&&!ring->droppedBusy&&!ring->unboundAfterArming?0:1;
    }catch(const std::exception& e){printf("FAILED %s\n",e.what());if(created){try{json4(out/L"failure.json",{{"failed",true},{"message",e.what()}});}catch(...){}}}
    if(ring)InterlockedExchange(&ring->stop,1);
    bool finished=!thread||WaitForSingleObject(thread,10000)==WAIT_OBJECT_0;
    if(finished&&remoteArgs)VirtualFreeEx(process,remoteArgs,0,MEM_RELEASE);
    if(thread)CloseHandle(thread);if(binary)fclose(binary);if(ring)UnmapViewOfFile(ring);if(mapping)CloseHandle(mapping);if(process)CloseHandle(process);if(stopEvent)CloseHandle(stopEvent);return exit;
}
