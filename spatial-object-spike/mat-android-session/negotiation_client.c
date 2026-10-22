/* Controlled-peer test: production LiStartConnection through completed RTSP,
 * then interrupt before control/video/audio start. No media sink is opened. */
#include "Limelight.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
static int handshake,selected=-1;
static void message(const char* format,...) {
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);
}
static void completed(int stage) {
    if(stage==STAGE_RTSP_HANDSHAKE) {
        handshake=1;selected=LiGetNegotiatedAudioCodec();
        printf("RTSP_SELECTED codec=%d\n",selected);fflush(stdout);
        LiInterruptConnection();
    }
}
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    SERVER_INFORMATION server;STREAM_CONFIGURATION config;CONNECTION_LISTENER_CALLBACKS listener;
    DECODER_RENDERER_CALLBACKS video;AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeServerInformation(&server);LiInitializeStreamConfiguration(&config);
    LiInitializeConnectionCallbacks(&listener);LiInitializeVideoCallbacks(&video);LiInitializeAudioCallbacks(&audio);
    server.address="127.0.0.1";server.rtspSessionUrl=argv[1];
    server.serverInfoAppVersion=argv[3][0]=='g'?"7.1.431.0":"7.1.431.-1";
    server.serverInfoGfeVersion="3.23.0.74";server.serverCodecModeSupport=262145;
    config.width=640;config.height=360;config.fps=30;config.bitrate=1500;config.packetSize=1392;
    config.streamingRemotely=STREAM_CFG_LOCAL;config.audioConfiguration=AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats=VIDEO_FORMAT_H264;config.audioCodec=atoi(argv[2]);config.encryptionFlags=ENCFLG_NONE;
    listener.logMessage=message;listener.stageComplete=completed;
    int result=LiStartConnection(&server,&config,&listener,&video,&audio,NULL,0,NULL,0);
    if(!result)LiStopConnection();
    printf("NEGOTIATION_RESULT handshake=%d codec=%d startResult=%d mediaTested=0\n",handshake,selected,result);
    return handshake?0:3;
}
