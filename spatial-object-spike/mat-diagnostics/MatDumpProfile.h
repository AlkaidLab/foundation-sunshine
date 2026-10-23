#pragma once
#include "../../third-party/moonlight-common-c/src/MatProfile.h"
typedef struct { char sha256[65]; uint32_t cycles; } LI_MAT_DUMP_PROFILE;
static inline int MatProfileSet(LI_MAT_DUMP_PROFILE* out,const char* hash,const char* count) {
    memset(out,0,sizeof(*out));
    if(!hash || !count || strlen(hash)!=64 || !*count)return 0;
    for(unsigned i=0;i<64;i++)if(!((hash[i]>='0'&&hash[i]<='9') || (hash[i]>='a'&&hash[i]<='f')))return 0;
    unsigned n=0;for(const char* p=count;*p;p++){if(*p<'0'||*p>'9'||n>1000)return 0;n=n*10+(unsigned)(*p-'0');}
    if(!n || n>1000)return 0;
    memcpy(out->sha256,hash,65);out->cycles=n;return 1;
}
