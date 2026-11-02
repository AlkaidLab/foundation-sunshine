/* Full production LiStartConnection + native MAT callbacks + H.264 decoding.
 * Diagnostic strict-live sink; no HDMI capability claim, files or replay. */
#include <errno.h>
// Limelight's socket compatibility layer remaps EAGAIN to WSAEWOULDBLOCK.
// Preserve the C-runtime value used by FFmpeg before including that layer.
enum { FFMPEG_EAGAIN=EAGAIN };
#include "Limelight-internal.h"
#include "mock_output.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <io.h>
static volatile LONG terminated,ended,decodeFailed,outputFailed;
static uint64_t cycles,videoFrames,videoUnits,firstAudio,lastAudio,firstVideo,lastVideo,lastVideoPts;
static int audioDecodeCalls,videoPtsReordered;static char audioSha[65];
static EVP_MD_CTX* digest;static AVCodecContext* decoder;static AVFrame* frame;
static MockDevice device;static MockOutput* output;static MockOutputStats outputStats;
static void audio_cleanup(void);
static void message(const char* fmt,...){va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);}
static void stage_start(int stage){fprintf(stderr,"STAGE starting %s\n",LiGetStageName(stage));}
static void stage_done(int stage){fprintf(stderr,"STAGE complete %s\n",LiGetStageName(stage));}
static void stage_fail(int stage,int error){fprintf(stderr,"STAGE failed %s %d\n",LiGetStageName(stage),error);}
static void connection_end(int error){fprintf(stderr,"TERMINATED %d\n",error);InterlockedExchange(&terminated,1);}
static int drain_video(void){int r;while((r=avcodec_receive_frame(decoder,frame))>=0){
    if(frame->width!=640||frame->height!=360){fprintf(stderr,"VIDEO_DIMENSIONS %dx%d\n",frame->width,frame->height);return -1;}
    uint64_t now=GetTickCount64();if(!videoFrames)firstVideo=now;lastVideo=now;++videoFrames;av_frame_unref(frame);
 }if(r!=AVERROR(FFMPEG_EAGAIN)&&r!=AVERROR_EOF)fprintf(stderr,"VIDEO_RECEIVE_FAILED code=%d expectedAgain=%d\n",r,AVERROR(FFMPEG_EAGAIN));return r==AVERROR(FFMPEG_EAGAIN)||r==AVERROR_EOF?0:-1;}
static int video_setup(int format,int width,int height,int fps,void* ctx,int flags){
 (void)ctx;(void)flags;if(format!=VIDEO_FORMAT_H264||width!=640||height!=360||fps!=30)return -1;
 const AVCodec* codec=avcodec_find_decoder(AV_CODEC_ID_H264);decoder=avcodec_alloc_context3(codec);frame=av_frame_alloc();
 if(!codec||!decoder||!frame)return -1;decoder->thread_count=1;decoder->flags|=AV_CODEC_FLAG_LOW_DELAY;
 return avcodec_open2(decoder,codec,NULL);
}
static int decode_video(PDECODE_UNIT unit){
 if(unit->fullLength<=0||unit->fullLength>4*1024*1024){InterlockedExchange(&decodeFailed,1);return DR_NEED_IDR;}
 AVPacket* packet=av_packet_alloc();int r=packet?av_new_packet(packet,unit->fullLength):-1;size_t offset=0;
 if(r>=0){for(PLENTRY p=unit->bufferList;p;p=p->next){if(p->length<=0||(size_t)p->length>unit->fullLength-offset){r=-1;break;}memcpy(packet->data+offset,p->data,p->length);offset+=p->length;}
  if(offset!=(size_t)unit->fullLength)r=-1;
  if(videoUnits&&unit->presentationTimeUs<lastVideoPts)videoPtsReordered=1;lastVideoPts=unit->presentationTimeUs;++videoUnits;
  if(r>=0){r=avcodec_send_packet(decoder,packet);if(r==AVERROR(FFMPEG_EAGAIN)){r=drain_video();if(!r)r=avcodec_send_packet(decoder,packet);}if(r>=0)r=drain_video();}
 }
 av_packet_free(&packet);if(r<0){fprintf(stderr,"VIDEO_DECODE_FAILED code=%d bytes=%d copied=%zu\n",r,unit->fullLength,offset);InterlockedExchange(&decodeFailed,1);return DR_NEED_IDR;}return DR_OK;
}
static void video_cleanup(void){av_frame_free(&frame);avcodec_free_context(&decoder);}
static int audio_cycle(void* ctx,const unsigned char* p,int n,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts){
 (void)ctx;if(n!=61440||epoch!=MatLiveProfile.epoch||domain!=MatLiveProfile.domain||index!=cycles||pts!=index*960||
    !mock_output_offer(output,p,n,epoch,domain,index,pts,GetTickCount64())||!EVP_DigestUpdate(digest,p,n))return 0;
 uint64_t now=GetTickCount64();if(!cycles)firstAudio=now;lastAudio=now;++cycles;return 1;
}
static int audio_end(void* ctx,uint64_t count){
 (void)ctx;unsigned n=0;unsigned char hash[32];if(count!=cycles||!cycles||!EVP_DigestFinal_ex(digest,hash,&n)||n!=32)return 0;
 for(unsigned i=0;i<32;++i)sprintf(audioSha+i*2,"%02x",hash[i]);
 return mock_output_end(output,count,GetTickCount64())&&mock_output_wait_end(output);
}
static int audio_init(int cfg,const POPUS_MULTISTREAM_CONFIGURATION opus,void* ctx,int flags){
 (void)cfg;(void)ctx;(void)flags;if(NegotiatedAudioCodec!=AUDIO_CODEC_MAT_LIVE||opus->samplesPerFrame!=960)return -1;
 digest=EVP_MD_CTX_new();if(!digest||!EVP_DigestInit_ex(digest,EVP_sha256(),NULL)){audio_cleanup();return -1;}
 MockCapabilities caps={1,1,1,1};uint64_t now=GetTickCount64();if(!mock_device_init(&device,caps,now)){audio_cleanup();return -1;}
 output=mock_output_open(mock_device_sink(&device),1,MatLiveProfile.epoch,MatLiveProfile.domain,now,1000,3000);if(!output){audio_cleanup();return -1;}
 LI_MAT_PLAYBACK_CALLBACKS cb={audio_cycle,audio_end,NULL};LiSetMatPlaybackCallbacks(&cb);return 0;
}
static void audio_cleanup(void){LiSetMatPlaybackCallbacks(NULL);if(output){mock_output_stop(output);outputStats=mock_output_poll(output,GetTickCount64());mock_output_free(output);output=NULL;}
 mock_device_close(&device);EVP_MD_CTX_free(digest);digest=NULL;}
