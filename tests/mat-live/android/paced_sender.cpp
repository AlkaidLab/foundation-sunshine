// Generated software source through the production fragmenter, AEAD and pacer.
// RTP/FEC framing mirrors the broadcast sender; this is not a full RTSP host.
extern "C" {
#include "Limelight-internal.h"
#include <rs.h>
}
#include "../../../src/spatial_transport/mat_live_session.h"
#include <iostream>
using spatial_audio::require;
struct Generated final:spatial_audio::EncodedAudioSource {
    unsigned count=0;
    spatial_audio::Descriptor descriptor()const override{return {};}
    bool start()override{return true;}
    spatial_audio::Read read(spatial_audio::Frame& f)override {
        if(count==250)return spatial_audio::Read::ended;
        f={};f.sequence=count;f.sample_start=count*960;f.bytes.resize(61440);
        for(unsigned j=0;j<f.bytes.size();++j)f.bytes[j]=(uint8_t)(j*7+count);
        memcpy(f.bytes.data(),"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);++count;return spatial_audio::Read::frame;
    }
    void stop()override{} spatial_audio::State state()const override{return spatial_audio::State::running;}
    std::string error()const override{return {};}
};
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    if(initializePlatform())return 2;
    SOCKET sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);sockaddr_in peer{};peer.sin_family=AF_INET;
    peer.sin_addr.s_addr=htonl(INADDR_LOOPBACK);peer.sin_port=htons((unsigned short)std::stoi(argv[1]));
    setNonFatalRecvTimeoutMs(sock,1000);
    if(bind(sock,(sockaddr*)&peer,sizeof(peer)))return 3;
    std::cout<<"READY\n"<<std::flush;char ping[64];SOCKADDR_LEN addrlen=sizeof(peer);
    int pingSize=0;for(unsigned i=0;i<20&&pingSize<4;++i)pingSize=recvfrom(sock,ping,sizeof(ping),0,(sockaddr*)&peer,&addrlen);
    if(pingSize<4)return 4;
    auto* rs=reed_solomon_new(4,2);const uint8_t matrix[]={0x77,0x40,0x38,0x0e,0xc7,0xa7,0x0d,0x6c};memcpy(rs->p,matrix,8);
    uint8_t storage[6][MAT_LIVE_PACKET],*shards[6];for(unsigned i=0;i<6;++i)shards[i]=storage[i];
    unsigned sequence=0,datagrams=0,checksum=0;uint8_t key[16]={};
#ifdef LC_WINDOWS
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);
#endif
    uint64_t last=0,gap=0,window=0;unsigned batch=0,maxbatch=0;
    const auto send=[&](const uint8_t* wire,size_t n) {
        auto now=PltGetMicroseconds();if(last&&now-last>gap)gap=now-last;last=now;
        if(now/1000!=window){window=now/1000;batch=0;}if(++batch>maxbatch)maxbatch=batch;
        require(sendto(sock,(const char*)wire,(int)n,0,(sockaddr*)&peer,sizeof(peer))==(int)n,"sendto failed");
        checksum^=mat_hash(wire,n);++datagrams;
    };
    try {
        spatial_transport::LiveSession s;s.profile={123,7};s.source=std::make_unique<Generated>();
        auto n=spatial_transport::live_session(s,key,sizeof(key),[&](const uint8_t* bytes,size_t length) {
            uint8_t wire[1200]={};auto* r=(RTP_PACKET*)wire;r->header=0x80;r->packetType=97;
            r->sequenceNumber=htons((uint16_t)sequence);r->timestamp=htonl(sequence);memcpy(r+1,bytes,length);
            memcpy(shards[sequence%4],bytes,length);send(wire,sizeof(*r)+length);
            if(sequence%4==3) {
                require(!reed_solomon_encode(rs,shards,6,(int)length),"FEC encode");
                for(unsigned i=0;i<2;++i) {
                    r->packetType=127;r->sequenceNumber=htons((uint16_t)(sequence+i+1));r->timestamp=0;
                    auto* f=(AUDIO_FEC_HEADER*)(r+1);f->fecShardIndex=(uint8_t)i;f->payloadType=97;
                    f->baseSequenceNumber=htons((uint16_t)(sequence-3));f->baseTimestamp=htonl(sequence-3);f->ssrc=0;
                    memcpy(f+1,shards[4+i],length);send(wire,sizeof(*r)+sizeof(*f)+length);
                }
            }
            ++sequence;
        },[&](auto delay) {
#ifdef LC_WINDOWS
            LARGE_INTEGER due;due.QuadPart=-(std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count()+99)/100;
            require(timer&&SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)&&WaitForSingleObject(timer,1000)==WAIT_OBJECT_0,"pacing timer");
#else
            std::this_thread::sleep_for(delay);
#endif
        },[]{return false;});
        std::cout<<"RESULT cycles="<<n<<" data="<<sequence<<" datagrams="<<datagrams<<" checksum="<<checksum<<" maxSendGapUs="<<gap<<" maxDatagramsPerMs="<<maxbatch<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    reed_solomon_release(rs);
#ifdef LC_WINDOWS
    CloseHandle(timer);
#endif
    closeSocket(sock);cleanupPlatform();return 0;
}
