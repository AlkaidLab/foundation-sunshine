#include "src/spatial_transport/mat_session_source.h"
#include "src/thread_safe.h"
#include "third-party/moonlight-common-c/src/MatDumpFile.h"
#include <iostream>
#include <fstream>
using spatial_audio::require;
int main(int argc,char** argv) {
    try {
        require(argc==3,"unit source.raw NEW_OUT");std::filesystem::path out=argv[2];
        require(std::filesystem::create_directory(out),"new unit output");
        auto m=spatial_transport::inspect_replay(argv[1]);require(m && m->profile.cycles==178,"frozen manifest count");
        require(std::string(m->profile.sha256)=="79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6","frozen manifest SHA");
        require(!spatial_transport::inspect_replay(""),"disabled default");
        auto sdp=spatial_transport::describe(*m);LI_MAT_DUMP_PROFILE profile{};
        require(MatProfileParse(&profile,sdp.c_str()),"DESCRIBE parse");
        LI_MAT_DUMP_PROFILE playback{};
        require(MatPlaybackProfileParse(&playback,sdp.c_str()),"explicit playback DESCRIBE parse");
        require(spatial_transport::accept_playback(m.get(),"1",playback.sha256,"178",MAT_PLAYBACK_CARRIER,MAT_PLAYBACK_CLOCK),"playback host policy");
        require(!spatial_transport::accept_playback(m.get(),"1",playback.sha256,"178","pcm",MAT_PLAYBACK_CLOCK),"playback carrier mismatch");
        require(!spatial_transport::accept_playback(m.get(),"1",playback.sha256,"178",MAT_PLAYBACK_CARRIER,"192000"),"logical clock mismatch");
        require(!MatPlaybackProfileParse(&playback,"a=x-ss-audio.mat-dump:1\r\n"),"dump capability does not imply playback");
        require(!MatPlaybackProfileParse(&playback,(sdp+"a=x-ss-audio.mat-playback:1\r\n").c_str()),"duplicate playback capability");
        require(MatPlaybackProfileParse(&playback,sdp.c_str()),"restore playback profile");
        require(MatPlaybackProfileConfirm(&playback,MAT_PLAYBACK_NAME,playback.sha256,"178",MAT_PLAYBACK_CARRIER,MAT_PLAYBACK_CLOCK),"playback explicit ACK");
        require(!MatPlaybackProfileConfirm(&playback,MAT_DUMP_NAME,playback.sha256,"178",MAT_PLAYBACK_CARRIER,MAT_PLAYBACK_CLOCK),"no dump ACK for playback");
        require(!MatPlaybackProfileConfirm(&playback,"opus",playback.sha256,"178",MAT_PLAYBACK_CARRIER,MAT_PLAYBACK_CLOCK),"no Opus fallback");
        require(!MatPlaybackProfileConfirm(&playback,MAT_PLAYBACK_NAME,playback.sha256,"178",nullptr,MAT_PLAYBACK_CLOCK),"missing carrier ACK");
        require(!MatPlaybackProfileConfirm(&playback,MAT_PLAYBACK_NAME,playback.sha256,"177",MAT_PLAYBACK_CARRIER,MAT_PLAYBACK_CLOCK),"wrong count ACK");
        require(spatial_transport::accept(m.get(),"1",profile.sha256,"178"),"host ANNOUNCE policy");
        require(!spatial_transport::accept(nullptr,"1",profile.sha256,"178"),"unconfigured host reject");
        require(!spatial_transport::accept(m.get(),"2",profile.sha256,"178"),"version reject");
        require(!spatial_transport::accept(m.get(),"1",profile.sha256,"177"),"cycle reject");
        require(!MatProfileParse(&profile,""),"legacy DESCRIBE reject");
        require(!MatProfileParse(&profile,(sdp+"a=x-ss-audio.mat-dump:1\r\n").c_str()),"duplicate reject");
        auto bad=sdp;bad.replace(bad.find("dump:1")+5,1,"2");require(!MatProfileParse(&profile,bad.c_str()),"unsupported SDP version");
        require(MatProfileParse(&profile,sdp.c_str()),"restore manifest");
        require(MatProfileConfirm(&profile,MAT_DUMP_NAME,profile.sha256,"178"),"explicit response ACK");
        require(!MatProfileConfirm(&profile,nullptr,profile.sha256,"178"),"missing response ACK");
        require(!MatProfileConfirm(&profile,"opus",profile.sha256,"178"),"no implicit fallback");
        require(!MatProfileConfirm(&profile,MAT_DUMP_NAME,profile.sha256,"179"),"wrong response count");
        LI_MAT_DUMP_PROFILE v{};require(!MatProfileSet(&v,profile.sha256,"1001"),"sequence limit");
        require(!MatProfileSet(&v,profile.sha256,"-1"),"signed count reject");
        require(!MatProfileSet(&v,profile.sha256,"178x"),"trailing count reject");
        safe::queue_t<int> q(2);require(q.try_raise(1)&&q.try_raise(2)&&!q.try_raise(3),"bounded queue reject without clearing");
        require(*q.pop()==1 && *q.pop()==2,"bounded queue preserves FIFO");q.stop();require(!q.try_raise(4),"stopped queue reject");
        // A short carrier-valid recording for fast writer/lifecycle failures.
        spatial_audio::MatReplaySource source(argv[1],1,1);require(source.start(),"fixture open");
        spatial_audio::Frame frame;require(source.read(frame)==spatial_audio::Read::frame,"fixture frame");
        auto tiny=out/"fixture.raw";{std::ofstream f(tiny,std::ios::binary);f.write((char*)frame.bytes.data(),frame.bytes.size());require(bool(f),"tiny write");}
        auto small=spatial_transport::inspect_replay(tiny.string());
        for(const auto& mode: {"success","missing","corrupt","wronghash","wrongcount","disconnect"}) {
            auto path=out/(std::string(mode)+".raw");auto expected=small->profile;
            if(std::string(mode)=="wronghash")expected.sha256[0]=expected.sha256[0]=='a'?'b':'a';
            if(std::string(mode)=="wrongcount")expected.cycles=2;
            auto dump=MatDumpOpen(path.string().c_str(),&expected);require(dump,"dump create");
            unsigned index=0;int result=0;
            spatial_transport::fragment(frame,[&](const uint8_t* p,size_t n){
                ++index;if(std::string(mode)=="missing" && index==20)return;
                if(std::string(mode)=="disconnect" && index>20)return;
                std::vector<uint8_t> packet(p,p+n);if(std::string(mode)=="corrupt" && index==20)packet[100]^=1;
                result=MatDumpPush(dump,packet.data(),packet.size());
            });
            uint8_t end[MAT_WIRE_PACKET];mat_wire_make(end,MAT_WIRE_END,1,1,1,0,mat_hash(frame.bytes.data(),frame.bytes.size()),nullptr);
            if(std::string(mode)!="disconnect")result=MatDumpPush(dump,end,sizeof(end));
            bool good=std::string(mode)=="success";require(bool(MatDumpPublished(dump))==good,"publish is fail-closed");
            MatDumpClose(dump);require(std::filesystem::exists(path)==good,"only successful final file");
            require(std::filesystem::exists(path.string()+".partial")==!good,"partial retained on failure");
            if(good)require(!MatDumpOpen(path.string().c_str(),&expected),"existing output never overwritten");
            (void)result;
        }
        // Mutation after DESCRIBE must withhold END (even if MAT header is valid).
        auto before=spatial_transport::inspect_replay(tiny.string());
        {std::fstream f(tiny,std::ios::in|std::ios::out|std::ios::binary);f.seekp(100);f.put(char(frame.bytes[100]^1));}
        bool failed=false;unsigned ends=0;
        try{spatial_transport::replay_session(*before,[&](const uint8_t* p,size_t){if(p[5]==MAT_WIRE_END)++ends;});}catch(...){failed=true;}
        require(failed && !ends,"mutated recording cannot send END");
        std::cout<<"PASS manifest/SDP/confirmation/bounds, queue FIFO, 6 dump lifecycle cases, source-mutation no-END\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
