/* Manual software probe: count at recv(), before the real RTP/FEC/decoder path.
 * No native attachment, audio device, packet recording or loss concealment. */
#include "Limelight-internal.h"
#include "MatLivePlayback.h"
#include "MatWire.h"
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
static unsigned seen[65536], received, duplicates, malformed, checksum;
static uint64_t last_recv,max_recv_gap,max_fec;
static int observed_recv(SOCKET s, char* buffer, int size, bool select);
static int observed_fec(PRTP_AUDIO_QUEUE q,PRTP_PACKET p,uint16_t n);
#define recvUdpSocket observed_recv
#define RtpaAddPacket observed_fec
#include "AudioStream.c"
#undef recvUdpSocket
#undef RtpaAddPacket
static atomic_int failed, completed;
static long long first_failure_rcvbuf_errors=-1;
static unsigned cycles;
static EVP_MD_CTX* digest;
static void message(const char* fmt, ...) { static unsigned logs;if(logs++>20)return;va_list a; va_start(a,fmt); vfprintf(stderr,fmt,a); va_end(a); }
static void failure(int error) {
    (void)error;
    if(atomic_exchange(&failed,1))return;
#ifdef __ANDROID__
    // Sample the first failure, before later watchdog shutdown can overflow an
    // unread socket. The runner subtracts its pre-run kernel counter.
    FILE* file=fopen("/proc/net/snmp","r");char line[1024];
    if(file) {
        while(fgets(line,sizeof(line),file)) {
            unsigned long long in,noports,errors,out,drops;
            if(sscanf(line,"Udp: %llu %llu %llu %llu %llu",&in,&noports,&errors,&out,&drops)==5)
                first_failure_rcvbuf_errors=(long long)drops;
        }
        fclose(file);
    }
#endif
}
static void noop(void) {}
static int init(int c, POPUS_MULTISTREAM_CONFIGURATION o, void* p, int f) { (void)c;(void)o;(void)p;(void)f;return 0; }
static void forbidden(char* p,int n) { (void)p;(void)n;failure(-1); }
static int cycle(void* p,const unsigned char* b,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts) {
    (void)p;if(e!=123||d!=7||i!=cycles||pts!=i*960||n!=61440)return 0;
    ++cycles;return EVP_DigestUpdate(digest,b,n)==1;
}
static int end(void* p,uint64_t count) { (void)p;if(count!=cycles)return 0;atomic_store(&completed,1);return 1; }
static int observed_recv(SOCKET s,char* buffer,int size,bool select) {
    int n=recvUdpSocket(s,buffer,size,select);
    if(n>0) {
        uint64_t now=PltGetMicroseconds();if(last_recv&&now-last_recv>max_recv_gap)max_recv_gap=now-last_recv;last_recv=now;
        PRTP_PACKET r=(PRTP_PACKET)buffer;unsigned id=65536;
        if(n>=(int)sizeof(*r)&&r->packetType==97)id=BE16(r->sequenceNumber)/4*6+BE16(r->sequenceNumber)%4;
        else if(n>=(int)(sizeof(*r)+sizeof(AUDIO_FEC_HEADER))&&r->packetType==127) {
            AUDIO_FEC_HEADER* f=(AUDIO_FEC_HEADER*)(r+1);
            if(f->fecShardIndex<2)id=BE16(f->baseSequenceNumber)/4*6+4+f->fecShardIndex;
        }
        ++received;
        if(id>=65536)++malformed;
        else if(seen[id]++)++duplicates;
        else checksum^=mat_hash((const uint8_t*)buffer,(size_t)n);
    }
    return n;
}
static int observed_fec(PRTP_AUDIO_QUEUE q,PRTP_PACKET p,uint16_t n) {
    uint64_t start=PltGetMicroseconds();int result=RtpaAddPacket(q,p,n);
    uint64_t elapsed=PltGetMicroseconds()-start;if(elapsed>max_fec)max_fec=elapsed;return result;
}
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    int raw=!strcmp(argv[1],"raw"),port=atoi(argv[3]);
    if(!raw&&strcmp(argv[1],"decode"))return 2;
    if(initializePlatform())return 3;
    ListenerCallbacks.logMessage=message;ListenerCallbacks.connectionTerminated=failure;
    AudioCallbacks.init=init;AudioCallbacks.start=noop;AudioCallbacks.stop=noop;AudioCallbacks.cleanup=noop;
    AudioCallbacks.decodeAndPlaySample=forbidden;
    AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=500;AppVersionQuad[3]=-1;
    LiInitializeStreamConfiguration(&StreamConfig);StreamConfig.audioCodec=NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;
    StreamConfig.audioConfiguration=AUDIO_CONFIGURATION_STEREO;MatLiveProfile=(LI_MAT_LIVE_PROFILE){123,7};
    AudioPacketDuration=1;AudioEncryptionEnabled=true;AddrLen=sizeof(struct sockaddr_in);
    struct sockaddr_in* remote=(struct sockaddr_in*)&RemoteAddr;remote->sin_family=AF_INET;
    if(inet_pton(AF_INET,argv[2],&remote->sin_addr)!=1||port<1024||port>65535)return 2;
    AudioPortNumber=(unsigned short)port;
    NormalQualityOpusConfig.sampleRate=48000;NormalQualityOpusConfig.channelCount=2;
    NormalQualityOpusConfig.streams=1;NormalQualityOpusConfig.coupledStreams=1;NormalQualityOpusConfig.mapping[1]=1;
    digest=EVP_MD_CTX_create();EVP_DigestInit_ex(digest,EVP_sha256(),NULL);
    LI_MAT_PLAYBACK_CALLBACKS cb={cycle,end,NULL};LiSetMatPlaybackCallbacks(&cb);
    if(initializeAudioStream()||notifyAudioPortNegotiationComplete())return 4;
    int capacity=0;SOCKADDR_LEN length=sizeof(capacity);getsockopt(rtpSocket,SOL_SOCKET,SO_RCVBUF,(char*)&capacity,&length);
    struct sockaddr_in local;SOCKADDR_LEN local_size=sizeof(local);
    getsockname(rtpSocket,(struct sockaddr*)&local,&local_size);
    printf("READY raw=%d receiveBuffer=%d localPort=%u\n",raw,capacity,ntohs(local.sin_port));fflush(stdout);
    if(!raw) { if(startAudioStream(NULL,0))return 5; }
    else {
        struct sockaddr_in peer=*remote;peer.sin_port=htons(AudioPortNumber);
        setNonFatalRecvTimeoutMs(rtpSocket,20);
        if(sendto(rtpSocket,"PING",4,0,(struct sockaddr*)&peer,sizeof(peer))!=4)return 6;
    }
    uint64_t deadline=PltGetMillis()+7500;
    while(!atomic_load(&failed)&&PltGetMillis()<deadline) {
        if(raw) { char buffer[4096];if(observed_recv(rtpSocket,buffer,sizeof(buffer),false)<0)break; }
        else PltSleepMs(5);
    }
    if(!raw)stopAudioStream();
    destroyAudioStream();
    unsigned unrecoverable=0,maxid=0;int first_bad=-1;
    for(unsigned i=0;i<65536;++i)if(seen[i])maxid=i;
    for(unsigned block=0;block<maxid/6;++block) {
        unsigned shards=0;for(unsigned i=0;i<6;++i)shards+=seen[block*6+i]!=0;
        if(shards<4){if(first_bad<0)first_bad=(int)block*4;++unrecoverable;}
    }
    unsigned hashsize;unsigned char hash[32];EVP_DigestFinal_ex(digest,hash,&hashsize);EVP_MD_CTX_destroy(digest);
    printf("RESULT received=%u duplicate=%u malformed=%u checksum=%u cycles=%u ended=%d failed=%d unrecoverableBlocks=%u firstBadBlock=%d firstFailureRcvbufErrors=%lld maxRecvGapUs=%llu maxFecUs=%llu sha256=",received,duplicates,malformed,checksum,cycles,atomic_load(&completed),atomic_load(&failed),unrecoverable,first_bad,first_failure_rcvbuf_errors,(unsigned long long)max_recv_gap,(unsigned long long)max_fec);
    for(unsigned i=0;i<hashsize;++i)printf("%02x",hash[i]);
    puts("");
    cleanupPlatform();return 0;
}
