#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "../resident/collector.cpp"
#include "mat_lifecycle_protocol.h"
static void InitializeState(LifeState* s){
    memset(s,0,sizeof(*s));s->magic=kLifeMagic;s->version=kLifeVersion;
    InterlockedExchange64(&s->heartbeat,GetTickCount64());
    for(auto& c:s->cases)c.createHr=c.mapperHr=c.initializeHr=c.submitHr=c.processHr=E_PENDING;
}
static void LoadInput(LifeState* s,const wchar_t* path){
    FILE* file{};_wfopen_s(&file,path,L"rb");Require(file!=nullptr,"Open input fixture");
    try{
        char magic[8]{};uint32_t head[4]{};
        Require(fread(magic,1,8,file)==8 && !memcmp(magic,"MATLIF1",8),"Lifecycle input magic");
        Require(fread(head,4,4,file)==4 && head[0]==1 && head[1]==2 && head[2]==240 && head[3]==480,"Input dimensions");
        for(unsigned i=0;i<2;++i){auto& c=s->cases[i];uint32_t header[3]{};
            Require(fread(header,4,3,file)==3 && header[0]==i+1 && header[1]==240 && header[2]==10,"Case header");
            c.kind=header[0];c.blocks=header[1];c.paceMs=header[2];
            Require(fread(c.input,sizeof(LifeBlock),kLifeBlocks,file)==kLifeBlocks,"Complete lifecycle blocks");
        }
        Require(fgetc(file)==EOF,"No trailing input");fclose(file);
    }catch(...){fclose(file);throw;}
}
static int Selftest(const wchar_t* dll,const wchar_t* input){
    HMODULE module=LoadLibraryW(dll);Require(module!=nullptr,"Load own helper");
    auto start=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(module,"StartMatLifecycleProbe"));Require(start!=nullptr,"Guard entry");
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(LifeState)),nullptr);Require(mapping!=nullptr,"Selftest mapping");
    auto* s=static_cast<LifeState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LifeState)));Require(s!=nullptr,"Selftest view");InitializeState(s);LoadInput(s,input);
    auto validate=reinterpret_cast<BOOL(WINAPI*)(LifeState*)>(GetProcAddress(module,"ValidateMatLifecycleInput"));
    Require(validate && validate(s),"Valid bounded lifecycle input");
    s->cases[0].input[0].count=21;Require(!validate(s),"Reject too many submissions");s->cases[0].input[0].count=1;
    s->cases[0].input[0].objects[0].flags=6;Require(!validate(s),"Reject update before create");s->cases[0].input[0].objects[0].flags=7;
    s->cases[0].input[90].objects[1].flags=7;Require(!validate(s),"Reject duplicate live ID");s->cases[0].input[90].objects[1].flags=8;
    Require(validate(s),"Restored valid input");
    HANDLE copy{},watch{};Require(DuplicateHandle(GetCurrentProcess(),mapping,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Selftest duplicate");
    Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&watch,SYNCHRONIZE,FALSE,0)!=FALSE,"Selftest watchdog");
    LifeArgs args{copy,watch};DWORD code=start(&args);bool okay=code==ERROR_BAD_ENVIRONMENT && s->status==-1 && !s->guardPassed && !s->eventCount;
    UnmapViewOfFile(s);CloseHandle(mapping);FreeLibrary(module);puts(okay?"PASS fixture dimensions; own helper rejects non-audiodg before codec calls":"FAIL guard");return okay?0:1;
}
static void SaveCase(const fs::path& root,LifeCase& c,DWORD pid,int64_t hz){
    fs::path out=root/(c.kind==1?L"lifecycle":L"twenty");Require(fs::create_directory(out),"New case output");
    FILE* records{};FILE* raw{};_wfopen_s(&records,(out/L"encoder-output.bin").c_str(),L"wb");_wfopen_s(&raw,(out/L"encoder-payload.raw").c_str(),L"wb");Require(records && raw,"Case output files");
    uint32_t offset=0;Require(c.records<=kLifeRecords && c.bytes<=kLifeBytes,"Shared output bounds");
    for(uint32_t i=0;i<c.records;++i){auto& r=c.outputRecords[i];Require(r.magic==0x42595445 && r.sequence==i && r.bytes<=kLifeRequest && r.bytes<=c.bytes-offset,"Output record bounds");
        Require(fwrite(&r,sizeof(r),1,records)==1 && fwrite(c.output+offset,1,r.bytes,records)==r.bytes && fwrite(c.output+offset,1,r.bytes,raw)==r.bytes,"Case write");offset+=r.bytes;
    }
    Require(offset==c.bytes,"Complete output bytes");fclose(records);fclose(raw);
    FILE* summary{};_wfopen_s(&summary,(out/L"summary.json").c_str(),L"wb");Require(summary!=nullptr,"Case summary");
    fprintf(summary,"{\"capture_kind\":\"independent_mat_encoder\",\"engine_pid\":%lu,\"status\":2,\"error\":0,\"thread_completed\":true,\"hooks_installed\":false,\"endpoint_calls\":0,\"initialization_attempted\":%s,\"initialized\":%s,\"create_hr\":%ld,\"mapper_hr\":%ld,\"initialize_hr\":%ld,\"submit_hr\":%ld,\"process_hr\":%ld,\"source_blocks\":%u,\"source_input_frames\":%u,\"processed_source_blocks\":%u,\"object_submissions\":%u,\"records\":%u,\"bytes\":%u,\"dropped\":0,\"invalid\":0,\"failures\":%u,\"qpc_frequency\":%lld,\"worker_case_ms\":%.6f,\"paced\":true,\"pace_ms\":10,\"source_frames_meaning\":\"full scheduled timeline including silence and drain, not per-object active duration\",\"mat_verified\":false}\n",pid,c.initializationAttempted?"true":"false",c.initialized?"true":"false",c.createHr,c.mapperHr,c.initializeHr,c.submitHr,c.processHr,c.blocks,c.blocks*480,c.processedBlocks,c.submissions,c.records,c.bytes,(c.initialized && c.processHr>=0 && c.processedBlocks==c.blocks)?0u:1u,hz,c.elapsedTicks*1000./hz);
    fclose(summary);
    FILE* timing{};_wfopen_s(&timing,(out/L"timing.csv").c_str(),L"wb");Require(timing!=nullptr,"Timing CSV");
    fprintf(timing,"block,target_qpc,start_qpc,process_start_qpc,finish_qpc,submissions,active_objects\n");
    for(unsigned b=0;b<c.processedBlocks;++b){auto t=c.timing[b];fprintf(timing,"%u,%lld,%lld,%lld,%lld,%u,%u\n",b,t.target,t.start,t.processStart,t.finish,t.submissions,t.active);}fclose(timing);
}
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==4 && !wcscmp(argv[1],L"--selftest"))return Selftest(argv[2],argv[3]);
    if(argc!=2 && argc!=5){puts("Usage: mat-lifecycle-probe NEW_DIR [HELPER_DLL VENDOR_DIRECTORY INPUT_BIN]");return 2;}
    HANDLE process{},mapping{},thread{};LifeState* state{};void* remoteArgs{};FILE* report{};bool done=true;int result=1;
    try{
        fs::path out=argv[1];Require(!fs::exists(out) && fs::create_directories(out),"New output directory");
        _wfopen_s(&report,(out/L"summary.json").c_str(),L"wb");Require(report!=nullptr,"Top summary");
        DWORD pid=EnginePid();Require(pid!=0,"Exactly one audiodg");
        process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid);
        DWORD openError=process?0:GetLastError();printf("PREFLIGHT audiodg=%lu error=%lu\n",pid,openError);
        if(!process || argc==2){fprintf(report,"{\"engine_pid\":%lu,\"open_process_error\":%lu,\"remote_code_started\":false,\"mat_verified\":false}\n",pid,openError);result=process?0:5;}
        else{
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(LifeState)),nullptr);Require(mapping!=nullptr,"Shared mapping");
            state=static_cast<LifeState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(LifeState)));Require(state!=nullptr,"Shared view");InitializeState(state);wcscpy_s(state->vendorDirectory,argv[3]);LoadInput(state,argv[4]);
            auto loaded=LoadTap(process,pid,argv[2]);HMODULE local=LoadLibraryExW(argv[2],nullptr,DONT_RESOLVE_DLL_REFERENCES);Require(local!=nullptr,"Read helper export");
            auto start=GetProcAddress(local,"StartMatLifecycleProbe");Require(start!=nullptr,"Helper export");uintptr_t entry=loaded+reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);
            HANDLE remoteMapping{},remoteCollector{};
            Require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate mapping");
            Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&remoteCollector,SYNCHRONIZE,FALSE,0)!=FALSE,"Duplicate watchdog");
            LifeArgs args{remoteMapping,remoteCollector};remoteArgs=RemoteCopy(process,&args,sizeof(args));thread=RemoteThread(process,entry,remoteArgs);done=false;auto began=GetTickCount64();
            while(GetTickCount64()-began<15000){InterlockedExchange64(&state->heartbeat,GetTickCount64());if(WaitForSingleObject(thread,20)==WAIT_OBJECT_0){done=true;break;}if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT)break;}
            if(!done){InterlockedExchange(&state->stop,1);done=WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;}
            FILE* log{};_wfopen_s(&log,(out/L"events.txt").c_str(),L"wb");Require(log!=nullptr,"Events file");LONG count=InterlockedCompareExchange(&state->eventCount,0,0);if(count>64)count=64;
            for(LONG i=0;i<count;++i){auto e=state->events[i];e.label[79]=0;fprintf(log,"%ld case=%u %s hr=0x%08X\n",i,e.caseIndex,e.label,e.hr);}fclose(log);
            fprintf(report,"{\"engine_pid\":%lu,\"remote_code_started\":true,\"thread_completed\":%s,\"status\":%ld,\"error\":%ld,\"guard_passed\":%s,\"mat_verified\":false,\"endpoint_calls\":0,\"helper_retained_until_host_exit\":true}\n",pid,done?"true":"false",state->status,state->error,state->guardPassed?"true":"false");
            if(done && state->status==2 && !state->error){LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);for(auto& c:state->cases)SaveCase(out,c,pid,hz.QuadPart);result=0;}
            if(!done)puts("TIMEOUT; remote args/helper retained; audiodg/thread will not be forcibly terminated");
        }
    }catch(const std::exception& e){printf("FAILED %s\n",e.what());}
    if(done && remoteArgs)VirtualFreeEx(process,remoteArgs,0,MEM_RELEASE);
    if(report)fclose(report);if(state)UnmapViewOfFile(state);if(mapping)CloseHandle(mapping);if(thread)CloseHandle(thread);if(process)CloseHandle(process);return result;
}
