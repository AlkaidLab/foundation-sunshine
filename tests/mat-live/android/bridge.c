/* Diagnostic JNI adapter, using the current receiver, not the archived Android fork.
 * Generated encrypted packets stay in memory. There is no AudioTrack or decoder. */
#include <jni.h>
#include <pthread.h>
#include "MatLivePlayback.h"
#include "MatLiveCrypto.h"
#include "../../../tools/mat-live/live_check_transport.h"

typedef struct {
    JavaVM* vm;
    JNIEnv* env; /* Only the attached worker uses this environment. */
    jobject sink;
    jmethodID cycle, end;
    jthrowable exception;
    uint64_t epoch;
    int mode, result;
} Run;

static int capture_exception(Run* r) {
    jthrowable error=(*r->env)->ExceptionOccurred(r->env);
    if(!error)return 0;
    (*r->env)->ExceptionClear(r->env);
    r->exception=(*r->env)->NewGlobalRef(r->env,error);
    (*r->env)->DeleteLocalRef(r->env,error);
    return 1;
}
static int cycle(void* context,const uint8_t* bytes,int n,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts) {
    Run* r=context;
    jobject buffer=(*r->env)->NewDirectByteBuffer(r->env,(void*)bytes,n);
    if(!buffer)return capture_exception(r),0;
    jboolean okay=(*r->env)->CallBooleanMethod(r->env,r->sink,r->cycle,buffer,
        (jlong)epoch,(jlong)domain,(jlong)index,(jlong)pts);
    int failed=capture_exception(r);
    (*r->env)->DeleteLocalRef(r->env,buffer);
    return !failed&&okay;
}
static int end(void* context,uint64_t count) {
    Run* r=context;
    jboolean okay=(*r->env)->CallBooleanMethod(r->env,r->sink,r->end,(jlong)count);
    return !capture_exception(r)&&okay;
}
static int send_packet(Run* r,MAT_LIVE_PLAYBACK* playback,MAT_LIVE_CIPHER* cipher,
                       uint8_t* plain,uint64_t sequence) {
    uint8_t wire[MAT_LIVE_PACKET];
    if(!MatLiveSeal(cipher,sequence,plain,wire))return -1;
    if(r->mode==1)wire[MAT_LIVE_HEADER]^=1; /* Authentication failure before Java sees data. */
    if(r->mode==8)return LiveCheckSend(wire,sizeof(wire))?0:-1;
    return MatLivePlaybackPush(playback,wire,sizeof(wire));
}
static void* worker(void* context) {
    Run* r=context;
    if((*r->vm)->AttachCurrentThread(r->vm,&r->env,NULL)!=JNI_OK)return NULL;
    uint8_t key[16]={0},carrier[MAT_WIRE_CYCLE],plain[MAT_WIRE_PACKET];
    LI_MAT_LIVE_PROFILE profile={r->epoch,7};
    LI_MAT_PLAYBACK_CALLBACKS callbacks={cycle,end,r};
    MAT_LIVE_PLAYBACK* playback=NULL;
    if(r->mode==8) {
        if(!LiveCheckOpen(&profile,key,&callbacks,2))goto done;
    } else playback=MatLivePlaybackOpen(&profile,key,&callbacks);
    MAT_LIVE_CIPHER cipher;
    if((!playback&&r->mode!=8)||!MatLiveCipherInit(&cipher,key,sizeof(key),r->mode==6?r->epoch-1:r->epoch,7))goto done;
    if(r->mode==8) {
        mat_wire_make(plain,MAT_WIRE_SYNC,r->epoch,7,0,0,0,NULL);
        for(uint64_t i=0;i<4;++i)if(send_packet(r,playback,&cipher,plain,i)<0)goto done;
    }
    uint64_t sequence=4;
    uint32_t hash=2166136261u;
    int total=r->mode==0||r->mode==8?128:1;
    for(int i=0;i<total;++i) {
        for(unsigned j=0;j<sizeof(carrier);++j)carrier[j]=(uint8_t)(j*7+i);
        memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
        uint32_t frame_hash=mat_hash(carrier,sizeof(carrier));
        for(unsigned fragment=0;fragment<MAT_WIRE_FRAGMENTS;++fragment) {
            mat_wire_make(plain,MAT_WIRE_DATA,r->epoch,7,i,fragment,frame_hash,carrier);
            r->result=send_packet(r,playback,&cipher,plain,sequence++);
            if(r->result<0)goto done;
        }
        hash=mat_hash_update(hash,carrier,sizeof(carrier));
    }
    if(r->mode!=2) {
        mat_wire_make(plain,MAT_WIRE_END,r->epoch,7,total,0,hash,NULL);
        for(int i=0;i<4;++i) {
            r->result=send_packet(r,playback,&cipher,plain,sequence++);
            if(r->result<0)break;
        }
    }
    if(r->mode==8&&r->result>=0)r->result=2;
done:
    if(r->mode==8)LiveCheckClose();else MatLivePlaybackClose(playback);
    (*r->vm)->DetachCurrentThread(r->vm);
    return NULL;
}
JNIEXPORT jint JNICALL Java_com_sunshine_matmock_MatInstrumentation_runNative(
        JNIEnv* env,jclass type,jobject sink,jlong epoch,jint mode) {
    (void)type;
    Run r={0};r.epoch=(uint64_t)epoch;r.mode=mode;r.result=-1;
    if((*env)->GetJavaVM(env,&r.vm)!=JNI_OK)return -1;
    r.sink=(*env)->NewGlobalRef(env,sink);if(!r.sink)return -1;
    jclass sink_class=(*env)->GetObjectClass(env,sink);
    r.cycle=(*env)->GetMethodID(env,sink_class,"onCycle","(Ljava/nio/ByteBuffer;JJJJ)Z");
    r.end=(*env)->GetMethodID(env,sink_class,"onEnd","(J)Z");
    (*env)->DeleteLocalRef(env,sink_class);
    pthread_t thread;
    if(r.cycle&&r.end&&pthread_create(&thread,NULL,worker,&r)==0)pthread_join(thread,NULL);
    (*env)->DeleteGlobalRef(env,r.sink);
    if(r.exception){(*env)->Throw(env,r.exception);(*env)->DeleteGlobalRef(env,r.exception);}
    return r.result;
}
JNIEXPORT jlongArray JNICALL Java_com_sunshine_matmock_MatInstrumentation_fecStatsNative(JNIEnv* env,jclass type) {
    (void)type;jlong values[]={(jlong)LiveCheckDrops(),(jlong)LiveCheckRecovered()};
    jlongArray result=(*env)->NewLongArray(env,2);if(result)(*env)->SetLongArrayRegion(env,result,0,2,values);return result;
}
