// Actual AudioStream receive/decoder threads with an in-memory mock consumer.
#include "AudioStream.c"
#include "MatReceiver.h"
#include "mock_output.h"
#include <stdarg.h>
#include <stdio.h>
static LONG terminated,decode_calls,cycles,ended;
static MockDevice device;static MockOutput* output;static MockOutputStats output_stats;
static EVP_MD_CTX* received_digest;static char received_sha[65];
static int output_mode;
static void cleanup(void);
static void log_message(const char* fmt,...){va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);}
static void terminate(int error){(void)error;InterlockedExchange(&terminated,1);}
static int cycle(void* ctx,const unsigned char* bytes,int n,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts){
    (void)ctx;
    if(!MatCarrierIsValid(bytes,(size_t)n)||epoch!=1||domain!=1||index!=0||pts!=0)return 0;
    if(!mock_output_offer(output,bytes,n,epoch,domain,index,pts,GetTickCount64())||!EVP_DigestUpdate(received_digest,bytes,n))return 0;
    InterlockedIncrement(&cycles);return 1;
}
static int end(void* ctx,uint64_t count){(void)ctx;if(count!=1||cycles!=1)return 0;mock_hex_digest(received_digest,received_sha);
    return mock_output_end(output,count,GetTickCount64())&&mock_output_wait_end(output);}
static int audio_init(int config,POPUS_MULTISTREAM_CONFIGURATION opus,void* ctx,int flags){
    (void)config;(void)ctx;(void)flags;
    if(opus->samplesPerFrame!=960||NegotiatedAudioCodec!=AUDIO_CODEC_MAT_LIVE)return -1;
    MockCapabilities caps={1,1,1,1};uint64_t now=GetTickCount64();
    if(!mock_device_init(&device,caps,now))return -1;
    if(output_mode==1||output_mode==3)device.speed_percent=0;
    output=mock_output_open(mock_device_sink(&device),1,1,1,now,1000,3000);
    received_digest=EVP_MD_CTX_create();
    if(!output||!received_digest||!EVP_DigestInit_ex(received_digest,EVP_sha256(),NULL)){cleanup();return -1;}
    LI_MAT_PLAYBACK_CALLBACKS callbacks={cycle,end,NULL};LiSetMatPlaybackCallbacks(&callbacks);return 0;
}
static void cleanup(void){LiSetMatPlaybackCallbacks(NULL);if(output){mock_output_stop(output);output_stats=mock_output_poll(output,GetTickCount64());mock_output_free(output);output=NULL;}
    mock_device_close(&device);EVP_MD_CTX_destroy(received_digest);received_digest=NULL;}
static void noop(void){}
static void forbidden_decode(char* p,int n){(void)p;(void)n;InterlockedIncrement(&decode_calls);terminate(-1);}
int main(int argc,char** argv){
    if(argc==2){if(!strcmp(argv[1],"--sink-stall"))output_mode=1;else if(!strcmp(argv[1],"--route-loss"))output_mode=2;
        else if(!strcmp(argv[1],"--cancel-drain"))output_mode=3;else return 2;}
    else if(argc!=1)return 2;
    if(initializePlatform())return 3;
    ListenerCallbacks.logMessage=log_message;ListenerCallbacks.connectionTerminated=terminate;
    AudioCallbacks.init=audio_init;AudioCallbacks.start=noop;AudioCallbacks.stop=noop;AudioCallbacks.cleanup=cleanup;
    AudioCallbacks.decodeAndPlaySample=forbidden_decode;AudioCallbacks.capabilities=0;
    AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=500;AppVersionQuad[3]=-1;
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.audioCodec=NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;
    StreamConfig.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    MatLiveProfile=(LI_MAT_LIVE_PROFILE){1,1};
    AudioPacketDuration=1;AudioEncryptionEnabled=false;
    AddrLen=sizeof(struct sockaddr_in);
    struct sockaddr_in* remote=(struct sockaddr_in*)&RemoteAddr;remote->sin_family=AF_INET;remote->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    memcpy(&LocalAddr,&RemoteAddr,AddrLen);RemoteAddrString="127.0.0.1";
    SOCKET ping_sink=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);struct sockaddr_in addr=*remote;addr.sin_port=0;
    if(bind(ping_sink,(struct sockaddr*)&addr,sizeof(addr)))return 5;
    int addr_len=sizeof(addr);getsockname(ping_sink,(struct sockaddr*)&addr,&addr_len);AudioPortNumber=ntohs(addr.sin_port);
    NormalQualityOpusConfig.sampleRate=48000;NormalQualityOpusConfig.channelCount=2;NormalQualityOpusConfig.streams=1;NormalQualityOpusConfig.coupledStreams=1;
    NormalQualityOpusConfig.mapping[1]=1;
    if(initializeAudioStream()||notifyAudioPortNegotiationComplete()||pingThreadStarted)return 6;
    if(startAudioStream(NULL,0)||!pingThreadStarted)return 7;
    getsockname(rtpSocket,(struct sockaddr*)&addr,&addr_len);printf("READY %u\n",ntohs(addr.sin_port));fflush(stdout);
    uint64_t deadline=PltGetMillis()+6000;
    while(!InterlockedCompareExchange(&terminated,0,0)&&!InterlockedCompareExchange(&ended,0,0)&&PltGetMillis()<deadline){
        uint64_t now=GetTickCount64();
        if(output_mode==2&&device.written)device.capabilities.route=2;
        if(output_mode==3&&mock_output_snapshot(output).state==MOCK_DRAINING){mock_output_stop(output);terminate(-1);}
        mock_device_tick(&device,now);output_stats=mock_output_poll(output,now);
        if(output_stats.state==MOCK_COMPLETED)InterlockedExchange(&ended,1);
        if(output_stats.state==MOCK_FAILED){fprintf(stderr,"MOCK_OUTPUT_FAILED %s\n",output_stats.error);terminate(-1);}
        PltSleepMs(1);
    }
    mock_output_stop(output);stopAudioStream();destroyAudioStream();closesocket(ping_sink);
    int success=ended&&cycles==1&&!terminated&&!decode_calls&&output_stats.state==MOCK_COMPLETED&&
        !strcmp(received_sha,device.written_sha)&&!strcmp(received_sha,device.played_sha);
    printf("RESULT success=%d cycles=%ld ended=%ld decodeCalls=%ld submittedBytes=%llu playedFrames=%llu sinkCompleted=%d\n",success,cycles,ended,decode_calls,
        (unsigned long long)output_stats.submitted_bytes,(unsigned long long)output_stats.played_frames,output_stats.state==MOCK_COMPLETED);fflush(stdout);
    cleanupPlatform();return success?0:1;
}
