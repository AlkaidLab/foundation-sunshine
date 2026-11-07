/* One serialized diagnostic session, matching common-c's global connection API. */
#include <jni.h>
#include <stdatomic.h>
#include <android/log.h>
#include "Limelight-internal.h"
typedef struct {
    JavaVM* vm; jobject audio,video;
    jmethodID init,cycle,end,opus,close,setup,submit,video_close;
    atomic_int failed,ended,terminated;
} Session;
static Session* active;
typedef struct { JNIEnv* env; int attached; } Scope;
static Scope enter(void) {
    Scope scope={0};
    if((*active->vm)->GetEnv(active->vm,(void**)&scope.env,JNI_VERSION_1_6)==JNI_EDETACHED) {
        if((*active->vm)->AttachCurrentThread(active->vm,&scope.env,NULL)!=JNI_OK)scope.env=NULL;
        else scope.attached=1;
    }
    if(!scope.env)atomic_store(&active->failed,1);
    return scope;
}
static int leave(Scope scope,int okay) {
    if(scope.env&&(*scope.env)->ExceptionCheck(scope.env)) {
        (*scope.env)->ExceptionDescribe(scope.env);(*scope.env)->ExceptionClear(scope.env);okay=0;
    }
    if(!okay)atomic_store(&active->failed,1);
    if(scope.attached)(*active->vm)->DetachCurrentThread(active->vm);
    return okay;
}
static void message(const char* fmt,...) {
    va_list args;va_start(args,fmt);__android_log_vprint(ANDROID_LOG_INFO,"MATSession",fmt,args);va_end(args);
}
static void terminated(int error){message("Terminated %d",error);atomic_store(&active->terminated,1);}
static int audio_cycle(void* ctx,const unsigned char* bytes,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts) {
    (void)ctx;Scope s=enter();if(!s.env)return 0;
    jobject buffer=(*s.env)->NewDirectByteBuffer(s.env,(void*)bytes,n);
    int okay=buffer&&(*s.env)->CallBooleanMethod(s.env,active->audio,active->cycle,buffer,(jlong)e,(jlong)d,(jlong)i,(jlong)pts);
    if(buffer)(*s.env)->DeleteLocalRef(s.env,buffer);
    return leave(s,okay);
}
static int audio_end(void* ctx,uint64_t count) {
    (void)ctx;Scope s=enter();if(!s.env)return 0;
    int okay=leave(s,(*s.env)->CallBooleanMethod(s.env,active->audio,active->end,(jlong)count));
    if(okay)atomic_store(&active->ended,1);return okay;
}
static int audio_init(int config,POPUS_MULTISTREAM_CONFIGURATION opus,void* ctx,int flags) {
    (void)config;(void)opus;(void)ctx;(void)flags;Scope s=enter();if(!s.env)return -1;
    int okay=leave(s,(*s.env)->CallBooleanMethod(s.env,active->audio,active->init,NegotiatedAudioCodec,(jlong)MatLiveProfile.epoch,(jlong)MatLiveProfile.domain));
    if(okay&&NegotiatedAudioCodec==AUDIO_CODEC_MAT_LIVE) {
        LI_MAT_PLAYBACK_CALLBACKS cb={audio_cycle,audio_end,NULL};LiSetMatPlaybackCallbacks(&cb);
    }
    return okay?0:-1;
}
static void opus_sample(char* bytes,int n) {
    Scope s=enter();if(!s.env)return;
    jobject buffer=(*s.env)->NewDirectByteBuffer(s.env,bytes,n);
    int okay=buffer&&(*s.env)->CallBooleanMethod(s.env,active->audio,active->opus,buffer);
    if(buffer)(*s.env)->DeleteLocalRef(s.env,buffer);leave(s,okay);
}
static void audio_cleanup(void) {
    LiSetMatPlaybackCallbacks(NULL);Scope s=enter();if(!s.env)return;
    (*s.env)->CallVoidMethod(s.env,active->audio,active->close);leave(s,1);
}
static int video_setup(int format,int width,int height,int fps,void* ctx,int flags) {
    (void)ctx;(void)flags;Scope s=enter();if(!s.env)return -1;
    return leave(s,(*s.env)->CallBooleanMethod(s.env,active->video,active->setup,format,width,height,fps))?0:-1;
}
static int video_submit(PDECODE_UNIT unit) {
    if(unit->fullLength<=0||unit->fullLength>4*1024*1024){atomic_store(&active->failed,1);return DR_NEED_IDR;}
    uint8_t* bytes=malloc(unit->fullLength);if(!bytes){atomic_store(&active->failed,1);return DR_NEED_IDR;}
    int offset=0;
    for(PLENTRY p=unit->bufferList;p;p=p->next) {
        if(p->length<=0||p->length>unit->fullLength-offset)break;
        memcpy(bytes+offset,p->data,p->length);offset+=p->length;
    }
    int okay=0;Scope s=enter();
    if(s.env) {
        jobject buffer=offset==unit->fullLength?(*s.env)->NewDirectByteBuffer(s.env,bytes,offset):NULL;
        okay=buffer&&(*s.env)->CallBooleanMethod(s.env,active->video,active->submit,buffer,(jlong)unit->presentationTimeUs);
        if(buffer)(*s.env)->DeleteLocalRef(s.env,buffer);okay=leave(s,okay);
    }
    free(bytes);return okay?DR_OK:DR_NEED_IDR;
}
static void video_cleanup(void) {
    Scope s=enter();if(!s.env)return;
    (*s.env)->CallVoidMethod(s.env,active->video,active->video_close);leave(s,1);
}
static int unhex(const char* text,char* out) {
    if(strlen(text)!=32)return 0;
    for(int i=0;i<16;++i){unsigned n;if(sscanf(text+i*2,"%2x",&n)!=1)return 0;out[i]=(char)n;}return 1;
}
JNIEXPORT jlongArray JNICALL Java_com_sunshine_matmock_Session_runNative(
        JNIEnv* env,jclass type,jobject audio_sink,jobject decoder,jobjectArray arguments) {
    (void)type;Session s={0};active=&s;(*env)->GetJavaVM(env,&s.vm);
    s.audio=(*env)->NewGlobalRef(env,audio_sink);s.video=(*env)->NewGlobalRef(env,decoder);
    jclass ac=(*env)->GetObjectClass(env,audio_sink),vc=(*env)->GetObjectClass(env,decoder);
    s.init=(*env)->GetMethodID(env,ac,"init","(IJJ)Z");s.cycle=(*env)->GetMethodID(env,ac,"cycle","(Ljava/nio/ByteBuffer;JJJJ)Z");
    s.end=(*env)->GetMethodID(env,ac,"end","(J)Z");s.opus=(*env)->GetMethodID(env,ac,"opus","(Ljava/nio/ByteBuffer;)Z");
    s.close=(*env)->GetMethodID(env,ac,"close","()V");s.setup=(*env)->GetMethodID(env,vc,"setup","(IIII)Z");
    s.submit=(*env)->GetMethodID(env,vc,"submit","(Ljava/nio/ByteBuffer;J)Z");s.video_close=(*env)->GetMethodID(env,vc,"close","()V");
    (*env)->DeleteLocalRef(env,ac);(*env)->DeleteLocalRef(env,vc);
    jstring strings[9];const char* args[9];
    for(int i=0;i<9;++i){strings[i]=(*env)->GetObjectArrayElement(env,arguments,i);args[i]=(*env)->GetStringUTFChars(env,strings[i],NULL);}
    SERVER_INFORMATION server;STREAM_CONFIGURATION config;CONNECTION_LISTENER_CALLBACKS listener;
    DECODER_RENDERER_CALLBACKS video;AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeServerInformation(&server);LiInitializeStreamConfiguration(&config);LiInitializeConnectionCallbacks(&listener);
    LiInitializeVideoCallbacks(&video);LiInitializeAudioCallbacks(&audio);
    server.address=args[0];server.rtspSessionUrl=args[1];server.serverInfoAppVersion=args[2];server.serverInfoGfeVersion=args[3];server.serverCodecModeSupport=atoi(args[4]);
    config.width=640;config.height=360;config.fps=30;config.bitrate=1500;config.packetSize=1392;config.streamingRemotely=STREAM_CFG_LOCAL;
    config.audioConfiguration=AUDIO_CONFIGURATION_STEREO;config.supportedVideoFormats=VIDEO_FORMAT_H264;
    config.clientRefreshRateX100=3000;config.audioCodec=atoi(args[7]);config.encryptionFlags=ENCFLG_AUDIO;
    listener.logMessage=message;listener.connectionTerminated=terminated;
    video.setup=video_setup;video.submitDecodeUnit=video_submit;video.cleanup=video_cleanup;video.capabilities=CAPABILITY_DIRECT_SUBMIT;
    audio.init=audio_init;audio.cleanup=audio_cleanup;audio.decodeAndPlaySample=opus_sample;
    int seconds=atoi(args[8]);
    int error=(!unhex(args[5],config.remoteInputAesKey)||!unhex(args[6],config.remoteInputAesIv)||seconds<1||seconds>60)?-1:
        LiStartConnection(&server,&config,&listener,&video,&audio,NULL,0,NULL,0);
    int codec=NegotiatedAudioCodec,encrypted=AudioEncryptionEnabled,connection_failed=0;
    if(!error) {
        uint64_t deadline=PltGetMillis()+(uint64_t)seconds*1000;
        while(!atomic_load(&s.ended)&&!atomic_load(&s.failed)&&!atomic_load(&s.terminated)&&PltGetMillis()<deadline)PltSleepMs(5);
        connection_failed=atomic_load(&s.terminated);LiStopConnection();
    }
    jlong values[]={error,codec,encrypted,atomic_load(&s.ended),atomic_load(&s.failed)||connection_failed};
    for(int i=0;i<9;++i){(*env)->ReleaseStringUTFChars(env,strings[i],args[i]);(*env)->DeleteLocalRef(env,strings[i]);}
    (*env)->DeleteGlobalRef(env,s.audio);(*env)->DeleteGlobalRef(env,s.video);active=NULL;
    jlongArray result=(*env)->NewLongArray(env,5);if(result)(*env)->SetLongArrayRegion(env,result,0,5,values);return result;
}
