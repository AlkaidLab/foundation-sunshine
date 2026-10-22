#include "MatPlayback.h"
#include "MatWire.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned offers,ends,reject_cycle,reject_end;
static int consume(void* context,const unsigned char* b,int n,uint64_t e,uint64_t d,uint64_t i,uint64_t pts){
    (void)context;(void)b;
    if(n!=61440 || e!=1 || d!=1 || i!=offers || pts!=i*960)return 0;
    offers++;return !reject_cycle || offers<3;
}
static int complete(void* context,uint64_t count){(void)context;ends++;return count==178 && !reject_end;}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char** argv){
    REQUIRE(argc==2);FILE* f=fopen(argv[1],"rb");REQUIRE(f);
    unsigned char* bytes=malloc(10936320);REQUIRE(bytes);
    REQUIRE(fread(bytes,1,10936320,f)==10936320 && fgetc(f)==EOF);fclose(f);
    const char* sha="79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6";
    for(unsigned mode=0;mode<6;mode++){
        LI_MAT_DUMP_PROFILE profile;REQUIRE(MatProfileSet(&profile,sha,mode==2?"177":"178"));
        if(mode==1)profile.sha256[0]='0';
        LI_MAT_PLAYBACK_CALLBACKS callbacks={consume,complete,NULL};
        REQUIRE(!MatPlaybackOpen(&profile,NULL));
        MAT_PLAYBACK_RECEIVER* r=MatPlaybackOpen(&profile,&callbacks);REQUIRE(r);
        offers=ends=0;reject_cycle=mode==3;reject_end=mode==4;
        uint8_t packet[MAT_WIRE_PACKET];uint32_t hash=2166136261u;int result=0;
        for(unsigned frame=0;frame<178 && result>=0;frame++){
            const uint8_t* cycle=bytes+frame*61440;
            hash=mat_hash_update(hash,cycle,61440);
            for(unsigned fragment=0;fragment<60 && result>=0;fragment++){
                mat_wire_make(packet,MAT_WIRE_DATA,1,1,frame,fragment,mat_hash(cycle,61440),cycle);
                result=MatPlaybackPush(r,packet,sizeof(packet));
            }
        }
        if(mode!=5 && result>=0){
            mat_wire_make(packet,MAT_WIRE_END,1,1,178,0,hash,NULL);
            result=MatPlaybackPush(r,packet,sizeof(packet));
        }
        if(mode==0){REQUIRE(result==2 && offers==178 && ends==1);REQUIRE(MatPlaybackPush(r,packet,sizeof(packet))==2 && ends==1);}
        else if(mode==5){REQUIRE(ends==0);}
        else{REQUIRE(result<0);REQUIRE(MatPlaybackPush(r,packet,sizeof(packet))<0);REQUIRE(ends==(mode==4?1u:0u));}
        MatPlaybackClose(r);
    }
    free(bytes);puts("PASS six playback receiver lifecycle cases: success/manifestSHA/count/consumer/end/missingEOS");return 0;
}
