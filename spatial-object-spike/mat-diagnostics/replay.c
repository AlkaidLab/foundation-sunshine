/* Offline diagnostic: validate a raw recording and deliver to a mock consumer.
 * No network source, native encoder or hardware output is opened. */
#include "recording_receiver.h"
#include <stdio.h>
static uint64_t cycles;
static int consume(void* context,const MAT_CYCLE_INFO* info,const uint8_t* p,size_t n) {
    (void)context;(void)p;(void)n;if(info->index!=cycles)return 0;++cycles;return 1;
}
static int end(void* context,uint64_t count){(void)context;return count==cycles;}
int main(int argc,char** argv) {
    if(argc!=2){fputs("mat-recording-check recording.raw (mock consumer only)\n",stderr);return 2;}
    FILE* file=fopen(argv[1],"rb");if(!file)return 2;
    MAT_RECORDING_RECEIVER receiver;MatRecordingInit(&receiver,consume,end,NULL);
    uint8_t bytes[MAT_WIRE_CYCLE];size_t n;int okay=1;
    while((n=fread(bytes,1,sizeof(bytes),file))!=0) {
        MAT_CYCLE_INFO info={1,1,cycles,cycles*960};
        if(MatRecordingPush(&receiver,&info,bytes,n)<0){okay=0;break;}
    }
    if(ferror(file))okay=0;
    fclose(file);
    if(okay)okay=MatRecordingEnd(&receiver,cycles)==2;
    printf("result=%s cycles=%llu consumer=mock nativeCapture=0 hardwareOutput=0\n",okay?"PASS":"FAIL",(unsigned long long)cycles);
    return okay?0:1;
}
