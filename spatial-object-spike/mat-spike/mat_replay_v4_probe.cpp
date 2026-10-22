#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "../resident/collector.cpp"
#include "mat_replay_v4_input.h"
#include <nlohmann/json.hpp>
using Json=nlohmann::json;
static void SaveJson(const fs::path& path,const Json& value){FILE* f{};_wfopen_s(&f,path.c_str(),L"wb");Require(f!=nullptr,"JSON output");auto text=value.dump(2);bool okay=fwrite(text.data(),1,text.size(),f)==text.size();int close=fclose(f);Require(okay&&close==0,"JSON write");}
static void Initialize(ReplayState* s){memset(s,0,sizeof(*s));s->magic=kReplayMagic;s->version=kReplayVersion;InterlockedExchange64(&s->heartbeat,GetTickCount64());for(auto& c:s->cases)c.createHr=c.mapperHr=c.initializeHr=c.submitHr=c.processHr=E_PENDING;}
static void LoadInput(ReplayState* s,const wchar_t* path){
    FILE* f{};_wfopen_s(&f,path,L"rb");Require(f!=nullptr,"Open MATV4P1 input");
    try{char magic[8];uint32_t head[4];Require(fread(magic,1,8,f)==8&&!memcmp(magic,"MATV4P1",8)&&fread(head,4,4,f)==4&&head[0]==1&&head[1]==2&&head[2]==400&&head[3]==480,"Unique MATV4P1 dimensions (not MATLIF1)");
        for(auto& c:s->cases){Require(fread(&c.domain,8,1,f)==1,"Case domain");uint32_t h[6];Require(fread(h,4,6,f)==6,"Case header");c.blocks=h[0];c.sourceBlocks=h[1];c.prerollBlocks=h[2];c.drainBlocks=h[3];c.paceMs=h[4];c.prerollBefore=h[5];Require(c.blocks<=400,"Case bound");
            for(uint32_t b=0;b<c.blocks;++b){auto& v=c.input[b];Require(fread(&v.phase,4,3,f)==3&&v.count<=20,"Compact block header");Require(fread(v.objects,sizeof(LifeObject),v.count,f)==v.count,"Compact objects");}
        }Require(fgetc(f)==EOF,"No trailing input");fclose(f);f=nullptr;Require(ReplayInputsValid(s),"Strict bounded replay input validation");
    }catch(...){if(f)fclose(f);throw;}
}
struct FakeContext {const ReplayBlock* block{};uint32_t cursor=0;uint64_t calls=0,samples=0;bool fail=false;};
static HRESULT __fastcall FakeSubmit(void* opaque,const void*,const void* properties,const float* pcm,UINT32 frames,float gain){
    auto& context=*static_cast<FakeContext*>(opaque);if(context.fail)return E_ABORT;
    if(!context.block||context.cursor>=context.block->count)return E_UNEXPECTED;
    const auto& expected=context.block->objects[context.cursor++];const auto& p=*static_cast<const ReplayProperties*>(properties);
    ReplayProperties correct{};correct.id=expected.id;correct.type=1;correct.flags=expected.flags;correct.x=expected.xyz[0];correct.y=expected.xyz[1];correct.z=expected.xyz[2];correct.volume=1;
    if(frames!=480||gain!=1.f||memcmp(&p,&correct,sizeof(p))||memcmp(pcm,expected.pcm,480*4))return E_FAIL;
    ++context.calls;context.samples+=480;return S_OK;
}
static Json OwnTests(ReplayState* s,const wchar_t* dll){
    std::vector<std::string> tests;auto test=[&](const char* name,auto fn){fn();tests.push_back(name);};
    auto rejects=[&]{Require(!ReplayInputsValid(s),"Expected invalid input rejection");};
    test("valid_variable_source_domains_and_padding",[&]{Require(ReplayInputsValid(s)&&s->cases[0].blocks==356&&s->cases[1].blocks==256,"Expected complete capture fixture");});
    auto& c=s->cases[0];auto& warm=c.input[c.prerollBefore];auto& first=c.input[c.prerollBefore+20];
    test("reject_source_frame_count_mismatch",[&]{++c.sourceBlocks;rejects();--c.sourceBlocks;});
    test("reject_object_over_capacity",[&]{auto n=warm.count;warm.count=21;rejects();warm.count=n;});
    test("reject_update_before_warm_create",[&]{warm.objects[0].flags=6;rejects();warm.objects[0].flags=7;});
    test("reject_duplicate_live_id",[&]{auto id=warm.objects[1].id;warm.objects[1].id=warm.objects[0].id;rejects();warm.objects[1].id=id;});
    test("reject_nonzero_warmup_audio",[&]{warm.objects[0].pcm[0]=.1f;rejects();warm.objects[0].pcm[0]=0;});
    test("reject_second_create_instead_of_source_update",[&]{first.objects[0].flags=7;rejects();first.objects[0].flags=6;});
    test("reject_source_index_skip",[&]{++first.sourceIndex;rejects();--first.sourceIndex;});
    test("reject_nonfinite_xyz",[&]{auto x=first.objects[0].xyz[0];uint32_t bits=0x7fc00000;memcpy(&first.objects[0].xyz[0],&bits,4);rejects();first.objects[0].xyz[0]=x;});
    test("reject_drain_as_extra_source",[&]{auto& last=c.input[c.blocks-1];last.phase=2;rejects();last.phase=3;});
    test("reject_wrong_domain",[&]{++s->cases[1].domain;rejects();--s->cases[1].domain;});
    test("reject_oversized_duration",[&]{auto n=c.blocks;c.blocks=401;rejects();c.blocks=n;});
    Require(ReplayInputsValid(s),"Restore fixture after negative tests");FakeContext context;uint64_t blocks=0;
    test("shared_submit_routine_unity_gain_xyz_pcm_all_cases",[&]{for(const auto& item:s->cases)for(uint32_t b=0;b<item.blocks;++b){context.block=&item.input[b];context.cursor=0;Require(SubmitReplayBlock(*context.block,&context,nullptr,FakeSubmit)==S_OK&&context.cursor==context.block->count,"Actual submit adapter contract");++blocks;}Require(blocks==612&&context.calls==863,"Dispatch coverage");});
    test("submit_hresult_failure_propagates_without_retry",[&]{FakeContext bad;bad.block=&warm;bad.fail=true;Require(SubmitReplayBlock(warm,&bad,nullptr,FakeSubmit)==E_ABORT&&bad.calls==0,"HRESULT was swallowed");});
    test("own_helper_guard_rejects_before_vendor_or_endpoint_calls",[&]{
        HMODULE module=LoadLibraryW(dll);Require(module!=nullptr,"Load only own test DLL");
        auto validate=reinterpret_cast<BOOL(WINAPI*)(ReplayState*)>(GetProcAddress(module,"ValidateMatReplayV4Input"));auto start=reinterpret_cast<DWORD(WINAPI*)(void*)>(GetProcAddress(module,"StartMatReplayV4Probe"));Require(validate&&start&&validate(s),"Own helper exports/validator");
        HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(ReplayState),nullptr);Require(mapping!=nullptr,"Guard test own mapping");auto* own=static_cast<ReplayState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(ReplayState)));Require(own!=nullptr,"Own guard view");memcpy(own,s,sizeof(*own));
        HANDLE copy{},watch{};Require(DuplicateHandle(GetCurrentProcess(),mapping,GetCurrentProcess(),&copy,0,FALSE,DUPLICATE_SAME_ACCESS)&&DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&watch,SYNCHRONIZE,FALSE,0),"Own guard handles");ReplayArgs args{copy,watch};auto result=start(&args);
        bool okay=result==ERROR_BAD_ENVIRONMENT&&own->status==-1&&!own->guardPassed&&!own->eventCount;UnmapViewOfFile(own);CloseHandle(mapping);FreeLibrary(module);Require(okay,"Non-audiodg guard failed");
    });
    return {{"passed",true},{"selftests_passed",tests.size()},{"tests",tests},{"fake_process_schedule_blocks",blocks},{"unity_submit_calls",context.calls},{"submitted_pcm_samples_including_zero_control",context.samples},{"vendor_loaded",false},{"native_encoder_called",false},{"process_attached",false}};
}
static void SaveCase(const fs::path& root,const ReplayCase& c,DWORD pid,int64_t hz){
    fs::path out=root/(L"domain-"+std::to_wstring(c.domain));Require(fs::create_directory(out),"New domain output");
    Require(c.records<=400&&c.bytes<=kReplayBytes&&c.processedBlocks<=c.blocks,"Output shared bounds");
    FILE* records{};FILE* raw{};_wfopen_s(&records,(out/L"encoder-output.bin").c_str(),L"wb");_wfopen_s(&raw,(out/L"encoder-payload.raw").c_str(),L"wb");Require(records&&raw,"Output files");uint32_t offset=0;
    for(uint32_t i=0;i<c.records;++i){const auto& r=c.outputRecords[i];Require(r.magic==0x42595445&&r.sequence==i&&r.bytes<=kReplayRequest&&offset<=c.bytes&&r.bytes<=c.bytes-offset,"Output record bounds");Require(fwrite(&r,sizeof(r),1,records)==1&&fwrite(c.output+offset,1,r.bytes,records)==r.bytes&&fwrite(c.output+offset,1,r.bytes,raw)==r.bytes,"Output write");offset+=r.bytes;}
    Require(offset==c.bytes,"Output complete bytes");Require(fclose(records)==0&&fclose(raw)==0,"Close output");
    bool complete=c.initialized&&c.createHr==S_OK&&c.mapperHr==S_OK&&c.initializeHr==S_OK&&c.submitHr==S_OK&&c.processHr==S_OK&&c.processedBlocks==c.blocks;
    SaveJson(out/L"summary.json",{{"capture_kind","independent_mat_encoder_replay_v4"},{"engine_pid",pid},{"status",complete?2:-1},{"error",complete?0:1},{"domain",c.domain},{"initialized",bool(c.initialized)},{"create_hr",c.createHr},{"mapper_hr",c.mapperHr},{"initialize_hr",c.initializeHr},{"submit_hr",c.submitHr},{"process_hr",c.processHr},{"scheduled_blocks",c.blocks},{"native_source_process_blocks",c.sourceBlocks},{"preroll_blocks",c.prerollBlocks},{"drain_blocks",c.drainBlocks},{"processed_blocks",c.processedBlocks},{"submissions",c.submissions},{"records",c.records},{"bytes",c.bytes},{"qpc_frequency",hz},{"worker_case_ms",c.elapsedTicks*1000./hz},{"mat_verified",false},{"endpoint_calls",0},{"hooks_installed",false}});
    FILE* timing{};_wfopen_s(&timing,(out/L"timing.csv").c_str(),L"wb");Require(timing!=nullptr,"Timing output");fprintf(timing,"block,phase,source_index,target_qpc,start_qpc,process_start_qpc,finish_qpc,submissions,active_objects\n");for(uint32_t b=0;b<c.processedBlocks;++b){const auto& t=c.timing[b];fprintf(timing,"%u,%u,%u,%lld,%lld,%lld,%lld,%u,%u\n",b,c.input[b].phase,c.input[b].sourceIndex,t.target,t.start,t.processStart,t.finish,t.submissions,t.active);}fclose(timing);
}
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);HANDLE process{},mapping{},thread{};ReplayState* state{};void* remoteArgs{};bool finished=true;fs::path out;bool created=false;int result=1;
    try{
        bool selftest=argc==5&&!wcscmp(argv[1],L"--selftest"),validate=argc==4&&!wcscmp(argv[1],L"--validate"),capture=argc==6&&!wcscmp(argv[1],L"--capture");
        Require(selftest||validate||capture,"Usage: --selftest OWN_DLL INPUT NEW_OUT | --validate INPUT NEW_OUT | --capture NEW_OUT HELPER VENDOR INPUT");
        const wchar_t* input=selftest?argv[3]:validate?argv[2]:argv[5];out=selftest?argv[4]:validate?argv[3]:argv[2];Require(!fs::exists(out)&&fs::create_directories(out),"NEW output directory required");created=true;
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(ReplayState),nullptr);Require(mapping!=nullptr,"Own replay mapping");state=static_cast<ReplayState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(ReplayState)));Require(state!=nullptr,"Own replay view");Initialize(state);LoadInput(state,input);
        if(selftest||validate){Json report;
            if(selftest)report=OwnTests(state,argv[2]);else{Json cases=Json::array();for(const auto& c:state->cases)cases.push_back({{"domain",c.domain},{"blocks",c.blocks},{"source_blocks",c.sourceBlocks},{"preroll_blocks",c.prerollBlocks},{"drain_blocks",c.drainBlocks}});report={{"passed",true},{"cases",cases},{"native_encoder_called",false},{"process_attached",false}};}
            SaveJson(out/L"proof.json",report);puts(report.dump(2).c_str());result=0;
        }else{
            SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID group{};BOOL admin=FALSE;Require(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&group)!=FALSE,"Admin group");CheckTokenMembership(nullptr,group,&admin);FreeSid(group);Require(admin!=FALSE,"Separately approved one-shot elevation required");
            HANDLE token{};if(OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token)){TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,L"SeDebugPrivilege",&p.Privileges[0].Luid);p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;AdjustTokenPrivileges(token,FALSE,&p,0,nullptr,nullptr);CloseHandle(token);}
            DWORD pid=EnginePid();Require(pid!=0,"Exactly one existing audiodg; no automatic restart");process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid);Require(process!=nullptr,"Open approved audiodg");wcscpy_s(state->vendorDirectory,argv[4]);
            auto loaded=LoadTap(process,pid,argv[3]);HMODULE local=LoadLibraryExW(argv[3],nullptr,DONT_RESOLVE_DLL_REFERENCES);Require(local!=nullptr,"Own helper exports");auto start=GetProcAddress(local,"StartMatReplayV4Probe");Require(start!=nullptr,"Replay export");auto entry=loaded+reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);
            HANDLE remoteMapping{},watch{};Require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS)&&DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&watch,SYNCHRONIZE,FALSE,0),"Remote handles");ReplayArgs args{remoteMapping,watch};remoteArgs=RemoteCopy(process,&args,sizeof(args));thread=RemoteThread(process,entry,remoteArgs);finished=false;auto began=GetTickCount64();
            while(GetTickCount64()-began<15000){InterlockedExchange64(&state->heartbeat,GetTickCount64());if(WaitForSingleObject(thread,20)==WAIT_OBJECT_0){finished=true;break;}if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT)break;}
            if(!finished){InterlockedExchange(&state->stop,1);finished=WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;}
            SaveJson(out/L"summary.json",{{"remote_code_started",true},{"thread_completed",finished},{"status",state->status},{"error",state->error},{"guard_passed",bool(state->guardPassed)},{"engine_pid",pid},{"mat_verified",false},{"helper_pinned_until_host_exit",true},{"endpoint_calls",0},{"hooks_installed",false}});
            if(finished){FILE* log{};_wfopen_s(&log,(out/L"events.txt").c_str(),L"wb");Require(log!=nullptr,"Event log");LONG count=state->eventCount;if(count>64)count=64;for(LONG i=0;i<count;++i){auto e=state->events[i];e.label[79]=0;fprintf(log,"%ld case=%u %s hr=0x%08X\n",i,e.caseIndex,e.label,e.hr);}fclose(log);LARGE_INTEGER hz;QueryPerformanceFrequency(&hz);for(const auto& c:state->cases)SaveCase(out,c,pid,hz.QuadPart);
                result=state->status==2&&!state->error?0:1;for(const auto& c:state->cases)if(c.processedBlocks!=c.blocks||c.processHr!=S_OK)result=1;
            }else puts("TIMEOUT: cooperative stop requested; no forced thread/engine termination; helper retained");
        }
    }catch(const std::exception& e){printf("FAILED %s\n",e.what());if(created){try{SaveJson(out/L"failure.json",{{"failed",true},{"message",e.what()}});}catch(...){}}}
    if(state&&thread&&!finished){InterlockedExchange(&state->stop,1);finished=WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;}
    if(finished&&remoteArgs)VirtualFreeEx(process,remoteArgs,0,MEM_RELEASE);
    if(state)UnmapViewOfFile(state);if(thread)CloseHandle(thread);if(mapping)CloseHandle(mapping);if(process)CloseHandle(process);return result;
}
