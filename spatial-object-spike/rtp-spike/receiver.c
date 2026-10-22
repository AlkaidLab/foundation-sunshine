// Dump-only client frontend. RTP reorder/FEC and AES are UNMODIFIED Moonlight
// production sources, not a Python protocol reimplementation. Loopback only.
#include "Limelight-internal.h"
#include <stdarg.h>
#include <inttypes.h>

int AppVersionQuad[4] = {7, 1, 500, 0};
int AudioPacketDuration = 32;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
static RTP_AUDIO_QUEUE queue;
static PPLT_CRYPTO_CONTEXT crypto;
static FILE *audio, *frames;
static int encrypted, delivered, errors, concealed;
// Unmodified Moonlight deliberately discards its first FEC group. The sender
// supplies four preroll packets; do NOT disable or bypass that synchronization.
static uint16_t expectedSeq=4;

uint64_t PltGetMicroseconds(void) {
    LARGE_INTEGER value, frequency;
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return (uint64_t)(value.QuadPart / frequency.QuadPart) * 1000000 +
        (uint64_t)(value.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
}
static void log_message(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
}
static void deliver(PRTP_PACKET packet, int length) {
    if (!length) { ++concealed; return; }
    unsigned char plaintext[8192];
    unsigned char *data = (unsigned char *)(packet + 1);
    int size = length - sizeof(*packet);
    if (packet->sequenceNumber != expectedSeq || packet->timestamp != (uint32_t)expectedSeq * 32) ++errors;
    expectedSeq = packet->sequenceNumber + 1;
    if (encrypted) {
        unsigned char key[16], iv[16] = {0};
        for (int i=0; i<16; ++i) key[i]=(unsigned char)i;
        uint32_t value = htonl(0x10203040u + packet->sequenceNumber);
        memcpy(iv, &value, sizeof(value));
        if (!PltDecryptMessage(crypto, ALGORITHM_AES_CBC, CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH,
                key,16,iv,16,NULL,0,data,size,plaintext,&size)) { ++errors; return; }
        data=plaintext;
    }
    if (size<6 || data[0]!=0x0b || data[1]!=0x77 || ((((data[2]&7)<<8)|data[3])+1)*2!=size) {
        ++errors; return;
    }
    if (fwrite(data,1,size,audio)!=(size_t)size) { ++errors; return; }
    fprintf(frames,"%u,%u,%d\n",packet->sequenceNumber,packet->timestamp,size);
    ++delivered;
}
static void accept_packet(unsigned char *bytes, int length) {
    PRTP_PACKET packet=(PRTP_PACKET)bytes;
    int status=RtpaAddPacket(&queue,packet,(uint16_t)length);
    if (RTPQ_HANDLE_NOW(status)) deliver(packet,length);
    if (RTPQ_PACKET_READY(status)) {
        uint16_t size; PRTP_PACKET next;
        while ((next=RtpaGetQueuedPacket(&queue,0,&size))!=NULL) { deliver(next,size); free(next); }
    }
}
int main(int argc, char **argv) {
    // output.eac3 frames.csv stats.json expected-frames plain|aes none|drop1|drop2|reorder
    if (argc!=7) return 2;
    const int count=atoi(argv[4]);
    if (count<1 || count>2000 || (strcmp(argv[5],"plain") && strcmp(argv[5],"aes"))) return 2;
    const char *fault=argv[6];
    if (strcmp(fault,"none") && strcmp(fault,"drop1") && strcmp(fault,"drop2") && strcmp(fault,"reorder")) return 2;
    encrypted=!strcmp(argv[5],"aes");
    audio=fopen(argv[1],"wbx"); frames=fopen(argv[2],"wx"); FILE *summary=fopen(argv[3],"wx");
    if (!audio || !frames || !summary) return 3;
    fprintf(frames,"sequence,timestamp_ms,bytes\n");
    ListenerCallbacks.logMessage=log_message;
    crypto=PltCreateCryptoContext(); RtpaInitializeQueue(&queue);
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2),&wsa)) return 4;
    SOCKET sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(sock,(struct sockaddr *)&address,sizeof(address))) return 4;
    int addrlen=sizeof(address); getsockname(sock,(struct sockaddr *)&address,&addrlen);
    DWORD timeout=500; int capacity=1024*1024;
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof(timeout));
    setsockopt(sock,SOL_SOCKET,SO_RCVBUF,(const char *)&capacity,sizeof(capacity));
    printf("READY %u\n",ntohs(address.sin_port)); fflush(stdout);
    unsigned char wire[8192], held[8192]; int heldLength=0;
    int wirePackets=0,dropped=0,timeouts=0,maxDatagram=0;
    const int wireFrames=count+4;
    const int wireExpected=wireFrames+(wireFrames/4)*2;
    const uint64_t deadline=PltGetMicroseconds()+(uint64_t)wireFrames*32000+5000000;
    while (wirePackets<wireExpected && PltGetMicroseconds()<deadline) {
        int length=recv(sock,(char *)wire,sizeof(wire),0);
        if (length<0) { if (WSAGetLastError()!=WSAETIMEDOUT) { ++errors; break; } ++timeouts; continue; }
        ++wirePackets; if (length>maxDatagram) maxDatagram=length;
        if (length<(int)sizeof(RTP_PACKET)) { ++errors; continue; }
        PRTP_PACKET packet=(PRTP_PACKET)wire;
        packet->sequenceNumber=ntohs(packet->sequenceNumber);
        packet->timestamp=ntohl(packet->timestamp); packet->ssrc=ntohl(packet->ssrc);
        const int seq=packet->sequenceNumber;
        const bool completeBlock=seq>=4 && seq/4<wireFrames/4;
        if (packet->packetType==97 && completeBlock) {
            if ((!strcmp(fault,"drop1") && seq%4==1) ||
                (!strcmp(fault,"drop2") && (seq%4==1 || seq%4==2))) { ++dropped; continue; }
            if (!strcmp(fault,"reorder") && seq%4==1) { memcpy(held,wire,length); heldLength=length; continue; }
        }
        accept_packet(wire,length);
        if (heldLength && packet->packetType==97 && seq%4==2) { accept_packet(held,heldLength); heldLength=0; }
    }
    if (heldLength) accept_packet(held,heldLength);
    fprintf(summary,"{\n\"prerollFrames\":4,\"expectedFrames\":%d,\"deliveredFrames\":%d,\"errors\":%d,\"concealed\":%d,"
        "\"wirePackets\":%d,\"wireExpected\":%d,\"injectedDrops\":%d,\"timeouts\":%d,\"maxDatagramBytes\":%d,"
        "\"fecDisabled\":%s,\"fecRecovered\":%u,\"fecFailed\":%u,\"fecInvalid\":%u,\"invalid\":%u\n}\n",
        count,delivered,errors,concealed,wirePackets,wireExpected,dropped,timeouts,maxDatagram,
        queue.incompatibleServer?"true":"false",queue.stats.packetCountFecRecovered,queue.stats.packetCountFecFailed,
        queue.stats.packetCountFecInvalid,queue.stats.packetCountInvalid);
    const int result=(delivered==count && !errors && !concealed && !queue.incompatibleServer && wirePackets==wireExpected)?0:1;
    fclose(audio); fclose(frames); fclose(summary);
    RtpaCleanupQueue(&queue); PltDestroyCryptoContext(crypto); closesocket(sock); WSACleanup();
    return result;
}
