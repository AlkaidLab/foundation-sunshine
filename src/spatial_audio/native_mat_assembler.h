#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
#include "../../spatial-object-spike/resident/encoded_protocol.h"
#include <array>
#include <functional>
namespace mat_capture {
struct Observations {
    uint64_t records=0,empty=0,wrong_result=0,wrong_frames=0,wrong_bytes=0,non_mat=0,accepted_halves=0;
    uint32_t last_frames=0,last_capacity=0,last_bytes=0;
    int32_t last_result=0;
};
// Runs in the collector process. Hook callbacks only copy into the fixed shared ring.
class Assembler {
    uint64_t sequence_=0,encoder_=0,last_qpc_=0,first_qpc_=0,skipped_=0;
    bool half_=false;
    std::array<uint8_t,MAT_WIRE_CYCLE> bytes_{};
    Observations observations_{};
public:
    uint64_t encoder()const{return encoder_;}
    uint64_t startup_skipped()const{return skipped_;}
    const Observations& observations()const{return observations_;}
    template<class Consumer>void push(const EncodedRecord& r,const uint8_t* data,Consumer consume) {
        using spatial_audio::require;
        require(r.magic==0x42595445 && r.sequence==sequence_++ && r.bytes<=kByteCapacity &&
            r.produced==r.bytes && r.capacity>=r.bytes && !r.flags && !r.reserved && r.qpc>0 &&
            uint64_t(r.qpc)>=last_qpc_,"Native encoder record integrity/sequence/time");last_qpc_=r.qpc;
        auto& o=observations_;++o.records;o.last_frames=r.inputFrames;o.last_capacity=r.capacity;
        o.last_bytes=r.bytes;o.last_result=r.result;
        if(!r.bytes)++o.empty;
        if(r.result!=S_OK)++o.wrong_result;
        if(r.inputFrames!=480)++o.wrong_frames;
        if(r.bytes!=30720)++o.wrong_bytes;
        // Counters overlap. Short/empty outputs never read stale ring bytes.
        // Only inspect the IEC preamble; no audio bytes are logged.
        bool mat_header=r.bytes>=5 && !memcmp(data,"\x72\xf8\x1f\x4e",4) && (data[4]&31)==0x16;
        if(!mat_header)++o.non_mat;
        if(!encoder_) {
            if(r.result!=S_OK || r.inputFrames!=480 || r.bytes!=30720 ||
                !mat_header) {++skipped_;return;}
            encoder_=r.encoder;require(encoder_!=0,"Native encoder identity");
        }
        require(r.encoder==encoder_ && r.result==S_OK && r.inputFrames==480 && r.bytes==30720,
            "Native encoder changed/failed/format changed; no source switch");
        ++o.accepted_halves;
        if(!half_) {memcpy(bytes_.data(),data,30720);first_qpc_=r.qpc;half_=true;return;}
        memcpy(bytes_.data()+30720,data,30720);spatial_audio::validate_carrier(bytes_);
        consume(bytes_,first_qpc_);half_=false;
    }
    bool partial()const{return half_;}
};
}
