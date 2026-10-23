#pragma once
#include "quantum_v4_protocol.h"
namespace q4 {
struct Pass {
    uint64_t renderer=0,encoder=0,implementation=0,domain=0,pass=0,call=0;
    uint32_t frames=0;
    bool valid=false,insideSubmit=false,sawProcess=false;
};
// Audio callbacks: fixed storage, one CAS attempt, no waits/heap/logging.
// Publication lock is NEVER held across an original vendor function call.
class Recorder {
    struct Binding {
        uint64_t renderer=0,encoder=0,implementation=0,domain=0,nextPass=0;
        bool used=false,inPass=false;
    };
    Ring* ring_=nullptr;
    Binding bindings_[domainCapacity]{};
    volatile LONG registry_=0;
    uint64_t nextDomain_=0;
    volatile LONG64 loss_=0;
    bool emit(Header header,const float* pcm=nullptr,bool allowStopped=false);
    Header header(const Pass& pass,uint32_t kind) const;
    bool close(Binding& binding,uint32_t reason,bool allowStopped=false);
public:
    void start(Ring* ring);
    bool healthy();
    void fail();
    bool begin(uint64_t renderer,uint64_t encoder,uint64_t implementation,uint32_t frames,Pass& pass);
    bool before(Pass& pass,const void* info,const void* properties,const float* pcm,uint32_t frames,float gain);
    void returned(Pass& pass,HRESULT result,bool snapshotPublished);
    void processed(Pass& pass,uint32_t frames,uint32_t capacity,const uint32_t* produced,HRESULT result);
    void end(Pass& pass);
    void retire(uint64_t encoder,uint32_t reason);
    void unbound();
    void finish(); // management thread, only after all callbacks have left
};
}
