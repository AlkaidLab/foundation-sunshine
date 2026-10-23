#pragma once
#include "../../third-party/moonlight-common-c/src/MatReceiver.h"
#include "../../third-party/moonlight-common-c/src/MatWire.h"
/* Diagnostic raw-cycle input only. Never participates in network authentication
 * or a production AudioStream. State is independent of MAT_RECEIVER. */
typedef struct {
    MAT_CONSUME_CYCLE consume;
    MAT_COMPLETE_STREAM complete;
    void* context;
    uint64_t epoch,domain,cycles;
    int failed,ended;
} MAT_RECORDING_RECEIVER;
static inline void MatRecordingInit(MAT_RECORDING_RECEIVER* s,MAT_CONSUME_CYCLE consume,MAT_COMPLETE_STREAM complete,void* context) {
    memset(s,0,sizeof(*s));s->consume=consume;s->complete=complete;s->context=context;
}
static inline int MatRecordingFail(MAT_RECORDING_RECEIVER* s){s->failed=1;return -1;}
static inline int MatRecordingPush(MAT_RECORDING_RECEIVER* s,const MAT_CYCLE_INFO* i,const uint8_t* p,size_t n) {
    if(s->failed||s->ended||!i||!i->epoch||!i->domain||i->index!=s->cycles||i->index>=UINT64_MAX/960||
       i->pts48k!=i->index*960||!MatCarrierIsValid(p,n))return MatRecordingFail(s);
    if(s->cycles&&(i->epoch!=s->epoch||i->domain!=s->domain))return MatRecordingFail(s);
    if(!s->consume||!s->consume(s->context,i,p,n))return MatRecordingFail(s);
    s->epoch=i->epoch;s->domain=i->domain;++s->cycles;return 1;
}
static inline int MatRecordingEnd(MAT_RECORDING_RECEIVER* s,uint64_t count) {
    if(s->failed||s->ended||!count||count!=s->cycles)return MatRecordingFail(s);
    if(s->complete&&!s->complete(s->context,count))return MatRecordingFail(s);
    s->ended=1;return 2;
}
