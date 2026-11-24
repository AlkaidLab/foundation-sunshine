/* Generated software fixture only. Not a native capture or hardware proof. */
#include "../../tools/mat-live/live_check_transport.h"
#include "../../third-party/moonlight-common-c/src/MatLiveCrypto.h"
#include <stdio.h>
#include <string.h>
static uint64_t received,ended;
static int cycle(void* context,const uint8_t* bytes,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts){
    (void)context;if(n!=61440||e!=123||d!=7||i!=received||pts!=i*960||bytes[8]!=(uint8_t)i)return 0;++received;return 1;
}
static int end(void* context,uint64_t n){(void)context;if(n!=received)return 0;++ended;return 1;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL software transport line %d\n",__LINE__);LiveCheckClose();return 1;}}while(0)
int main(void){for(int loss=0;loss<2;++loss){
    LI_MAT_LIVE_PROFILE profile={123,7};uint8_t key[16]={0};LI_MAT_PLAYBACK_CALLBACKS cb={cycle,end,NULL};received=ended=0;
    CHECK(LiveCheckOpen(&profile,key,&cb,loss));MAT_LIVE_CIPHER cipher;CHECK(MatLiveCipherInit(&cipher,key,16,123,7));
    uint8_t plain[MAT_WIRE_PACKET],wire[MAT_LIVE_PACKET],carrier[MAT_WIRE_CYCLE]={0};uint64_t packet=0;
    memcpy(carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);mat_wire_make(plain,MAT_WIRE_SYNC,123,7,0,0,0,NULL);
    for(unsigned i=0;i<4;++i){CHECK(MatLiveSeal(&cipher,packet++,plain,wire));CHECK(LiveCheckSend(wire,sizeof(wire)));}
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<1300;++i){carrier[8]=(uint8_t)i;uint32_t h=mat_hash(carrier,sizeof(carrier));
        for(unsigned j=0;j<60;++j){mat_wire_make(plain,MAT_WIRE_DATA,123,7,i,j,h,carrier);CHECK(MatLiveSeal(&cipher,packet++,plain,wire));CHECK(LiveCheckSend(wire,sizeof(wire)));}
        hash=mat_hash_update(hash,carrier,sizeof(carrier));
    }
    mat_wire_make(plain,MAT_WIRE_END,123,7,1300,0,hash,NULL);
    for(unsigned i=0;i<4;++i){CHECK(MatLiveSeal(&cipher,packet++,plain,wire));CHECK(LiveCheckSend(wire,sizeof(wire)));}
    CHECK(received==1300&&ended==1&&LiveCheckDrops()==LiveCheckRecovered());CHECK(loss?LiveCheckDrops()>0:LiveCheckDrops()==0);
    printf("PASS_SOFTWARE_LOOPBACK generatedCycles=%llu datagrams=%llu drops=%llu recovered=%llu nativeCapture=0\n",
        (unsigned long long)received,(unsigned long long)LiveCheckDatagrams(),(unsigned long long)LiveCheckDrops(),(unsigned long long)LiveCheckRecovered());
    LiveCheckClose();
}return 0;}
