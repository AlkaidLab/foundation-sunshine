/* Diagnostic adapter to the actual client RTP/FEC + AudioStream decode path.
 * UDP is loopback only. There is no AudioTrack sink or RTSP server here. */
#include "../../third-party/moonlight-common-c/src/AudioStream.c"
#include "live_check_transport.h"
#include "../../third-party/moonlight-common-c/src/MatLiveCrypto.h"
#include <rs.h>
static SOCKET sender=INVALID_SOCKET,receiver=INVALID_SOCKET;
static struct sockaddr_in destination;
static int terminated,drop_mode,initialized;
static uint64_t next_packet,datagrams,drops;
static reed_solomon* encoder;
static uint8_t shards[RTPA_TOTAL_SHARDS][MAT_LIVE_PACKET];
static void failure(int code){(void)code;terminated=1;}
void LiveCheckClose(void){
    if(sender!=INVALID_SOCKET)closeSocket(sender);sender=INVALID_SOCKET;
    if(receiver!=INVALID_SOCKET)closeSocket(receiver);receiver=INVALID_SOCKET;
    if(encoder)reed_solomon_release(encoder);encoder=NULL;
    if(matLive)MatLivePlaybackClose(matLive);matLive=NULL;
    if(initialized){RtpaCleanupQueue(&rtpAudioQueue);cleanupPlatform();initialized=0;}
}
int LiveCheckOpen(const LI_MAT_LIVE_PROFILE* profile,const uint8_t key[16],const LI_MAT_PLAYBACK_CALLBACKS* callbacks,int drop){
    if(initialized||initializePlatform())return 0;initialized=1;
    AppVersionQuad[0]=7;AppVersionQuad[1]=1;AppVersionQuad[2]=431;AppVersionQuad[3]=-1;
    NegotiatedAudioCodec=AUDIO_CODEC_MAT_LIVE;AudioEncryptionEnabled=true;AudioPacketDuration=1;
    ListenerCallbacks.connectionTerminated=failure;terminated=0;next_packet=datagrams=drops=0;drop_mode=drop;
    matPlaybackFailedReported=matPlaybackCompleteReported=false;
    RtpaInitializeQueue(&rtpAudioQueue);lastSeq=3;
    matLive=MatLivePlaybackOpen(profile,key,callbacks);if(!matLive)goto fail;
    encoder=reed_solomon_new(RTPA_DATA_SHARDS,RTPA_FEC_SHARDS);if(!encoder)goto fail;
    {const uint8_t parity[]={0x77,0x40,0x38,0x0e,0xc7,0xa7,0x0d,0x6c};memcpy(encoder->p,parity,sizeof(parity));}
    sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(sender==INVALID_SOCKET||receiver==INVALID_SOCKET)goto fail;
    memset(&destination,0,sizeof(destination));destination.sin_family=AF_INET;destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(receiver,(struct sockaddr*)&destination,sizeof(destination)))goto fail;
    {socklen_t n=sizeof(destination);
#ifdef LC_WINDOWS
     DWORD timeout=1000;
#else
     struct timeval timeout={1,0};
#endif
     if(getsockname(receiver,(struct sockaddr*)&destination,&n)||setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout)))goto fail;}
    return 1;
fail:LiveCheckClose();return 0;
}
static int send_one(const uint8_t* wire,int length){
    QUEUED_AUDIO_PACKET input={0};PRTP_PACKET r=(PRTP_PACKET)input.data;
    if(sendto(sender,(const char*)wire,length,0,(struct sockaddr*)&destination,sizeof(destination))!=length)return 0;
    input.header.size=recv(receiver,input.data,MAX_PACKET_SIZE,0);
    if(input.header.size!=length||memcmp(input.data,wire,length))return 0;++datagrams;
    r->sequenceNumber=BE16(r->sequenceNumber);r->timestamp=BE32(r->timestamp);r->ssrc=BE32(r->ssrc);
    int result=RtpaAddPacket(&rtpAudioQueue,r,(uint16_t)length);
    if(RTPQ_HANDLE_NOW(result))decodeInputData(&input);
    else if(RTPQ_PACKET_READY(result)){
        uint16_t n;PQUEUED_AUDIO_PACKET queued;
        while((queued=(PQUEUED_AUDIO_PACKET)RtpaGetQueuedPacket(&rtpAudioQueue,sizeof(QUEUED_AUDIO_PACKET_HEADER),&n))!=NULL){
            queued->header.size=n;decodeInputData(queued);free(queued);
        }
    }
    return !terminated;
}
int LiveCheckSend(const uint8_t* payload,size_t length){
    uint8_t wire[sizeof(RTP_PACKET)+sizeof(AUDIO_FEC_HEADER)+MAT_LIVE_PACKET]={0};
    PRTP_PACKET r=(PRTP_PACKET)wire;uint64_t sequence=next_packet++;unsigned index=(unsigned)(sequence%4);
    if(!initialized||length!=MAT_LIVE_PACKET||terminated)return 0;
    r->header=0x80;r->packetType=97;r->sequenceNumber=BE16((uint16_t)sequence);r->timestamp=BE32((uint32_t)sequence);
    memcpy(shards[index],payload,length);memcpy(r+1,payload,length);
    /* Controlled loss occurs after encryption, before the real UDP receive path.
     * Never drop SYNC. One missing data shard every 50 blocks must be recovered. */
    if(drop_mode&&sequence>=4&&index==1&&(sequence/4)%50==1)++drops;
    else if(!send_one(wire,(int)(sizeof(*r)+length)))return 0;
    if(index==3){
        uint8_t* pointers[RTPA_TOTAL_SHARDS];for(unsigned i=0;i<RTPA_TOTAL_SHARDS;++i)pointers[i]=shards[i];
        if(reed_solomon_encode(encoder,pointers,RTPA_TOTAL_SHARDS,(int)length))return 0;
        /* Mode 2 proves recovery with exactly three data + one parity shard.
         * Unlike Debug FEC validation, production needs no extra test shard. */
        for(unsigned i=0;i<(drop_mode==2?1:RTPA_FEC_SHARDS);++i){
            AUDIO_FEC_HEADER* f=(AUDIO_FEC_HEADER*)(r+1);r->packetType=127;r->sequenceNumber=BE16((uint16_t)(sequence+i+1));
            r->timestamp=0;f->fecShardIndex=(uint8_t)i;f->payloadType=97;f->baseSequenceNumber=BE16((uint16_t)(sequence-3));
            f->baseTimestamp=BE32((uint32_t)(sequence-3));f->ssrc=0;memcpy(f+1,shards[4+i],length);
            if(!send_one(wire,(int)(sizeof(*r)+sizeof(*f)+length)))return 0;
        }
    }
    return !terminated;
}
uint64_t LiveCheckDatagrams(void){return datagrams;}
uint64_t LiveCheckDrops(void){return drops;}
uint64_t LiveCheckRecovered(void){return rtpAudioQueue.stats.packetCountFecRecovered;}
