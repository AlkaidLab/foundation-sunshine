#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
#include "../../spatial-object-spike/resident/encoded_protocol.h"
#include <array>
#include <functional>
namespace mat_capture {
// Runs in the collector process. Hook callbacks only copy into the fixed shared ring.
class Assembler {
    uint64_t sequence_=0,encoder_=0,last_qpc_=0,first_qpc_=0,skipped_=0;
    bool half_=false;
    std::array<uint8_t,MAT_WIRE_CYCLE> bytes_{};
public:
    uint64_t encoder()const{return encoder_;}
    uint64_t startup_skipped()const{return skipped_;}
    template<class Consumer>void push(const EncodedRecord& r,const uint8_t* data,Consumer consume) {
        using spatial_audio::require;
        require(r.magic==0x42595445 && r.sequence==sequence_++ && r.bytes<=kByteCapacity &&
            r.produced==r.bytes && r.capacity>=r.bytes && !r.flags && !r.reserved && r.qpc>0 &&
            uint64_t(r.qpc)>=last_qpc_,"Native encoder record integrity/sequence/time");last_qpc_=r.qpc;
        if(!encoder_) {
            if(r.result!=S_OK || r.inputFrames!=480 || r.bytes!=30720 ||
                memcmp(data,"\x72\xf8\x1f\x4e",4) || (data[4]&31)!=0x16) {++skipped_;return;}
            encoder_=r.encoder;require(encoder_!=0,"Native encoder identity");
        }
        require(r.encoder==encoder_ && r.result==S_OK && r.inputFrames==480 && r.bytes==30720,
            "Native encoder changed/failed/format changed; no source switch");
        if(!half_) {memcpy(bytes_.data(),data,30720);first_qpc_=r.qpc;half_=true;return;}
        memcpy(bytes_.data()+30720,data,30720);spatial_audio::validate_carrier(bytes_);
        consume(bytes_,first_qpc_);half_=false;
    }
    bool partial()const{return half_;}
};
}
