#pragma once
#include "../../src/spatial_audio/encoded_audio_source.h"
#include <filesystem>
#include <fstream>

namespace spatial_audio {
// Read one carrier cycle at a time: memory is independent of recording length.
// This source has no audio/device/foreign-process or private-codec operations.
class MatReplaySource final:public EncodedAudioSource {
    std::filesystem::path path_;uint64_t epoch_,domain_,sequence_=0,total_=0;
    std::ifstream input_;State state_=State::idle;std::string error_;
public:
    MatReplaySource(std::filesystem::path path,uint64_t epoch,uint64_t domain):path_(std::move(path)),epoch_(epoch),domain_(domain){}
    Descriptor descriptor()const override{return {};}
    bool start()override{
        if(state_!=State::idle){error_="source is single-session";return false;}
        state_=State::preparing;
        try{require(epoch_&&domain_,"nonzero replay session identity");auto size=std::filesystem::file_size(path_);require(size&&size%carrier_bytes==0,"whole carrier cycles required");total_=size/carrier_bytes;input_.open(path_,std::ios::binary);require(bool(input_),"open replay input");state_=State::running;return true;}
        catch(const std::exception& e){error_=e.what();state_=State::fault;return false;}
    }
    Read read(Frame& out)override{
        if(state_==State::stopped)return Read::ended;
        if(state_!=State::running)return Read::failed;
        try{
            if(sequence_==total_){require(input_.peek()==EOF,"recording grew during replay");input_.close();state_=State::stopped;return Read::ended;}
            Frame next;next.epoch=epoch_;next.domain=domain_;next.sequence=sequence_;next.sample_start=sequence_*960;next.bytes.resize(carrier_bytes);
            input_.read(reinterpret_cast<char*>(next.bytes.data()),next.bytes.size());require(input_.gcount()==carrier_bytes,"recording truncated during replay");validate_carrier(next.bytes);out=std::move(next);++sequence_;return Read::frame;
        }catch(const std::exception& e){error_=e.what();state_=State::fault;input_.close();return Read::failed;}
    }
    void stop()override{input_.close();if(state_!=State::fault)state_=State::stopped;}
    State state()const override{return state_;}
    std::string error()const override{return error_;}
};
}
