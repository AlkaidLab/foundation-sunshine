#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "../resident/collector.cpp"
#include "audiodg_instance_protocol.h"
static void Init(InstanceState* s,const wchar_t* directory) {
    memset(s,0,sizeof(*s));s->magic=kInstanceMagic;s->version=kInstanceVersion;
    if(directory)wcscpy_s(s->vendorDirectory,directory);
    InterlockedExchange64(&s->heartbeat,GetTickCount64());
    for(auto& c:s->components){c.loadHr=c.factoryHr=E_PENDING;for(int i=0;i<2;++i)c.activateHr[i]=c.encoderQiHr[i]=E_PENDING;}
}
static int Selftest(const wchar_t* path) {
    HMODULE mod=LoadLibraryW(path);Require(mod!=nullptr,"Load own helper for guard selftest");
    auto start=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(mod,"StartMatInstanceProbe"));Require(start!=nullptr,"Selftest export");
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(InstanceState),nullptr);
    Require(mapping!=nullptr,"Selftest mapping");
    auto* state=static_cast<InstanceState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(InstanceState)));Require(state!=nullptr,"Selftest map");
    Init(state,L"C:\\not-a-vendor-directory");HANDLE duplicate{},watchdog{};
    Require(DuplicateHandle(GetCurrentProcess(),mapping,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Selftest duplicate");
    Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&watchdog,SYNCHRONIZE,FALSE,0)!=FALSE,"Selftest watchdog");
    InstanceArgs args{duplicate,watchdog};DWORD code=start(&args);
    bool okay=code==ERROR_BAD_ENVIRONMENT && state->status==-1 && state->error==ERROR_BAD_ENVIRONMENT && !state->guardPassed && !state->eventCount;
    UnmapViewOfFile(state);CloseHandle(mapping);FreeLibrary(mod);
    puts(okay?"PASS own-process guard rejects before loading codecs":"FAIL guard selftest");return okay?0:1;
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==3 && !wcscmp(argv[1],L"--selftest"))return Selftest(argv[2]);
    if(argc!=2 && argc!=4){puts("Usage: audiodg-instance-probe NEW_DIR [DEPLOYED_HELPER_DLL INSTALLED_VENDOR_DIR]");return 2;}
    HANDLE process{},mapping{},thread{};InstanceState* state{};void* remoteArgs{};FILE* report{};int result=1;bool threadDone=true;
    try {
        fs::path out=argv[1];Require(!fs::exists(out) && fs::create_directories(out),"New result directory");
        _wfopen_s(&report,(out/L"summary.json").c_str(),L"wb");Require(report!=nullptr,"Result file");
        DWORD pid=EnginePid();Require(pid!=0,"Exactly one audiodg");
        const DWORD rights=PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE;
        process=OpenProcess(rights,FALSE,pid);DWORD openError=process?0:GetLastError();
        printf("PREFLIGHT engine=%lu required_access=0x%lX OpenProcess error=%lu\n",pid,rights,openError);
        if(!process || argc==2) {
            fprintf(report,"{\"engine_pid\":%lu,\"open_process_error\":%lu,\"remote_code_started\":false,\"stage_a_completed\":false,\"encoder_initialized\":false,\"object_submitted\":false,\"encoded_bytes\":0,\"mat_verified\":false}\n",pid,openError);
            result=process?0:5;
        } else {
            Require(Module(pid,L"audioeng.dll")!=0,"Audio engine module visible");
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(InstanceState),nullptr);Require(mapping!=nullptr,"Shared state");
            state=static_cast<InstanceState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(InstanceState)));Require(state!=nullptr,"Map state");Init(state,argv[3]);
            auto loaded=LoadTap(process,pid,fs::path(argv[2]));
            HMODULE local=LoadLibraryExW(argv[2],nullptr,DONT_RESOLVE_DLL_REFERENCES);Require(local!=nullptr,"Read own helper export");
            auto start=GetProcAddress(local,"StartMatInstanceProbe");Require(start!=nullptr,"Helper entry");
            uintptr_t entry=loaded+reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);
            HANDLE remoteMapping{},remoteCollector{};
            Require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate mapping");
            Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&remoteCollector,SYNCHRONIZE,FALSE,0)!=FALSE,"Duplicate watchdog");
            InstanceArgs args{remoteMapping,remoteCollector};remoteArgs=RemoteCopy(process,&args,sizeof(args));
            thread=RemoteThread(process,entry,remoteArgs);threadDone=false;auto began=GetTickCount64();
            while(GetTickCount64()-began<15000) {
                InterlockedExchange64(&state->heartbeat,GetTickCount64());
                if(WaitForSingleObject(thread,20)==WAIT_OBJECT_0){threadDone=true;break;}
                if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT)break;
            }
            if(!threadDone){InterlockedExchange(&state->stop,1);threadDone=WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;}
            LONG count=InterlockedCompareExchange(&state->eventCount,0,0);if(count>static_cast<LONG>(kInstanceEvents))count=kInstanceEvents;
            FILE* log{};_wfopen_s(&log,(out/L"events.txt").c_str(),L"wb");Require(log!=nullptr,"Event log");
            for(LONG i=0;i<count;++i){auto e=state->events[i];e.label[79]=0;fprintf(log,"%ld component=%u %s hr=0x%08X a=0x%llX b=0x%llX\n",i,e.component,e.label,e.hr,e.a,e.b);}fclose(log);
            fprintf(report,"{\"engine_pid\":%lu,\"open_process_error\":0,\"remote_code_started\":true,\"thread_completed\":%s,\"status\":%ld,\"error\":%ld,\"guard_passed\":%s,\"stage_a_completed\":%s,\"encoder_initialized\":false,\"object_submitted\":false,\"encoded_bytes\":0,\"mat_verified\":false,\"helper_retained_until_host_exit\":true,\"components\":[",pid,threadDone?"true":"false",state->status,state->error,state->guardPassed?"true":"false",threadDone && state->status==2?"true":"false");
            for(unsigned i=0;i<2;++i){auto c=state->components[i];fprintf(report,"%s{\"load_hr\":%ld,\"factory_hr\":%ld,\"activate_hr\":[%ld,%ld],\"encoder_qi_hr\":[%ld,%ld],\"preloaded\":%s,\"distinct_canonical_identities\":%s}",i?",":"",c.loadHr,c.factoryHr,c.activateHr[0],c.activateHr[1],c.encoderQiHr[0],c.encoderQiHr[1],c.preloaded?"true":"false",c.distinctCanonicalIdentities?"true":"false");}
            fputs("]}\n",report);result=threadDone && state->status==2?0:1;
            if(!threadDone)puts("TIMEOUT: no thread termination or audiodg restart; remote args/module retained for safety");
        }
    }catch(const std::exception& e){printf("FAILED %s\n",e.what());}
    if(threadDone && remoteArgs)VirtualFreeEx(process,remoteArgs,0,MEM_RELEASE);
    if(report)fclose(report);if(state)UnmapViewOfFile(state);if(mapping)CloseHandle(mapping);if(thread)CloseHandle(thread);if(process)CloseHandle(process);
    return result;
}
