// Full production LiStartConnection frontend. No audio or video decoder.
#include "Limelight-internal.h"
#include "../mat-diagnostics/MatDumpFile.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
static volatile LONG terminated, audio_decode_calls, video_units;
static MAT_DUMP_FILE* dump;
static const char* dump_path;
static void message(const char* fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
static void stage_start(int stage) { fprintf(stderr,"STAGE starting %s\n",LiGetStageName(stage)); }
static void stage_done(int stage) { fprintf(stderr,"STAGE complete %s\n",LiGetStageName(stage)); }
static void stage_fail(int stage,int err) { fprintf(stderr,"STAGE failed %s %d\n",LiGetStageName(stage),err); }
static void connection_end(int err) { fprintf(stderr,"TERMINATED %d\n",err); InterlockedExchange(&terminated,1); }
static int video_setup(int fmt,int w,int h,int fps,void* ctx,int flags) { (void)fmt;(void)w;(void)h;(void)fps;(void)ctx;(void)flags;return 0; }
static int discard_video(PDECODE_UNIT unit) { (void)unit;InterlockedIncrement(&video_units);return DR_OK; }
static int audio_init(int cfg,const POPUS_MULTISTREAM_CONFIGURATION opus,void* ctx,int flags) {
    (void)cfg;(void)ctx;(void)flags;
    if(NegotiatedAudioCodec!=AUDIO_CODEC_MAT_DUMP||opus->samplesPerFrame!=960)return -1;
    dump=MatDumpOpen(dump_path,&MatDumpProfile);if(!dump)return -1;
    LI_MAT_PLAYBACK_CALLBACKS callbacks=MatDumpCallbacks(dump);LiSetMatPlaybackCallbacks(&callbacks);return 0;
}
static void audio_cleanup(void){LiSetMatPlaybackCallbacks(NULL);MatDumpClose(dump);dump=NULL;}
static void forbidden_audio_decode(char* data,int size) { (void)data;(void)size;InterlockedIncrement(&audio_decode_calls); }
static int unhex(const char* text,unsigned char* out,int bytes) {
    if(strlen(text)!=(size_t)bytes*2)return 0;
    for(int i=0;i<bytes;i++){unsigned v;if(sscanf(text+2*i,"%2x",&v)!=1)return 0;out[i]=(unsigned char)v;}return 1;
}
int main(int argc,char** argv) {
    // URL appversion GfeVersion codec-mask output key-hex IV-hex plain|aes
    if(argc!=9)return 2;
    SERVER_INFORMATION server;STREAM_CONFIGURATION config;
    CONNECTION_LISTENER_CALLBACKS listener;DECODER_RENDERER_CALLBACKS video;AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeServerInformation(&server);LiInitializeStreamConfiguration(&config);
    LiInitializeConnectionCallbacks(&listener);LiInitializeVideoCallbacks(&video);LiInitializeAudioCallbacks(&audio);
    server.address="127.0.0.1";server.rtspSessionUrl=argv[1];server.serverInfoAppVersion=argv[2];
    server.serverInfoGfeVersion=argv[3];server.serverCodecModeSupport=(int)strtoul(argv[4],NULL,0);
    config.width=640;config.height=360;config.fps=30;config.bitrate=1500;config.packetSize=1392;
    config.streamingRemotely=STREAM_CFG_LOCAL;config.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats=VIDEO_FORMAT_H264;config.clientRefreshRateX100=3000;
    config.audioCodec=AUDIO_CODEC_MAT_DUMP;config.encryptionFlags=!strcmp(argv[8],"aes")?ENCFLG_AUDIO:ENCFLG_NONE;
    if(!unhex(argv[6],(unsigned char*)config.remoteInputAesKey,16) ||
        !unhex(argv[7],(unsigned char*)config.remoteInputAesIv,16))return 2;
    dump_path=argv[5];
    listener.stageStarting=stage_start;listener.stageComplete=stage_done;listener.stageFailed=stage_fail;
    listener.logMessage=message;listener.connectionTerminated=connection_end;
    video.setup=video_setup;video.submitDecodeUnit=discard_video;video.capabilities=CAPABILITY_DIRECT_SUBMIT;
    audio.init=audio_init;audio.cleanup=audio_cleanup;audio.decodeAndPlaySample=forbidden_audio_decode;
    int err=LiStartConnection(&server,&config,&listener,&video,&audio,NULL,0,NULL,0);
    if(err){fprintf(stderr,"START_FAILED %d\n",err);return 3;}
    // Reaching CONNECTED implies production RTSP validation accepted the echo.
    printf("CONNECTED codec=%d encrypted=%d profileConfirmed=1 sha=%s cycles=%u\n",NegotiatedAudioCodec,
        AudioEncryptionEnabled,MatDumpProfile.sha256,MatDumpProfile.cycles);fflush(stdout);
    ULONGLONG deadline=GetTickCount64()+15000;int exists=0;
    while(!InterlockedCompareExchange(&terminated,0,0) && GetTickCount64()<deadline) {
        struct stat st;if(!stat(argv[5],&st)){exists=1;break;}Sleep(10);
    }
    LiStopConnection();
    printf("RESULT success=%d decodeCalls=%ld videoUnitsDiscarded=%ld\n",exists && !audio_decode_calls,audio_decode_calls,video_units);
    return exists && !audio_decode_calls?0:4;
}
