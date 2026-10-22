#include "MatDepacketizer.h"
#include "mat_wire.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static MAT_DEPACKETIZER state;
static uint8_t cycle[MAT_WIRE_CYCLE],packet[MAT_WIRE_PACKET];
static unsigned writes;static int io_fail;
static int write_cycle(void* context,const uint8_t* bytes,size_t n) {
    (void)context;CHECK(n==sizeof(cycle));CHECK(!memcmp(bytes,cycle,n));++writes;return !io_fail;
}
static void init(void) {writes=0;io_fail=0;MatDepacketizerInit(&state,write_cycle,NULL);}
static void make(unsigned i) {mat_wire_make(packet,MAT_WIRE_DATA,1,1,0,i,mat_hash(cycle,sizeof(cycle)),cycle);}
static void reseal(void) {mat_put32(packet+72,mat_packet_hash(packet));}
int main(void) {
    for(unsigned i=0;i<sizeof(cycle);i++)cycle[i]=(uint8_t)i;
    memcpy(cycle,"\x72\xf8\x1f\x4e\x16\0\x02\0",8);
    init();CHECK(!MatDepacketizerFinish(&state));
    // All 60 fragments, reverse order, duplicate each before completion.
    for(unsigned i=60;i-->0;) {make(i);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==(i?0:1));if(i)CHECK(!MatDepacketizerPush(&state,packet,sizeof(packet)));}
    CHECK(writes==1 && state.next_frame==1);CHECK(!MatDepacketizerFinish(&state));
    mat_wire_make(packet,MAT_WIRE_END,1,1,1,0,mat_hash(cycle,sizeof(cycle)),NULL);
    CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==2);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==2);
    CHECK(MatDepacketizerFinish(&state));make(0);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    // Header validation must precede all untrusted indexing/allocation.
    unsigned offsets[]={0,4,5,6,7,8,12,16,24,32,40,48,52,56,60,61,62,63,64,68,76};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++) {
        init();make(0);packet[offsets[i]]^=0xff;reseal();int r=MatDepacketizerPush(&state,packet,sizeof(packet));
        // epoch/domain are opaque IDs: nonzero changes on FIRST packet valid.
        // frame hash is validated only on completed frame.
        if(offsets[i]==16 || offsets[i]==24 || offsets[i]==68)CHECK(r==0);else CHECK(r<0);
    }
    init();make(0);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet)-1)<0);
    init();make(0);packet[100]^=1;CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    init();make(0);CHECK(!MatDepacketizerPush(&state,packet,sizeof(packet)));packet[100]^=1;reseal();CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    init();make(0);CHECK(!MatDepacketizerPush(&state,packet,sizeof(packet)));make(1);mat_put64(packet+16,2);reseal();CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    init();make(0);CHECK(!MatDepacketizerPush(&state,packet,sizeof(packet)));
    mat_wire_make(packet,MAT_WIRE_END,1,1,1,0,0,NULL);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);CHECK(!writes);
    init();io_fail=1;for(unsigned i=0;i<60;i++){make(i);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==(i==59?-1:0));}
    CHECK(!MatDepacketizerFinish(&state));
    init();CHECK(MatDepacketizerPush(&state,NULL,MAT_WIRE_PACKET)<0);
    init();make(0);mat_put64(packet+16,0);reseal();CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    init();for(unsigned i=0;i<60;i++){make(i);mat_put32(packet+68,123);reseal();CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==(i==59?-1:0));}CHECK(!writes);
    init();for(unsigned i=0;i<60;i++){make(i);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))==(i==59?1:0));}
    mat_wire_make(packet,MAT_WIRE_END,1,1,1,0,123,NULL);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    init();make(0);CHECK(!MatDepacketizerPush(&state,packet,sizeof(packet)));
    mat_wire_make(packet,MAT_WIRE_DATA,1,1,1,0,mat_hash(cycle,sizeof(cycle)),cycle);CHECK(MatDepacketizerPush(&state,packet,sizeof(packet))<0);
    printf("PASS reverse-order, duplicates, EOS, missing fragment/cycle, %zu header mutations, packet/frame/stream checksum, epoch, IO failure, null; stateBytes=%zu\n",sizeof(offsets)/sizeof(*offsets),sizeof(state));
    return 0;
}
