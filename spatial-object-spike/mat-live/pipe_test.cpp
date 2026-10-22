// Own-process byte-pipe test. Generated carrier only; no audiodg or vendor use.
// The test-source descriptor is intentionally rejected by native preparation.
#define MAT_LIVE_PUBLISHER_HELPERS_ONLY
#include "capture.cpp"
#include "../../src/spatial_audio/mat_capture_reader.h"
#include <atomic>
#include <iostream>
static void CheckPipe(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
static void Run(unsigned mode){
    Shared shared;shared.ready=true;shared.encoder=42;shared.last_cycle=GetTickCount64();
    const auto name=L"\\\\.\\pipe\\SunshineMatLive-eos-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(mode);
    mat_capture::Descriptor descriptor{mat_capture::test_source,GetCurrentProcessId(),GetCurrentProcessId(),1,1,7,42,10000000};
    std::string serverError;std::atomic<bool> finished=false;HANDLE client=INVALID_HANDLE_VALUE;
    std::thread server([&]{try{Serve(shared,name,descriptor);}catch(const std::exception& e){serverError=e.what();}finished=true;});
    std::exception_ptr failure;
    try {
        auto until=GetTickCount64()+2000;
        while(client==INVALID_HANDLE_VALUE&&GetTickCount64()<until){
            client=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(client==INVALID_HANDLE_VALUE)Sleep(1);
        }
        CheckPipe(client!=INVALID_HANDLE_VALUE,"Connect own-process pipe");
        char command[8];memcpy(command,"MCAPOPEN",8);CheckPipe(Io(client,command,8,true,2000),"OPEN");
        uint8_t header[mat_capture::descriptor_bytes];CheckPipe(Io(client,header,sizeof(header),false,2000),"Descriptor");
        mat_capture::Descriptor decoded;CheckPipe(!mat_capture::decode_descriptor(header,decoded),"Generated fixture must stay forbidden as native input");
        memcpy(command,"MCAPPLAY",8);CheckPipe(Io(client,command,8,true,2000),"PLAY");
        until=GetTickCount64()+2000;
        for(;;){bool playing=false;{std::lock_guard lock(shared.mutex);playing=shared.playing;}if(playing)break;CheckPipe(GetTickCount64()<until,"PLAY admission");Sleep(1);}
        Cycle cycle{};memcpy(cycle.bytes.data(),"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);cycle.qpc=Qpc();
        {std::lock_guard lock(shared.mutex);shared.queue.push_back(cycle);shared.last_cycle=GetTickCount64();shared.ended=true;shared.changed.notify_all();}
        // Make the producer finish writing before consuming the tail. A write
        // completing into a pipe buffer does not mean the client consumed it.
        Sleep(100);
        uint64_t next=0,last=0;spatial_audio::Frame frame;bool ackAttempted=false,ended=false;
        auto io=[&](void* data,size_t n,bool writing){
            if(writing){ackAttempted=true;if(mode==2)throw std::runtime_error("Simulated acknowledgement cancellation");if(mode==1)mat_put64(static_cast<uint8_t*>(data)+8,999);}
            CheckPipe(Io(client,data,(DWORD)n,writing,2000),"Tail pipe I/O");
        };
        CheckPipe(mat_capture::read_record(io,next,last,7,42,frame)==spatial_audio::Read::frame&&next==1&&frame.epoch==7&&frame.domain==42&&std::equal(frame.bytes.begin(),frame.bytes.end(),cycle.bytes.begin()),"Production reader final cycle");
        try{ended=mat_capture::read_record(io,next,last,7,42,frame)==spatial_audio::Read::ended;}catch(...){if(mode!=2)throw;}
        CheckPipe(ackAttempted&&(ended==(mode!=2)),"Unacknowledged EOS must not become clean end");
        if(mode==2){auto began=GetTickCount64();while(!finished&&GetTickCount64()-began<3000)Sleep(1);CheckPipe(finished,"Missing acknowledgement must be bounded");}
        CloseHandle(client);client=INVALID_HANDLE_VALUE;server.join();CheckPipe(serverError.empty(),"Server exception");
        CheckPipe(shared.acknowledged_leases==(mode==0?1u:0u),"Wrong/missing end acknowledgement accepted");
    }catch(...){failure=std::current_exception();}
    if(client!=INVALID_HANDLE_VALUE)CloseHandle(client);
    if(server.joinable()){Broken(shared,"Test cleanup");server.join();}
    if(failure)std::rethrow_exception(failure);
}
int main(){try{
    for(unsigned mode=0;mode<3;++mode)Run(mode);
    for(unsigned mode=0;mode<3;++mode){uint8_t record[mat_capture::record_bytes];uint64_t next=mode==0?0:1,last=0;spatial_audio::Frame frame;
        mat_capture::encode_record(record,mode==2?mat_capture::error_kind:mat_capture::end_kind,next,1,nullptr,0);if(mode==1)record[28]^=1;
        bool wrote=false,failed=false;try{mat_capture::read_record([&](void* p,size_t n,bool writing){if(writing)wrote=true;else {CheckPipe(n==sizeof(record),"Control read size");memcpy(p,record,n);}},next,last,7,42,frame);}catch(...){failed=true;}
        CheckPipe(failed&&!wrote,"Invalid/empty/error EOF acknowledged");
    }
    std::cout<<"PASS production pipe/reader delayed tail, exact EOS acknowledgement, wrong/missing/cancelled acknowledgement and invalid/empty/error EOF; nativeCapture=0\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL_PIPE_TAIL "<<e.what()<<" nativeCapture=0\n";return 1;}
}
