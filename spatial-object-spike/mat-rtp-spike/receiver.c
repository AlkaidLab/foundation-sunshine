// Native Moonlight dump frontend, NOT a replacement RTP/FEC/AES implementation.
// Actual RtpAudioQueue.c + PlatformCrypto.c -> portable MatDepacketizer -> file.
#include "Limelight-internal.h"
#include "MatDepacketizer.h"
#include "mat_wire.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <inttypes.h>
#include <ctype.h>

int AppVersionQuad[4]={7,1,500,0};
int AudioPacketDuration=1; // Transport tick only, carrier PTS is inside SMAT.
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static RTP_AUDIO_QUEUE queue;
static PPLT_CRYPTO_CONTEXT crypto;
static MAT_DEPACKETIZER depacketizer;
static EVP_MD_CTX* digest;
static FILE* output;
static unsigned encrypted,errors,concealed,delivered;
static uint16_t expected_sequence=4;
uint64_t PltGetMicroseconds(void) {
    LARGE_INTEGER v,f;QueryPerformanceCounter(&v);QueryPerformanceFrequency(&f);
    return (uint64_t)(v.QuadPart/f.QuadPart)*1000000+(uint64_t)(v.QuadPart%f.QuadPart)*1000000/f.QuadPart;
}
static void log_message(const char* fmt,...) {va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);}
static int write_cycle(void* unused,const uint8_t* bytes,size_t n) {
    (void)unused;
    return fwrite(bytes,1,n,output)==n && EVP_DigestUpdate(digest,bytes,n)==1;
}
static void deliver(PRTP_PACKET packet,int length) {
    if(!length) {++concealed;++errors;return;}
    if(packet->sequenceNumber!=expected_sequence || packet->timestamp!=packet->sequenceNumber)++errors;
    expected_sequence=packet->sequenceNumber+1;
    uint8_t plaintext[MAT_WIRE_PACKET+32];uint8_t* data=(uint8_t*)(packet+1);
    int size=length-sizeof(*packet);
    if(encrypted) {
        uint8_t key[16],iv[16]={0};for(int i=0;i<16;i++)key[i]=(uint8_t)i;
        uint32_t value=htonl(0x10203040u+packet->sequenceNumber);memcpy(iv,&value,4);
        if(size!=MAT_WIRE_PACKET+16 || !PltDecryptMessage(crypto,ALGORITHM_AES_CBC,
                CIPHER_FLAG_RESET_IV|CIPHER_FLAG_FINISH,key,16,iv,16,NULL,0,data,size,plaintext,&size)) {++errors;return;}
        data=plaintext;
    }
    if(MatDepacketizerPush(&depacketizer,data,(size_t)size)<0)++errors;
    ++delivered;
}
static void accept_packet(uint8_t* bytes,int length) {
    PRTP_PACKET p=(PRTP_PACKET)bytes;int status=RtpaAddPacket(&queue,p,(uint16_t)length);
    if(RTPQ_HANDLE_NOW(status))deliver(p,length);
    if(RTPQ_PACKET_READY(status)) {
        uint16_t size;PRTP_PACKET next;
        while((next=RtpaGetQueuedPacket(&queue,0,&size))!=NULL) {deliver(next,size);free(next);}
    }
}
int main(int argc,char** argv) {
    // final.raw stats.json expected-cycles trusted-sha256 plain|aes fault
    if(argc!=7)return 2;
    int count=atoi(argv[3]);const char* expected_hash=argv[4];const char* fault=argv[6];
    if(count<1 || count>1000 || strlen(expected_hash)!=64 ||
        (strcmp(argv[5],"plain") && strcmp(argv[5],"aes")))return 2;
    for(unsigned i=0;i<64;i++)if(!isxdigit((unsigned char)expected_hash[i]))return 2;
    const char* faults[]={"none","reorder","duplicate","drop1","drop2","drop3","dropframe","noend","corrupt"};
    int valid=0;for(unsigned i=0;i<sizeof(faults)/sizeof(*faults);i++)if(!strcmp(fault,faults[i]))valid=1;
    if(!valid)return 2;
    char partial[4096];if(snprintf(partial,sizeof(partial),"%s.partial",argv[1])>=(int)sizeof(partial))return 2;
    FILE* exists=fopen(argv[1],"rb");if(exists){fclose(exists);return 3;}
    output=fopen(partial,"wbx");FILE* summary=fopen(argv[2],"wx");if(!output || !summary)return 3;
    encrypted=!strcmp(argv[5],"aes");digest=EVP_MD_CTX_new();
    if(!digest || EVP_DigestInit_ex(digest,EVP_sha256(),NULL)!=1)return 4;
    MatDepacketizerInit(&depacketizer,write_cycle,NULL);
    ListenerCallbacks.logMessage=log_message;crypto=PltCreateCryptoContext();RtpaInitializeQueue(&queue);
    WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa))return 4;
    SOCKET sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(sock,(struct sockaddr*)&addr,sizeof(addr)))return 4;
    int addrlen=sizeof(addr);getsockname(sock,(struct sockaddr*)&addr,&addrlen);
    DWORD timeout=250;int capacity=4*1024*1024;
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout));
    setsockopt(sock,SOL_SOCKET,SO_RCVBUF,(const char*)&capacity,sizeof(capacity));
    printf("READY %u\n",ntohs(addr.sin_port));fflush(stdout);
    uint8_t wire[4096],held[4096];int held_length=0;
    unsigned wire_packets=0,drops=0,duplicates=0,max_datagram=0;
    unsigned transport_packets=4+(unsigned)count*60+4,wire_expected=transport_packets/4*6;
    uint64_t deadline=PltGetMicroseconds()+(uint64_t)count*20000+5000000;
    const uint64_t start=PltGetMicroseconds();
    while(wire_packets<wire_expected && PltGetMicroseconds()<deadline) {
        int length=recv(sock,(char*)wire,sizeof(wire),0);
        if(length<0) {if(WSAGetLastError()!=WSAETIMEDOUT){++errors;break;}continue;}
        ++wire_packets;if((unsigned)length>max_datagram)max_datagram=length;
        if(length<(int)sizeof(RTP_PACKET) || length>1200){++errors;continue;}
        PRTP_PACKET p=(PRTP_PACKET)wire;p->sequenceNumber=ntohs(p->sequenceNumber);
        p->timestamp=ntohl(p->timestamp);p->ssrc=ntohl(p->ssrc);
        unsigned seq=p->sequenceNumber;
        unsigned base=p->packetType==127 && length>=24?ntohs(((PAUDIO_FEC_HEADER)(p+1))->baseSequenceNumber):seq&~3u;
        if((!strcmp(fault,"dropframe") && base>=64 && base<124) ||
            (!strcmp(fault,"noend") && base>=transport_packets-4)) {++drops;continue;}
        if(p->packetType==97 && seq>=4 && seq<transport_packets-4) {
            if((!strcmp(fault,"drop1") && seq%4==1) ||
                (!strcmp(fault,"drop2") && (seq%4==1 || seq%4==2)) ||
                (!strcmp(fault,"drop3") && base==64 && seq%4<3)) {++drops;continue;}
            if(!strcmp(fault,"reorder") && seq%4==1) {memcpy(held,wire,length);held_length=length;continue;}
            if(!strcmp(fault,"corrupt") && seq==64)wire[length-1]^=1;
        }
        // RtpaAddPacket may modify packet fields; retain an original duplicate.
        uint8_t duplicate[4096];if(!strcmp(fault,"duplicate"))memcpy(duplicate,wire,length);
        accept_packet(wire,length);
        if(!strcmp(fault,"duplicate")){accept_packet(duplicate,length);++duplicates;}
        if(held_length && p->packetType==97 && seq%4==2){accept_packet(held,held_length);held_length=0;}
    }
    if(held_length)accept_packet(held,held_length);
    uint8_t hash[32];unsigned hash_length=0;char hex[65];
    if(EVP_DigestFinal_ex(digest,hash,&hash_length)!=1 || hash_length!=32)++errors;
    for(unsigned i=0;i<32;i++)sprintf(hex+i*2,"%02x",hash[i]);hex[64]=0;
    if(fflush(output))++errors;if(fclose(output))++errors;
    int complete=MatDepacketizerFinish(&depacketizer);
    int result=(!errors && !concealed && complete && depacketizer.next_frame==(unsigned)count &&
        !_stricmp(hex,expected_hash) && wire_packets==wire_expected && !queue.incompatibleServer)?0:1;
    if(!result && rename(partial,argv[1])){++errors;result=1;}
    fprintf(summary,"{\"cycles\":%llu,\"expectedCycles\":%d,\"sha256\":\"%s\",\"published\":%s,"
        "\"completeEOS\":%s,\"errors\":%u,\"concealed\":%u,\"wirePackets\":%u,\"wireExpected\":%u,"
        "\"injectedDrops\":%u,\"duplicates\":%u,\"deliveredPackets\":%u,\"maxDatagramBytes\":%u,"
        "\"elapsedMs\":%llu,\"fecDisabled\":%s,\"fecRecovered\":%u,\"fecFailed\":%u,\"fecInvalid\":%u,\"invalid\":%u}\n",
        (unsigned long long)depacketizer.next_frame,count,hex,result?"false":"true",complete?"true":"false",errors,concealed,
        wire_packets,wire_expected,drops,duplicates,delivered,max_datagram,(unsigned long long)((PltGetMicroseconds()-start)/1000),
        queue.incompatibleServer?"true":"false",queue.stats.packetCountFecRecovered,queue.stats.packetCountFecFailed,
        queue.stats.packetCountFecInvalid,queue.stats.packetCountInvalid);
    fclose(summary);RtpaCleanupQueue(&queue);PltDestroyCryptoContext(crypto);EVP_MD_CTX_free(digest);
    closesocket(sock);WSACleanup();return result;
}
