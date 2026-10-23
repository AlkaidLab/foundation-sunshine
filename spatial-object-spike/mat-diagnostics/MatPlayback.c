#include "MatPlayback.h"
#include "../../third-party/moonlight-common-c/src/MatReceiver.h"
#include <stdlib.h>
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
#include <psa/crypto.h>
#else
#include <openssl/evp.h>
#endif
struct MAT_PLAYBACK_RECEIVER {
    MAT_RECEIVER receiver;
    LI_MAT_DUMP_PROFILE expected;
    LI_MAT_PLAYBACK_CALLBACKS callbacks;
    int failed;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_hash_operation_t digest;
#else
    EVP_MD_CTX* digest;
#endif
};
static int cycle(void* context,const MAT_CYCLE_INFO* info,const uint8_t* p,size_t n) {
    MAT_PLAYBACK_RECEIVER* s=context;
    if(info->index>=s->expected.cycles) return 0;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    if(psa_hash_update(&s->digest,p,n)!=PSA_SUCCESS) return 0;
#else
    if(EVP_DigestUpdate(s->digest,p,n)!=1) return 0;
#endif
    return s->callbacks.cycle(s->callbacks.context,p,(int)n,info->epoch,info->domain,info->index,info->pts48k);
}
static int end(void* context,uint64_t count) {
    MAT_PLAYBACK_RECEIVER* s=context;
    unsigned char bytes[32];size_t length=0;char hash[65]={0};
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    if(psa_hash_finish(&s->digest,bytes,sizeof(bytes),&length)!=PSA_SUCCESS) return 0;
#else
    unsigned out=0;
    if(EVP_DigestFinal_ex(s->digest,bytes,&out)!=1) return 0;
    length=out;
#endif
    if(length!=32 || count!=s->expected.cycles) return 0;
    const char* hex="0123456789abcdef";
    for(unsigned i=0;i<32;i++){hash[2*i]=hex[bytes[i]>>4];hash[2*i+1]=hex[bytes[i]&15];}
    if(strcmp(hash,s->expected.sha256)) return 0;
    return s->callbacks.end(s->callbacks.context,count);
}
MAT_PLAYBACK_RECEIVER* MatPlaybackOpen(const LI_MAT_DUMP_PROFILE* expected,const LI_MAT_PLAYBACK_CALLBACKS* callbacks) {
    if(!expected || !callbacks || !callbacks->cycle || !callbacks->end) return NULL;
    char count[16];LI_MAT_DUMP_PROFILE checked;
    /* Avoid stdio formatting in the reception hot path. */
    uint32_t value=expected->cycles;unsigned n=0;
    if(!value || value>1000) return NULL;
    do {count[n++]=(char)('0'+value%10);value/=10;} while(value);
    count[n]=0;for(unsigned i=0;i<n/2;i++){char t=count[i];count[i]=count[n-i-1];count[n-i-1]=t;}
    if(!MatProfileSet(&checked,expected->sha256,count)) return NULL;
    MAT_PLAYBACK_RECEIVER* s=calloc(1,sizeof(*s));if(!s)return NULL;
    s->expected=checked;s->callbacks=*callbacks;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    s->digest=psa_hash_operation_init();
    if(psa_crypto_init()!=PSA_SUCCESS || psa_hash_setup(&s->digest,PSA_ALG_SHA_256)!=PSA_SUCCESS){free(s);return NULL;}
#else
    s->digest=EVP_MD_CTX_create();
    if(!s->digest || EVP_DigestInit_ex(s->digest,EVP_sha256(),NULL)!=1){EVP_MD_CTX_destroy(s->digest);free(s);return NULL;}
#endif
    MatReceiverInit(&s->receiver,cycle,end,s);return s;
}
int MatPlaybackPush(MAT_PLAYBACK_RECEIVER* s,const unsigned char* p,size_t n) {
    if(!s || s->failed)return -1;
    int result=MatReceiverPush(&s->receiver,p,n);
    if(result<0)s->failed=1;
    return result;
}
void MatPlaybackFail(MAT_PLAYBACK_RECEIVER* s) {if(s){s->failed=1;MatReceiverAbort(&s->receiver);}}
void MatPlaybackClose(MAT_PLAYBACK_RECEIVER* s) {
    if(!s)return;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_hash_abort(&s->digest);
#else
    EVP_MD_CTX_destroy(s->digest);
#endif
    free(s);
}
