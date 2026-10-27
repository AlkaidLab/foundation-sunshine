/* Generated carrier -> actual authenticated receiver -> simulated output.
 * The optional UDP build also uses the existing production RTP/FEC adapter. */
#include "mock_output.h"
#include "MatLivePlayback.h"
#include "MatLiveCrypto.h"
#include <stdio.h>
#ifdef MOCK_USE_UDP
#include "../../tools/mat-live/live_check_transport.h"
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"mock output line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct {
    MockDevice device;MockOutput* output;MAT_LIVE_PLAYBACK* receiver;
    LI_MAT_LIVE_PROFILE profile;MAT_LIVE_CIPHER cipher;EVP_MD_CTX* expected;
    uint64_t now,packet,cycles,end_enter,end_return;uint32_t hash;
    uint8_t carrier[MOCK_CYCLE_BYTES],first_wire[MAT_LIVE_PACKET];
} Fixture;
static int cycle(void* context,const uint8_t* bytes,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts){
    Fixture* f=(Fixture*)context;return mock_output_offer(f->output,bytes,n,e,d,i,pts,f->now);
}
static MockOutputStats poll(Fixture* f,uint64_t now);
static int end(void* context,uint64_t count){
    Fixture* f=(Fixture*)context;if(!mock_output_end(f->output,count,f->now))return 0;
    f->end_enter=f->now;MockOutputStats s=poll(f,f->now);
    for(unsigned i=0;i<3000&&s.state==MOCK_DRAINING;++i)s=poll(f,f->now+5);
    f->end_return=f->now;return s.state==MOCK_COMPLETED;
}
static void open_fixture(Fixture* f,uint64_t epoch,unsigned stall,unsigned drain){
    memset(f,0,sizeof(*f));f->profile=(LI_MAT_LIVE_PROFILE){epoch,7};f->hash=2166136261u;
    MockCapabilities caps={1,1,1,1};CHECK(mock_device_init(&f->device,caps,0));
    f->output=mock_output_open(mock_device_sink(&f->device),1,epoch,7,0,stall,drain);CHECK(f->output);
    uint8_t key[16]={0};CHECK(MatLiveCipherInit(&f->cipher,key,16,epoch,7));
    LI_MAT_PLAYBACK_CALLBACKS cb={cycle,end,f};
#ifdef MOCK_USE_UDP
    CHECK(LiveCheckOpen(&f->profile,key,&cb,1));
#else
    f->receiver=MatLivePlaybackOpen(&f->profile,key,&cb);CHECK(f->receiver);
#endif
    f->expected=EVP_MD_CTX_create();CHECK(f->expected&&EVP_DigestInit_ex(f->expected,EVP_sha256(),NULL));
}
static int push_wire(Fixture* f,const uint8_t* wire){
#ifdef MOCK_USE_UDP
    (void)f;return LiveCheckSend(wire,MAT_LIVE_PACKET);
#else
    return MatLivePlaybackPush(f->receiver,wire,MAT_LIVE_PACKET)>=0;
#endif
}
static int packet(Fixture* f,const uint8_t* plain){
    uint8_t wire[MAT_LIVE_PACKET];uint64_t index=f->packet++;
    CHECK(MatLiveSeal(&f->cipher,index,plain,wire));if(index==4)memcpy(f->first_wire,wire,sizeof(wire));
#ifndef MOCK_USE_UDP
    if(index<4)return 1; /* AudioStream handles the four transport SYNC packets. */
#endif
    return push_wire(f,wire);
}
static void sync_fixture(Fixture* f){uint8_t p[MAT_WIRE_PACKET];mat_wire_make(p,MAT_WIRE_SYNC,f->profile.epoch,7,0,0,0,NULL);for(int i=0;i<4;++i)CHECK(packet(f,p));}
static int frame(Fixture* f){
    for(unsigned n=0;n<sizeof(f->carrier);++n)f->carrier[n]=(uint8_t)(n*7+f->cycles);
    memcpy(f->carrier,"\x72\xf8\x1f\x4e\x16\x00\x02\x00",8);
    uint8_t p[MAT_WIRE_PACKET];uint32_t hash=mat_hash(f->carrier,sizeof(f->carrier));
    for(unsigned n=0;n<60;++n){mat_wire_make(p,MAT_WIRE_DATA,f->profile.epoch,7,f->cycles,n,hash,f->carrier);if(!packet(f,p))return 0;}
    f->hash=mat_hash_update(f->hash,f->carrier,sizeof(f->carrier));CHECK(EVP_DigestUpdate(f->expected,f->carrier,sizeof(f->carrier)));++f->cycles;return 1;
}
static int eos(Fixture* f){uint8_t p[MAT_WIRE_PACKET];mat_wire_make(p,MAT_WIRE_END,f->profile.epoch,7,f->cycles,0,f->hash,NULL);for(int i=0;i<4;++i)if(!packet(f,p))return 0;return 1;}
static MockOutputStats poll(Fixture* f,uint64_t now){f->now=now;mock_device_tick(&f->device,now);return mock_output_poll(f->output,now);}
static void complete(Fixture* f){
    MockOutputStats s=poll(f,f->now);for(unsigned i=0;i<1000&&s.state==MOCK_DRAINING;++i)s=poll(f,f->now+5);
    CHECK(s.state==MOCK_COMPLETED&&s.accepted==f->cycles&&s.written==f->cycles);
    CHECK(s.submitted_bytes==f->cycles*MOCK_CYCLE_BYTES&&s.played_frames*MOCK_FRAME_BYTES==s.submitted_bytes);
    char expected[65];mock_hex_digest(f->expected,expected);
    CHECK(!strcmp(expected,f->device.written_sha)&&!strcmp(expected,f->device.played_sha));
    CHECK(s.peak_backlog_bytes<=6*MOCK_CYCLE_BYTES&&f->device.closed);
}
static void close_fixture(Fixture* f){
#ifdef MOCK_USE_UDP
    LiveCheckClose();
#else
    MatLivePlaybackClose(f->receiver);
#endif
    mock_output_free(f->output);EVP_MD_CTX_destroy(f->expected);
}
int main(void){
    Fixture* f=(Fixture*)calloc(1,sizeof(*f));CHECK(f);
    MockCapabilities caps={1,1,1,1};CHECK(mock_preferred_codec(caps,0)==6&&mock_preferred_codec(caps,1)==7);
    for(unsigned n=0;n<4;++n){MockCapabilities bad=caps;if(n==0)bad.route=0;if(n==1)bad.hdmi=0;if(n==2)bad.mat=0;if(n==3)bad.direct_iec=0;
        CHECK(mock_preferred_codec(bad,0)==0&&mock_preferred_codec(bad,1)==-1);}
    puts("PASS mock capability gate: AUTO/strict MAT, unsupported routes choose Opus/reject");

    open_fixture(f,123,1000,3000);sync_fixture(f);f->device.max_write=4096;f->device.zero_every=11;
#ifdef MOCK_USE_UDP
    unsigned total=1300;
#else
    unsigned total=500;
#endif
    unsigned emitted=0;for(uint64_t now=0;emitted<total;now+=5){CHECK(poll(f,now).state==MOCK_RUNNING);
        /* Some complete cycles arrive 10ms late; packet bytes still use real authentication. */
        if(now>=(uint64_t)emitted*20+(emitted%10==5?10:0)){CHECK(frame(f));++emitted;}}
    CHECK(eos(f)&&f->end_return>f->end_enter);complete(f);
    printf("PASS paced mock sink: cycles=%u, short/zero writes, arrival jitter, exact written/played SHA, bounded backlog",total);
#ifdef MOCK_USE_UDP
    CHECK(LiveCheckDrops()>0&&LiveCheckDrops()==LiveCheckRecovered());printf(", UDP/FEC recovered=%llu",(unsigned long long)LiveCheckRecovered());
#endif
    puts("");close_fixture(f);

    open_fixture(f,124,1000,3000);sync_fixture(f);CHECK(frame(f)&&frame(f));memset(f->carrier,0,sizeof(f->carrier));CHECK(eos(f)&&f->end_return>f->end_enter);complete(f);close_fixture(f);
    puts("PASS owned cycle buffers survive producer reuse; EOS waits for the consumed tail");

    open_fixture(f,125,1000,3000);sync_fixture(f);f->device.busy=1;
    for(unsigned n=0;n<MOCK_QUEUE_CYCLES;++n)CHECK(frame(f));
    CHECK(!frame(f));CHECK(poll(f,0).state==MOCK_FAILED);close_fixture(f);
    puts("PASS slow consumer overflow terminates at four queued cycles without dropping data");

    open_fixture(f,126,1000,3000);sync_fixture(f);f->device.busy=1;CHECK(frame(f));CHECK(!eos(f));
    CHECK(poll(f,f->now).state==MOCK_FAILED);close_fixture(f);
    open_fixture(f,127,1000,3000);sync_fixture(f);f->device.fail_after=272;CHECK(frame(f));CHECK(!eos(f));
    CHECK(poll(f,f->now).state==MOCK_FAILED&&f->device.written==272&&f->device.played==0);close_fixture(f);
    open_fixture(f,128,1000,3000);sync_fixture(f);f->device.invalid_write=1;CHECK(frame(f));
    CHECK(poll(f,0).state==MOCK_FAILED);close_fixture(f);
    puts("PASS zero-write timeout, error following a partial write, and misaligned write rejection");

    open_fixture(f,129,1000,3000);sync_fixture(f);f->device.speed_percent=0;CHECK(frame(f));CHECK(!eos(f));
    CHECK(poll(f,f->now).state==MOCK_FAILED);close_fixture(f);
    open_fixture(f,130,10000,100);sync_fixture(f);f->device.speed_percent=0;CHECK(frame(f));CHECK(!eos(f));
    CHECK(poll(f,f->now).state==MOCK_FAILED);close_fixture(f);
    puts("PASS write completion is not playback completion; stalled playhead and EOS drain fail");

    open_fixture(f,131,1000,3000);sync_fixture(f);CHECK(frame(f));uint8_t stale[MAT_LIVE_PACKET];memcpy(stale,f->first_wire,sizeof(stale));
    CHECK(poll(f,0).state==MOCK_RUNNING);f->device.capabilities.route=2;CHECK(poll(f,1).state==MOCK_FAILED);
    f->device.capabilities.route=1;CHECK(poll(f,2).state==MOCK_FAILED);close_fixture(f);
    open_fixture(f,132,1000,3000);sync_fixture(f);CHECK(!push_wire(f,stale));CHECK(poll(f,0).accepted==0);close_fixture(f);
    open_fixture(f,133,1000,3000);sync_fixture(f);CHECK(frame(f));CHECK(eos(f));complete(f);close_fixture(f);
    puts("PASS route loss stays terminal; reconnect rejects stale ciphertext and starts with a fresh output");

    for(unsigned n=0;n<3;++n){open_fixture(f,140+n,1000,3000);sync_fixture(f);CHECK(frame(f));CHECK(poll(f,0).state==MOCK_RUNNING);
        if(n==0)f->device.capabilities.hdmi=0;
        if(n==1)f->device.capabilities.mat=0;
        if(n==2)f->device.capabilities.direct_iec=0;
        CHECK(poll(f,1).state==MOCK_FAILED);close_fixture(f);}
    puts("PASS capability loss on an unchanged route is terminal");

    open_fixture(f,143,1000,3000);sync_fixture(f);f->device.speed_percent=50;int rejected=0;
    for(uint64_t now=0;now<2000&&!rejected;now+=5){CHECK(poll(f,now).state==MOCK_RUNNING);if(now%20==0)rejected=!frame(f);}
    MockOutputStats slow=poll(f,f->now);CHECK(rejected&&slow.state==MOCK_FAILED&&slow.queued<=MOCK_QUEUE_CYCLES&&slow.peak_backlog_bytes<=6*MOCK_CYCLE_BYTES);close_fixture(f);
    puts("PASS sustained device clock mismatch remains bounded and fails without resampling or frame loss");

    open_fixture(f,144,1000,3000);sync_fixture(f);CHECK(frame(f));CHECK(poll(f,20).state==MOCK_RUNNING);
    CHECK(poll(f,19).state==MOCK_FAILED);close_fixture(f);
    puts("PASS a reversed playback polling clock cannot complete a session");

    open_fixture(f,134,1000,3000);sync_fixture(f);CHECK(frame(f));CHECK(poll(f,0).state==MOCK_RUNNING);
    CHECK(poll(f,20).state==MOCK_RUNNING);mock_output_stop(f->output);CHECK(poll(f,21).state==MOCK_STOPPED);close_fixture(f);
    puts("PASS absent authenticated END and cancellation never report completion");
    free(f);puts("PASS mock client output; hardwareSink=0 nativeCapture=0 atmosDecoded=0");return 0;
}
