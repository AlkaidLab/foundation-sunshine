#pragma once
#include "../../spatial-object-spike/mat-spike/cpp_mat_stream.hpp"
#include <functional>

namespace spatial_audio {
// Worker-only adapter. It receives owned bytes, not borrowed audio pointers.
class ObjectBroker {
    struct Domain {std::unique_ptr<cpp_bridge_native4::DomainBridge> bridge;int64_t first=0;};
    q4::Assembler assembler_;std::map<uint64_t,Domain> domains_;
    std::function<bool(const mat_stream::Packet&)> emit_;
    std::set<uint64_t> excluded_encoders_;uint64_t selected_;
    bool poisoned_=false,closed_=false;
public:
    uint64_t events=0,source_rounds=0,closes=0,excluded_domains=0;
    explicit ObjectBroker(std::function<bool(const mat_stream::Packet&)> emit,uint64_t selected=0,std::set<uint64_t> excluded={}):emit_(std::move(emit)),excluded_encoders_(std::move(excluded)),selected_(selected){}
    void push(const q4::Header& h,const float* pcm){
        cpp_bridge::check(!poisoned_&&!closed_,"object source is terminal");
        try{
            q4::Quantum q;bool sealed=assembler_.push(h,pcm,q);++events;
            if(h.kind==q4::DomainBegin){bool include=(!selected_||selected_==h.domain)&&!excluded_encoders_.contains(h.encoder)&&!excluded_encoders_.contains(h.implementation);Domain d;if(include)d.bridge=std::make_unique<cpp_bridge_native4::DomainBridge>(h.domain);else ++excluded_domains;domains_.emplace(h.domain,std::move(d));}
            auto at=domains_.find(h.domain);cpp_bridge::check(at!=domains_.end(),"broker domain");auto& d=at->second;
            if(h.kind==q4::PassBegin)d.first=h.audio.qpc;
            if(sealed&&d.bridge){auto packet=std::make_unique<mat_stream::Packet>();mat_stream::snapshot(*packet,h.domain,d.bridge->consume(q,d.first,h.audio.qpc));cpp_bridge::check(emit_(*packet),"object sink refused frame; epoch cannot resume");++source_rounds;d.bridge->identities.clear();}
            if(h.kind==q4::DomainEnd){if(d.bridge){auto p=std::make_unique<mat_stream::Packet>();mat_stream::snapshot(*p,h.domain,d.bridge->finish());cpp_bridge::check(emit_(*p),"object sink refused EOF");++closes;}domains_.erase(at);}
        }catch(...){poisoned_=true;throw;}
    }
    void finish(){cpp_bridge::check(!poisoned_&&!closed_,"terminal object source");try{assembler_.finish();cpp_bridge::check(domains_.empty(),"unclosed source domain");closed_=true;}catch(...){poisoned_=true;throw;}}
};
}