static void forbidden_audio(char* p,int n){(void)p;(void)n;++audioDecodeCalls;}
static int unhex(const char* s,unsigned char* out,int n){if(strlen(s)!=(size_t)n*2)return 0;for(int i=0;i<n;++i){unsigned v;if(sscanf(s+i*2,"%2x",&v)!=1)return 0;out[i]=(unsigned char)v;}return 1;}
static int decoder_self_test(void){
 if(video_setup(VIDEO_FORMAT_H264,640,360,30,NULL,0))return 1;
 for(;;){uint32_t length;uint64_t pts;if(fread(&length,4,1,stdin)!=1)break;if(!length||length>4*1024*1024||fread(&pts,8,1,stdin)!=1){fprintf(stderr,"SELFTEST_LENGTH %u\n",length);return 1;}
  char* bytes=malloc(length);if(!bytes||fread(bytes,1,length,stdin)!=length)return 1;
  LENTRY entry={NULL,bytes,(int)length,BUFFER_TYPE_PICDATA};DECODE_UNIT unit={0};unit.fullLength=(int)length;unit.bufferList=&entry;unit.presentationTimeUs=pts;
  int result=decode_video(&unit);free(bytes);if(result!=DR_OK)return 1;
 }
 video_cleanup();int okay=videoFrames==120&&!decodeFailed&&!videoPtsReordered;
 printf("%s generated H264 pipe -> production client decode callback: frames=%llu nativeCapture=0 mediaFiles=0\n",okay?"PASS":"FAIL",(unsigned long long)videoFrames);return okay?0:1;
}
int main(int argc,char** argv){
 if(argc==2&&!strcmp(argv[1],"--self-test")){_setmode(_fileno(stdin),_O_BINARY);return decoder_self_test();}
 // URL appversion GfeVersion codec-mask key-hex iv-hex result-json max-seconds
 if(argc!=9)return 2;setvbuf(stdout,NULL,_IONBF,0);
 SERVER_INFORMATION server;STREAM_CONFIGURATION config;CONNECTION_LISTENER_CALLBACKS listener;
 DECODER_RENDERER_CALLBACKS video;AUDIO_RENDERER_CALLBACKS audio;
 LiInitializeServerInformation(&server);LiInitializeStreamConfiguration(&config);LiInitializeConnectionCallbacks(&listener);LiInitializeVideoCallbacks(&video);LiInitializeAudioCallbacks(&audio);
 server.address="127.0.0.1";server.rtspSessionUrl=argv[1];server.serverInfoAppVersion=argv[2];server.serverInfoGfeVersion=argv[3];server.serverCodecModeSupport=(int)strtoul(argv[4],NULL,0);
 config.width=640;config.height=360;config.fps=30;config.bitrate=1500;config.packetSize=1392;config.streamingRemotely=STREAM_CFG_LOCAL;
 config.audioConfiguration=AUDIO_CONFIGURATION_STEREO;config.supportedVideoFormats=VIDEO_FORMAT_H264;config.clientRefreshRateX100=3000;config.audioCodec=AUDIO_CODEC_MAT_LIVE;config.encryptionFlags=ENCFLG_AUDIO;
 unsigned seconds=(unsigned)strtoul(argv[8],NULL,10);if(!seconds||seconds>60||!unhex(argv[5],(unsigned char*)config.remoteInputAesKey,16)||!unhex(argv[6],(unsigned char*)config.remoteInputAesIv,16))return 2;
 listener.stageStarting=stage_start;listener.stageComplete=stage_done;listener.stageFailed=stage_fail;listener.logMessage=message;listener.connectionTerminated=connection_end;
 video.setup=video_setup;video.submitDecodeUnit=decode_video;video.cleanup=video_cleanup;video.capabilities=CAPABILITY_DIRECT_SUBMIT;
 audio.init=audio_init;audio.cleanup=audio_cleanup;audio.decodeAndPlaySample=forbidden_audio;
 uint64_t began=GetTickCount64();int error=LiStartConnection(&server,&config,&listener,&video,&audio,NULL,0,NULL,0);
 if(error){fprintf(stderr,"START_FAILED %d\n",error);return 3;}
 int codec=NegotiatedAudioCodec,encrypted=AudioEncryptionEnabled;
 printf("CONNECTED codec=%d encrypted=%d profileConfirmed=1 video=H264\n",codec,encrypted);
 while(!InterlockedCompareExchange(&terminated,0,0)&&!InterlockedCompareExchange(&ended,0,0)&&!InterlockedCompareExchange(&decodeFailed,0,0)&&!InterlockedCompareExchange(&outputFailed,0,0)&&GetTickCount64()-began<seconds*1000){
  uint64_t now=GetTickCount64();mock_device_tick(&device,now);outputStats=mock_output_poll(output,now);
  if(outputStats.state==MOCK_COMPLETED)InterlockedExchange(&ended,1);
  if(outputStats.state==MOCK_FAILED){fprintf(stderr,"MOCK_OUTPUT_FAILED %s\n",outputStats.error);InterlockedExchange(&outputFailed,1);}Sleep(5);
 }
 int cleanEnd=InterlockedCompareExchange(&ended,0,0),connectionFailed=InterlockedCompareExchange(&terminated,0,0);
 mock_output_stop(output);LiStopConnection();uint64_t overlapStart=firstAudio>firstVideo?firstAudio:firstVideo,overlapEnd=lastAudio<lastVideo?lastAudio:lastVideo;
 uint64_t overlap=overlapEnd>overlapStart?overlapEnd-overlapStart:0;
 int sinkPassed=outputStats.state==MOCK_COMPLETED&&!strcmp(audioSha,device.written_sha)&&!strcmp(audioSha,device.played_sha);
 int passed=cleanEnd&&sinkPassed&&!connectionFailed&&!decodeFailed&&!outputFailed&&!audioDecodeCalls&&!videoPtsReordered&&codec==7&&encrypted&&videoFrames>=30&&cycles>=100&&overlap>=2000;
 FILE* result=fopen(argv[7],"wx");if(!result)return 5;
 fprintf(result,"{\"mockOutputCompleted\":%s,\"mockWrittenSha256\":\"%s\",\"mockPlayedSha256\":\"%s\",\"mockPlayedFrames\":%llu,\"mockPeakBacklogBytes\":%llu,",sinkPassed?"true":"false",device.written_sha,device.played_sha,(unsigned long long)outputStats.played_frames,(unsigned long long)outputStats.peak_backlog_bytes);
 fprintf(result,"\"passed\":%s,\"codec\":%d,\"encrypted\":%s,\"authenticatedEOS\":%s,\"cycles\":%llu,\"bytes\":%llu,\"sha256\":\"%s\",\"videoFramesDecoded\":%llu,\"videoUnits\":%llu,\"videoPtsReordered\":%s,\"decodeFailed\":%s,\"audioDecodeCalls\":%d,\"overlapMs\":%llu,\"fullLiStartConnection\":true,\"hardwareSink\":false,\"avSyncCalibrated\":false}\n",passed?"true":"false",codec,encrypted?"true":"false",cleanEnd?"true":"false",(unsigned long long)cycles,(unsigned long long)(cycles*61440),audioSha,(unsigned long long)videoFrames,(unsigned long long)videoUnits,videoPtsReordered?"true":"false",decodeFailed?"true":"false",audioDecodeCalls,(unsigned long long)overlap);fclose(result);
 printf("RESULT passed=%d cycles=%llu videoFramesDecoded=%llu overlapMs=%llu sha256=%s authenticatedEOS=%d\n",passed,(unsigned long long)cycles,(unsigned long long)videoFrames,(unsigned long long)overlap,audioSha,cleanEnd);return passed?0:4;
}
