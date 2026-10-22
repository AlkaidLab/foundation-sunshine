#pragma once
#include "mat_dump_profile.h"
#include "mat_fragmenter.h"
#include <chrono>
#include <cstring>
#include <thread>

namespace spatial_transport {
struct ReplayWait {
    void operator()(std::chrono::nanoseconds duration) const {
        // Portable dump/test fallback; the normal host supplies its blocking
        // high-precision timer. The caller rechecks the deadline even if a
        // MinGW sleep rounds a sub-ms wait down to a scheduler yield.
        if(duration>std::chrono::nanoseconds::zero())std::this_thread::sleep_for(duration);
    }
};
// Own session worker. Emit is bounded/cancellable and returns only after actual
// broadcast ACK; no foreign-process callback, device, or private encoder calls.
template<class Emit,class Wait=ReplayWait> void replay_session(const ReplayManifest& manifest,Emit emit,Wait wait={}) {
    using spatial_audio::require;
    auto epoch=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())|1;
    spatial_audio::MatReplaySource source(manifest.path,epoch,1);require(source.start(),"MAT replay open");
    auto digest=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    require(digest && EVP_DigestInit_ex(digest.get(),EVP_sha256(),nullptr)==1,"MAT session digest init");
    uint8_t control[MAT_WIRE_PACKET];mat_wire_make(control,MAT_WIRE_SYNC,epoch,1,0,0,0,nullptr);
    for(unsigned i=0;i<4;i++)emit(control,sizeof(control));
    auto next_fragment=std::chrono::steady_clock::now();uint64_t cycles=0;uint32_t hash=2166136261u;
    spatial_audio::Frame f;
    while(true) {
        auto r=source.read(f);if(r==spatial_audio::Read::ended)break;
        require(r==spatial_audio::Read::frame && cycles<manifest.profile.cycles && f.sequence==cycles,"MAT session recording changed");
        unsigned fragment_index=0;
        fragment(f,[&](const uint8_t* p,size_t n){
            // A stalled host must not catch up by bursting all overdue packets
            // into the client's bounded dump queue. Preserve every carrier byte
            // and its media timestamp; dump transport can take longer than media.
            while(std::chrono::steady_clock::now()<next_fragment)
                wait(next_fragment-std::chrono::steady_clock::now());
            auto emitted_at=std::chrono::steady_clock::now();
            auto interval=uint64_t(fragment_index+1)*20000/60-uint64_t(fragment_index)*20000/60;
            ++fragment_index;
            emit(p,n);
            next_fragment=emitted_at+std::chrono::microseconds(interval);
        });
        require(EVP_DigestUpdate(digest.get(),f.bytes.data(),f.bytes.size())==1,"MAT session digest update");
        hash=mat_hash_update(hash,f.bytes.data(),f.bytes.size());++cycles;
    }
    require(cycles==manifest.profile.cycles,"MAT session cycle count changed");
    unsigned char raw[32];unsigned n=0;char hex[65]={};constexpr char digits[]="0123456789abcdef";
    require(EVP_DigestFinal_ex(digest.get(),raw,&n)==1 && n==32,"MAT session digest final");
    for(unsigned i=0;i<32;i++){hex[i*2]=digits[raw[i]>>4];hex[i*2+1]=digits[raw[i]&15];}
    require(!std::strcmp(hex,manifest.profile.sha256),"MAT session content changed after DESCRIBE");
    // Never send successful END for a changed recording, even if dimensions fit.
    mat_wire_make(control,MAT_WIRE_END,epoch,1,cycles,0,hash,nullptr);
    for(unsigned i=0;i<4;i++)emit(control,sizeof(control));
}
}
