// Test fixture frontend, ACTUAL Android-checkout AudioStream.c (threads, LBQ,
// initial-drop/ready-ping policy, decryption and MAT dump). Not an APK/device.
#include "AudioStream.c"
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
static LONG terminated,decode_calls;
static void log_message(const char* fmt,...){va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);}
static void terminate(int error){(void)error;InterlockedExchange(&terminated,1);}
static int audio_init(int config,POPUS_MULTISTREAM_CONFIGURATION opus,void* ctx,int flags){
    (void)config;(void)ctx;(void)flags;return opus->samplesPerFrame==960 && NegotiatedAudioCodec==AUDIO_CODEC_MAT_DUMP?0:-1;
}
static void noop(void){}
static void forbidden_decode(char* p,int n){(void)p;(void)n;InterlockedIncrement(&decode_calls);terminate(-1);}
int main(int argc,char** argv){
    if(argc!=5)return 2; // final.raw trusted-sha cycles plain|aes
    if(initializePlatform())return 3;
    ListenerCallbacks.logMessage=log_message;ListenerCallbacks.connectionTerminated=terminate;
    AudioCallbacks.init=audio_init;AudioCallbacks.start=noop;AudioCallbacks.stop=noop;AudioCallbacks.cleanup=noop;
    AudioCallbacks.decodeAndPlaySample=forbidden_decode;AudioCallbacks.capabilities=0;
    AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=500;AppVersionQuad[3]=-1;
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.audioCodec=NegotiatedAudioCodec=AUDIO_CODEC_MAT_DUMP;
    StreamConfig.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    StreamConfig.width=1280;StreamConfig.height=720;StreamConfig.fps=60;StreamConfig.bitrate=10000;StreamConfig.packetSize=1392;
    NegotiatedVideoFormat=VIDEO_FORMAT_H264;
    if(strlen(argv[1])>=sizeof(StreamConfig.matDumpPath))return 2;
    strcpy(StreamConfig.matDumpPath,argv[1]);
    char describe[256];snprintf(describe,sizeof(describe),"a=x-ss-audio.mat-dump:1\r\na=x-ss-audio.mat-sha256:%s\r\na=x-ss-audio.mat-cycles:%s\r\n",argv[2],argv[3]);
    if(!MatProfileParse(&MatDumpProfile,describe))return 2;
    if(!MatProfileConfirm(&MatDumpProfile,MAT_DUMP_NAME,argv[2],argv[3]))return 2;
    AudioEncryptionEnabled=!strcmp(argv[4],"aes");
    for(int i=0;i<16;i++)StreamConfig.remoteInputAesKey[i]=(char)i;
    uint32_t iv=htonl(0x10203040u);memcpy(StreamConfig.remoteInputAesIv,&iv,4);
    AddrLen=sizeof(struct sockaddr_in);
    struct sockaddr_in* remote=(struct sockaddr_in*)&RemoteAddr;remote->sin_family=AF_INET;remote->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    memcpy(&LocalAddr,&RemoteAddr,AddrLen);
    RemoteAddrString="127.0.0.1";
    int length=0;char* announce=getSdpPayloadForStreamConfig(12,&length);
    if(!announce || AudioPacketDuration!=1 || !strstr(announce,"x-ml-audio.codec:mat-dump-v1") ||
        !strstr(announce,argv[2]) || !strstr(announce,"x-nv-aqos.packetDuration:1"))return 4;
    char announce_path[1100];snprintf(announce_path,sizeof(announce_path),"%s.announce",argv[1]);
    FILE* f=fopen(announce_path,"wx");if(!f)return 4;fwrite(announce,1,length,f);fclose(f);free(announce);
    SOCKET ping_sink=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);struct sockaddr_in addr=*remote;addr.sin_port=0;
    if(bind(ping_sink,(struct sockaddr*)&addr,sizeof(addr)))return 5;
    int addr_len=sizeof(addr);getsockname(ping_sink,(struct sockaddr*)&addr,&addr_len);AudioPortNumber=ntohs(addr.sin_port);
    NormalQualityOpusConfig.sampleRate=48000;NormalQualityOpusConfig.channelCount=2;NormalQualityOpusConfig.streams=1;NormalQualityOpusConfig.coupledStreams=1;
    NormalQualityOpusConfig.mapping[1]=1;
    if(initializeAudioStream() || notifyAudioPortNegotiationComplete() || pingThreadStarted)return 6;
    if(startAudioStream(NULL,0) || !pingThreadStarted)return 7;
    getsockname(rtpSocket,(struct sockaddr*)&addr,&addr_len);
    printf("READY %u\n",ntohs(addr.sin_port));fflush(stdout);
    uint64_t deadline=PltGetMillis()+10000;int exists=0;
    while(!InterlockedCompareExchange(&terminated,0,0) && PltGetMillis()<deadline){
        struct stat st;if(!stat(argv[1],&st)){exists=1;break;}PltSleepMs(10);
    }
    PltSleepMs(50);stopAudioStream();destroyAudioStream();closesocket(ping_sink);
    int result=exists && !terminated && !decode_calls?0:1;
    printf("RESULT success=%d decodeCalls=%ld readyAfterReceiver=1\n",!result,decode_calls);fflush(stdout);
    cleanupPlatform();return result;
}
