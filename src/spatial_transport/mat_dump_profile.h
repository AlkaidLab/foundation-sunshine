#pragma once
#include "../spatial_audio/mat_replay_source.h"
#include "../../third-party/moonlight-common-c/src/MatDumpProfile.h"
#include "../../third-party/moonlight-common-c/src/MatPlaybackProfile.h"
#include <memory>
#include <openssl/evp.h>
#include <sstream>

namespace spatial_transport {
struct ReplayManifest {
    std::filesystem::path path;
    LI_MAT_DUMP_PROFILE profile{};
};
// Host-local setting only. Never accept a path from the client. Snapshot once
// per DESCRIBE ticket; ANNOUNCE, retries and producer use the same manifest.
inline std::shared_ptr<const ReplayManifest> inspect_replay(const std::string& path) {
    using spatial_audio::require;
    if(path.empty())return {};
    auto size=std::filesystem::file_size(path);
    require(size && size%61440==0 && size/61440<=1000,"MAT dump supports 1..1000 carrier cycles");
    spatial_audio::MatReplaySource source(path,1,1);require(source.start(),"MAT dump replay open");
    auto digest=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    require(digest && EVP_DigestInit_ex(digest.get(),EVP_sha256(),nullptr)==1,"MAT manifest digest init");
    spatial_audio::Frame f;uint32_t cycles=0;
    while(true){auto r=source.read(f);if(r==spatial_audio::Read::ended)break;
        require(r==spatial_audio::Read::frame && cycles<size/61440,"MAT manifest read");
        require(EVP_DigestUpdate(digest.get(),f.bytes.data(),f.bytes.size())==1,"MAT manifest hash");++cycles;}
    require(cycles==size/61440,"MAT manifest changed size");
    unsigned char bytes[32];unsigned n=0;
    require(EVP_DigestFinal_ex(digest.get(),bytes,&n)==1 && n==32,"MAT manifest digest final");
    auto result=std::make_shared<ReplayManifest>();result->path=std::filesystem::absolute(path);result->profile.cycles=cycles;
    constexpr char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){result->profile.sha256[i*2]=hex[bytes[i]>>4];result->profile.sha256[i*2+1]=hex[bytes[i]&15];}
    return result;
}
inline std::string describe(const ReplayManifest& m) {
    return "a=x-ss-audio.mat-dump:1\r\na=x-ss-audio.mat-playback:1\r\na=x-ss-audio.mat-carrier:" MAT_PLAYBACK_CARRIER
        "\r\na=x-ss-audio.mat-clock:" MAT_PLAYBACK_CLOCK "\r\na=x-ss-audio.mat-sha256:"+std::string(m.profile.sha256)+
        "\r\na=x-ss-audio.mat-cycles:"+std::to_string(m.profile.cycles)+"\r\n";
}
inline bool accept(const ReplayManifest* m,std::string_view version,std::string_view hash,std::string_view cycles) {
    return m && version=="1" && hash==m->profile.sha256 && cycles==std::to_string(m->profile.cycles);
}
inline bool accept_playback(const ReplayManifest* m,std::string_view version,std::string_view hash,
        std::string_view cycles,std::string_view carrier,std::string_view clock) {
    return accept(m,version,hash,cycles) && carrier==MAT_PLAYBACK_CARRIER && clock==MAT_PLAYBACK_CLOCK;
}
}
