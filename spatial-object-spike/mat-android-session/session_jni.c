/* Actual common-c session + actual production MAT JNI delivery. Hardware alone is fake. */
#include "Limelight-internal.h"
#include "mat_carrier_bridge.h"
#include <stdio.h>
#include <stdarg.h>
static JavaVM* vm;
static DWORD envKey=FLS_OUT_OF_INDEXES;
static void* bridge;
static volatile LONG terminated,complete,decodeCalls;
static void CALLBACK detach_env(void* ignored){(void)ignored;(*vm)->DetachCurrentThread(vm);}
static JNIEnv* thread_env(void){
    JNIEnv* env=NULL;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_8)==JNI_OK)return env;
    if((*vm)->AttachCurrentThreadAsDaemon(vm,(void**)&env,NULL)!=JNI_OK)return NULL;
    FlsSetValue(envKey,(void*)1);return env;
}
static int cycle(void* context,const unsigned char* b,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts){
    return MatPlaybackBridgeCycle(context,thread_env(),b,n,e,d,i,pts);
}
static int end(void* context,uint64_t n){
    int ok=MatPlaybackBridgeEnd(context,thread_env(),n);
    if(ok)InterlockedExchange(&complete,1);return ok;
}
static int audio_init(int cfg,const POPUS_MULTISTREAM_CONFIGURATION format,void* context,int flags){
    (void)cfg;(void)context;(void)flags;
    if(NegotiatedAudioCodec!=AUDIO_CODEC_MAT_IEC61937 || format->sampleRate!=48000 || format->samplesPerFrame!=960)return -1;
    LI_MAT_PLAYBACK_CALLBACKS callbacks={cycle,end,bridge};LiSetMatPlaybackCallbacks(&callbacks);return 0;
}
static void audio_cleanup(void){LiSetMatPlaybackCallbacks(NULL);}
static void forbidden_decode(char* p,int n){(void)p;(void)n;InterlockedIncrement(&decodeCalls);}
static void message(const char* fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);}
static void failed(int stage,int error){fprintf(stderr,"STAGE_FAILED %s %d\n",LiGetStageName(stage),error);}
static void ended(int error){fprintf(stderr,"TERMINATED %d\n",error);InterlockedExchange(&terminated,1);}
static int video_setup(int f,int w,int h,int fps,void* c,int flags){(void)f;(void)w;(void)h;(void)fps;(void)c;(void)flags;return 0;}
static int discard_video(PDECODE_UNIT unit){(void)unit;return DR_OK;}
static int unhex(const char* text,unsigned char* out,int bytes){
    if(strlen(text)!=(size_t)bytes*2)return 0;
    for(int i=0;i<bytes;i++){unsigned v;if(sscanf(text+2*i,"%2x",&v)!=1)return 0;out[i]=(unsigned char)v;}return 1;
}
JNIEXPORT jint JNICALL Java_com_limelight_binding_audio_MatSessionClient_nativeRun
    (JNIEnv* env,jclass type,jobjectArray args,jobject consumer){
    (void)type;
    if((*env)->GetArrayLength(env,args)!=8)return 2;
    const char* argv[8]={0};jstring strings[8]={0};int code=2;
    for(int i=0;i<8;i++){strings[i]=(*env)->GetObjectArrayElement(env,args,i);argv[i]=(*env)->GetStringUTFChars(env,strings[i],NULL);if(!argv[i])goto done;}
    (*env)->GetJavaVM(env,&vm);envKey=FlsAlloc(detach_env);if(envKey==FLS_OUT_OF_INDEXES)goto done;
    bridge=MatPlaybackBridgeCreate(env,consumer);if(!bridge)goto done;
    SERVER_INFORMATION server;STREAM_CONFIGURATION config;
    CONNECTION_LISTENER_CALLBACKS listener;DECODER_RENDERER_CALLBACKS video;AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeServerInformation(&server);LiInitializeStreamConfiguration(&config);
    LiInitializeConnectionCallbacks(&listener);LiInitializeVideoCallbacks(&video);LiInitializeAudioCallbacks(&audio);
    server.address="127.0.0.1";server.rtspSessionUrl=argv[0];server.serverInfoAppVersion=argv[1];
    server.serverInfoGfeVersion=argv[2];server.serverCodecModeSupport=(int)strtoul(argv[3],NULL,0);
    config.width=640;config.height=360;config.fps=30;config.bitrate=1500;config.packetSize=1392;
    config.streamingRemotely=STREAM_CFG_LOCAL;config.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats=VIDEO_FORMAT_H264;config.clientRefreshRateX100=3000;
    config.audioCodec=AUDIO_CODEC_MAT_IEC61937;config.encryptionFlags=!strcmp(argv[7],"aes")?ENCFLG_AUDIO:ENCFLG_NONE;
    if(!unhex(argv[5],(unsigned char*)config.remoteInputAesKey,16) || !unhex(argv[6],(unsigned char*)config.remoteInputAesIv,16))goto done;
    listener.stageFailed=failed;listener.logMessage=message;listener.connectionTerminated=ended;
    video.setup=video_setup;video.submitDecodeUnit=discard_video;video.capabilities=CAPABILITY_DIRECT_SUBMIT;
    audio.init=audio_init;audio.cleanup=audio_cleanup;audio.decodeAndPlaySample=forbidden_decode;
    int err=LiStartConnection(&server,&config,&listener,&video,&audio,NULL,0,NULL,0);
    if(err){fprintf(stderr,"START_FAILED %d\n",err);code=3;goto done;}
    printf("CONNECTED codec=%d encrypted=%d profileConfirmed=1 sha=%s cycles=%u\n",NegotiatedAudioCodec,
        AudioEncryptionEnabled,MatDumpProfile.sha256,MatDumpProfile.cycles);fflush(stdout);
    ULONGLONG deadline=GetTickCount64()+20000;
    while(!InterlockedCompareExchange(&terminated,0,0) && !InterlockedCompareExchange(&complete,0,0) && GetTickCount64()<deadline)Sleep(5);
    LiStopConnection();
    code=complete && !decodeCalls?0:4;
    printf("RESULT success=%d decodeCalls=%ld inputSHAVerified=%d rendererDrained=%d\n",code==0,decodeCalls,complete,complete);fflush(stdout);
done:
    LiSetMatPlaybackCallbacks(NULL);
    if(bridge){MatPlaybackBridgeDestroy(bridge,env);bridge=NULL;}
    if(envKey!=FLS_OUT_OF_INDEXES){FlsFree(envKey);envKey=FLS_OUT_OF_INDEXES;}
    for(int i=0;i<8;i++){if(argv[i])(*env)->ReleaseStringUTFChars(env,strings[i],argv[i]);if(strings[i])(*env)->DeleteLocalRef(env,strings[i]);}
    return code;
}
