#define SPATIAL_COLLECTOR_HELPERS_ONLY
#include "collector.cpp"
#include "encoded_protocol.h"
static bool EncoderOriginal(HANDLE p,DWORD pid) {
    const BYTE expected[]={0x48,0x83,0xec,0x38,0x48,0x8b,0x49,0x08};BYTE actual[8]{};SIZE_T got{};
    auto base=Module(pid,L"audioeng.dll");
    return base && ReadProcessMemory(p,reinterpret_cast<void*>(base+0x31e00),actual,8,&got) && got==8 && !memcmp(actual,expected,8);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) {puts("encoded_probe NEW_DIRECTORY SECONDS_1_TO_30");return 2;}
    fs::path out=argv[1];int seconds=_wtoi(argv[2]);if(seconds<1 || seconds>30) return 2;
    SetConsoleCtrlHandler(Ctrl,TRUE);setvbuf(stdout,nullptr,_IONBF,0);
    HANDLE process{},mapping{},thread{};EncodedRing* r{};void* remoteArgs{};FILE* binary{};int result=1;
    try {
        Require(!fs::exists(out) && fs::create_directories(out),"NEW encoded output directory");
        DWORD pid=EnginePid();Require(pid!=0,"Exactly one audiodg");
        process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_CREATE_THREAD|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,pid);
        Require(process!=nullptr,"Open audiodg elevated");Require(EncoderOriginal(process,pid),"Encoder process original bytes");
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(EncodedRing),nullptr);Require(mapping!=nullptr,"Byte ring");
        r=static_cast<EncodedRing*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(EncodedRing)));Require(r!=nullptr,"Map byte ring");
        memset(r,0,sizeof(*r));r->magic=kByteMagic;r->version=1;InterlockedExchange64(&r->heartbeat,GetTickCount64());
        HANDLE remoteMapping{},remoteCollector{};
        Require(DuplicateHandle(GetCurrentProcess(),mapping,process,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"Duplicate ring");
        Require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process,&remoteCollector,SYNCHRONIZE,FALSE,0)!=FALSE,"Watchdog process");
        fs::path dll=L"C:\\ProgramData\\SpatialObjectSpike\\engine_tap_encoded_v1.dll";
        auto loaded=LoadTap(process,pid,dll);auto local=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);Require(local!=nullptr,"Read tap exports");
        auto start=GetProcAddress(local,"StartCapture");Require(start!=nullptr,"Tap export");
        auto entry=loaded+reinterpret_cast<uintptr_t>(start)-reinterpret_cast<uintptr_t>(local);FreeLibrary(local);
        StartArgs args{remoteMapping,remoteCollector};remoteArgs=RemoteCopy(process,&args,sizeof(args));
        _wfopen_s(&binary,(out/L"encoder-output.bin").c_str(),L"wb");Require(binary!=nullptr,"Encoded file");setvbuf(binary,nullptr,_IOFBF,1024*1024);
        auto began=GetTickCount64();thread=RemoteThread(process,entry,remoteArgs);
        while(!r->status && GetTickCount64()-began<10000 && WaitForSingleObject(thread,10)==WAIT_TIMEOUT) {}
        printf("READY status=%ld error=%ld collector=%lu engine=%lu ring=%zu\n",r->status,r->error,GetCurrentProcessId(),pid,sizeof(*r));Require(r->status==1,"Encoded READY");
        for(;;) {
            auto now=GetTickCount64();InterlockedExchange64(&r->heartbeat,now);
            if(interrupted || now-began>=static_cast<uint64_t>(seconds)*1000 || Read64(&r->bytes)>256*1024*1024 || fs::exists(out/L"STOP")) InterlockedExchange(&r->stop,1);
            auto read=Read64(&r->read),write=Read64(&r->write);
            while(read<write) {
                auto& s=r->slots[read%kByteSlots];Require(s.record.magic==0x42595445 && s.record.bytes<=kByteCapacity && s.record.sequence==static_cast<uint64_t>(read),"Byte ring integrity");
                Require(fwrite(&s.record,sizeof(s.record),1,binary)==1 && fwrite(s.data,1,s.record.bytes,binary)==s.record.bytes,"Byte file write");
                InterlockedExchange64(&r->read,++read);
            }
            if(WaitForSingleObject(thread,0)!=WAIT_TIMEOUT && Read64(&r->read)==Read64(&r->write)) break;
            Require(WaitForSingleObject(process,0)==WAIT_TIMEOUT,"Audio engine still alive");Sleep(5);
        }
        Require(fflush(binary)==0,"Final encoded flush");bool restored=EncoderOriginal(process,pid);LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);
        FILE* report{};_wfopen_s(&report,(out/L"summary.json").c_str(),L"wb");Require(report!=nullptr,"Encoded summary");
        fprintf(report,"{\"engine_pid\":%lu,\"collector_pid\":%lu,\"seconds\":%.3f,\"calls\":%lld,\"records\":%lld,\"bytes\":%lld,\"dropped\":%lld,\"invalid\":%lld,\"failures\":%lld,\"status\":%ld,\"error\":%ld,\"original_bytes_restored\":%s,\"qpc_frequency\":%lld}\n",pid,GetCurrentProcessId(),(GetTickCount64()-began)/1000.,Read64(&r->calls),Read64(&r->write),Read64(&r->bytes),Read64(&r->dropped),Read64(&r->invalid),Read64(&r->failures),r->status,r->error,restored?"true":"false",hz.QuadPart);fclose(report);
        printf("STOP records=%lld bytes=%lld dropped=%lld invalid=%lld failures=%lld status=%ld error=%ld restored=%d\n",Read64(&r->write),Read64(&r->bytes),Read64(&r->dropped),Read64(&r->invalid),Read64(&r->failures),r->status,r->error,restored);
        result=r->status==2 && !r->error && !r->dropped && !r->invalid && !r->failures && restored?0:1;
    } catch(const std::exception& e){printf("FAILED %s\n",e.what());}
    if(r) InterlockedExchange(&r->stop,1);
    bool done=!thread || WaitForSingleObject(thread,10000)==WAIT_OBJECT_0;
    if(done && remoteArgs) VirtualFreeEx(process,remoteArgs,0,MEM_RELEASE);
    if(thread) CloseHandle(thread);if(binary) fclose(binary);if(r) UnmapViewOfFile(r);if(mapping) CloseHandle(mapping);if(process) CloseHandle(process);
    return result;
}
