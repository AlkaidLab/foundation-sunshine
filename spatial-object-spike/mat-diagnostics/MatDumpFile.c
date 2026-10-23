#include "MatDumpFile.h"
#include "../../third-party/moonlight-common-c/src/MatReceiver.h"
#include "../../third-party/moonlight-common-c/src/MatPlayback.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#define mat_open _open
#define mat_fdopen _fdopen
#define mat_close _close
#else
#include <unistd.h>
#define mat_open open
#define mat_fdopen fdopen
#define mat_close close
#ifndef O_BINARY
#define O_BINARY 0
#endif
#endif
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
#include <psa/crypto.h>
#else
#include <openssl/evp.h>
#endif

struct MAT_DUMP_FILE {
    MAT_PLAYBACK_RECEIVER* receiver; /* Optional SMAT adapter for diagnostic tests. */
    LI_MAT_DUMP_PROFILE expected;
    FILE* file;
    char final_path[1024],partial_path[1040],status_path[1040],sha256[65];
    uint64_t cycles,epoch,domain;
    int failed,published,created,completeEOS;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_hash_operation_t digest;
#else
    EVP_MD_CTX* digest;
#endif
};
static FILE* exclusive(const char* path) {
    int fd=mat_open(path,O_WRONLY|O_CREAT|O_EXCL|O_BINARY,0600);
    if(fd<0)return NULL;
    FILE* f=mat_fdopen(fd,"wb");
    if(!f)mat_close(fd);
    return f;
}
static int write_cycle(void* context,const uint8_t* p,int bytes,uint64_t epoch,uint64_t domain,uint64_t index,uint64_t pts) {
    MAT_DUMP_FILE* s=context;
    size_t n=bytes>0?(size_t)bytes:0;
    if(s->failed||s->published||!epoch||!domain||index!=s->cycles||index>=s->expected.cycles||pts!=index*960||
       (index&&(epoch!=s->epoch||domain!=s->domain))||!MatCarrierIsValid(p,n)||!s->file||fwrite(p,1,n,s->file)!=n){s->failed=1;return 0;}
    s->epoch=epoch;s->domain=domain;++s->cycles;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    if(psa_hash_update(&s->digest,p,n)!=PSA_SUCCESS)s->failed=1;
#else
    if(EVP_DigestUpdate(s->digest,p,n)!=1)s->failed=1;
#endif
    return !s->failed;
}
static void status(MAT_DUMP_FILE* s) {
    FILE* f=exclusive(s->status_path);if(!f)return;
    fprintf(f,"{\"published\":%s,\"completeEOS\":%s,\"failed\":%s,\"cycles\":%llu,\"expectedCycles\":%u,\"sha256\":\"%s\",\"expectedSha256\":\"%s\"}\n",
        s->published?"true":"false",s->completeEOS?"true":"false",s->failed?"true":"false",
        (unsigned long long)s->cycles,s->expected.cycles,s->sha256,s->expected.sha256);fclose(f);
}
MAT_DUMP_FILE* MatDumpOpen(const char* path,const LI_MAT_DUMP_PROFILE* expected) {
    if(!path || !expected || !*path || strlen(path)>=1024)return NULL;
    char count[16];snprintf(count,sizeof(count),"%u",expected->cycles);LI_MAT_DUMP_PROFILE checked;
    if(!MatProfileSet(&checked,expected->sha256,count))return NULL;
    MAT_DUMP_FILE* s=calloc(1,sizeof(*s));if(!s)return NULL;s->expected=checked;
    memcpy(s->final_path,path,strlen(path)+1);snprintf(s->partial_path,sizeof(s->partial_path),"%s.partial",path);
    snprintf(s->status_path,sizeof(s->status_path),"%s.json",path);
    struct stat st;if(stat(path,&st)==0 || stat(s->status_path,&st)==0){free(s);return NULL;}
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    s->digest=psa_hash_operation_init();
    if(psa_crypto_init()!=PSA_SUCCESS || psa_hash_setup(&s->digest,PSA_ALG_SHA_256)!=PSA_SUCCESS){free(s);return NULL;}
#else
    s->digest=EVP_MD_CTX_new();
    if(!s->digest || EVP_DigestInit_ex(s->digest,EVP_sha256(),NULL)!=1){EVP_MD_CTX_free(s->digest);free(s);return NULL;}
#endif
    s->file=exclusive(s->partial_path);
    if(!s->file){MatDumpClose(s);return NULL;}
    s->created=1;return s;
}
void MatDumpFail(MAT_DUMP_FILE* s) {if(s)s->failed=1;}
int MatDumpPublished(const MAT_DUMP_FILE* s){return s && s->published;}
static int end_stream(void* context,uint64_t count) {
    MAT_DUMP_FILE* s=context;
    if(!s||s->failed)return 0;
    if(s->published)return count==s->cycles;
    if(!count||count!=s->cycles||count!=s->expected.cycles||!s->file){s->failed=1;return 0;}
    s->completeEOS=1;
    unsigned char bytes[32];size_t length=0;
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    if(psa_hash_finish(&s->digest,bytes,sizeof(bytes),&length)!=PSA_SUCCESS)s->failed=1;
#else
    unsigned out=0;if(EVP_DigestFinal_ex(s->digest,bytes,&out)!=1)s->failed=1;length=out;
#endif
    if(length!=32)s->failed=1;
    if(!s->failed)for(unsigned i=0;i<32;i++)snprintf(s->sha256+i*2,3,"%02x",bytes[i]);
    if(fflush(s->file))s->failed=1;
    if(fclose(s->file))s->failed=1;
    s->file=NULL;
    if(strcmp(s->sha256,s->expected.sha256))s->failed=1;
    // link() on POSIX refuses an existing destination, unlike rename(). Android
    // app-private files support same-directory links. Never overwrite a dump.
    if(!s->failed) {
#ifdef _WIN32
        if(rename(s->partial_path,s->final_path))s->failed=1;
#else
        if(link(s->partial_path,s->final_path))s->failed=1;else if(unlink(s->partial_path))s->failed=1;
#endif
    }
    s->published=!s->failed;status(s);return !s->failed;
}
LI_MAT_PLAYBACK_CALLBACKS MatDumpCallbacks(MAT_DUMP_FILE* s) {
    LI_MAT_PLAYBACK_CALLBACKS callbacks={write_cycle,end_stream,s};return callbacks;
}
int MatDumpPush(MAT_DUMP_FILE* s,const unsigned char* p,size_t n) {
    if(!s||s->failed)return -1;
    if(!s->receiver){LI_MAT_PLAYBACK_CALLBACKS callbacks=MatDumpCallbacks(s);s->receiver=MatPlaybackOpen(&s->expected,&callbacks);}
    int result=s->receiver?MatPlaybackPush(s->receiver,p,n):-1;
    if(result<0)s->failed=1;
    return result;
}
void MatDumpClose(MAT_DUMP_FILE* s) {
    if(!s)return;
    MatPlaybackClose(s->receiver);
    if(s->file){if(fclose(s->file))s->failed=1;s->file=NULL;}
    if(!s->published && s->created){s->failed=1;status(s);}
#if defined(USE_MBEDTLS) || defined(USE_PSA_CRYPTO)
    psa_hash_abort(&s->digest);
#else
    EVP_MD_CTX_free(s->digest);
#endif
    free(s);
}
