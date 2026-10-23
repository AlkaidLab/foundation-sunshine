// Own-process callback/bridge tests. No audiodg, vendor loading or file replay.
#define SONIC_TAP_TEST
#include "tap.cpp"
#include "bridge.h"
#include <iostream>
#include <limits>
static BYTE hostA[0x150],hostB[0x150];static float samples[480];static bool endA;
static HRESULT __fastcall FakeMono(void*,uint32_t,const float* pcm,uint32_t count,float){
    // Deliberately alter caller storage after interception to test copy ownership.
    if(pcm)for(uint32_t n=0;n<count;++n)const_cast<float*>(pcm)[n]=.9f;return S_OK;
}
static HRESULT __fastcall FakeEnd(void*){return S_OK;}
static unsigned dispatched;
static void __fastcall FakeDispatch(void*,uint32_t inputs,void* input,uint32_t outputs,void* output){if(inputs==1&&input&&outputs==1&&!output)++dispatched;}
static void __fastcall FakeRender(void*,uint32_t,void*,uint32_t,void*){
    for(auto& f:samples)f=.2f;HookMono(hostA,0,samples,480,.5f);
    for(auto& f:samples)f=.1f;HookMono(hostB,0,samples,480,.25f);
    if(endA)HookEnd(hostA);
}
static void Host(BYTE* host,uint32_t id,float volume,float x){
    memset(host,0,0x150);uint32_t type=1;memcpy(host+0x64,&volume,4);memcpy(host+0x68,&id,4);memcpy(host+0x6c,&type,4);memcpy(host+0x98,&x,4);
}
static void Check(bool b){if(!b)throw std::runtime_error("Sonic test assertion");}
int main(){try{
    auto ring=std::make_unique<sonic_live::Ring>();memset(ring.get(),0,sizeof(*ring));
    BYTE renderer[0x230]{};uint32_t rate=48000,frames=480;memcpy(renderer+0x48,&rate,4);memcpy(renderer+0x22c,&frames,4);
    BYTE connection[24]{};memcpy(connection+8,&frames,4);void* input=connection;
    {uint32_t flags=1;memcpy(connection+12,&flags,4);void* table[]={reinterpret_cast<void*>(FakeDispatch)};void* engine=table;void* engineAddress=&engine;memcpy(renderer+0x130,&engineAddress,sizeof(engineAddress));originalDispatch=FakeDispatch;
     InterlockedExchangePointer(&current,ring.get());HookDispatch(renderer,1,&input,1,nullptr);
     Check(dispatched==1&&ring->dispatchCalls==1&&ring->dispatchEntry==reinterpret_cast<LONG64>(FakeDispatch)&&ring->dispatchFrames==480&&ring->dispatchFlags==1&&!ring->passes&&!ring->write&&!active);}
    Host(hostA,42,.3f,.75f);Host(hostB,43,.8f,-.5f);originalRender=FakeRender;originalMono=FakeMono;originalEnd=FakeEnd;
    InterlockedExchangePointer(&current,ring.get());endA=false;HookRender(renderer,1,&input,1,nullptr);
    Check(ring->write==1&&ring->passes==1&&ring->monoCalls==2&&!ring->invalid&&!ring->unbound&&!ring->dropped);
    auto p=std::make_unique<sonic_live::Pass>(ring->slots[0]);Check(p->count==2&&p->objects[0].pcm[0]==.2f&&p->objects[1].pcm[0]==.1f&&samples[0]==.9f);
    Check(p->rate==48000&&p->deviceFrames==480&&p->requestedFrames==480&&p->objects[0].xyz[0]==.75f);
    sonic_live::Bridge bridge;std::vector<std::unique_ptr<receiver::Slot>> packets;
    auto emit=[&](const receiver::Slot& s){packets.push_back(std::make_unique<receiver::Slot>(s));return true;};
    bridge.push(*p,emit);Check(packets.size()==1&&packets[0]->header.calls==2&&packets[0]->calls[0].flags==7);
    Check(packets[0]->calls[0].pcm[0]==.2f*(.3f*.5f)&&packets[0]->calls[1].pcm[0]==.1f*(.8f*.25f));
    endA=true;HookRender(renderer,1,&input,1,nullptr);*p=ring->slots[1];Check(p->objects[0].ended==1);bridge.push(*p,emit);
    p->sequence=2;p->firstQpc=p->lastQpc+1;p->lastQpc=p->firstQpc;p->count=1;p->endCount=0;p->objects[0]=p->objects[1];p->objects[0].ended=1;bridge.push(*p,emit);
    Check(packets[2]->header.calls==2&&packets[2]->calls[0].flags==8&&packets[2]->calls[1].flags==6);
    p->sequence=3;++p->firstQpc;++p->lastQpc;p->count=0;bridge.push(*p,emit);bridge.finish(emit);
    Check(bridge.sourcePasses==4&&bridge.emptyPasses==1&&bridge.inputSamples==5*480&&packets[3]->calls[0].flags==8&&packets[4]->header.kind==2);
    {sonic_live::Bridge delayed;*p=ring->slots[0];unsigned calls=0;delayed.push(*p,[](auto&){return true;});
     p->sequence=1;++p->firstQpc;++p->lastQpc;p->count=0;p->endCount=2;p->ends[0]={reinterpret_cast<uint64_t>(hostA),42,0};p->ends[1]={reinterpret_cast<uint64_t>(hostB),43,0};
     delayed.push(*p,[&](auto& packet){Check(packet.header.calls==2&&packet.calls[0].flags==8&&packet.calls[1].flags==8);++calls;return true;});
     delayed.finish([&](auto& packet){Check(packet.header.kind==2&&packet.header.calls==0);++calls;return true;});Check(calls==2);}
    for(unsigned test=0;test<10;++test){sonic_live::Bridge broken;*p=ring->slots[0];
        if(test==0)p->deviceFrames=448;if(test==1)p->rate=44100;if(test==2)p->objects[0].offset=16;if(test==3)p->objects[0].result=S_FALSE;
        if(test==4)p->objects[0].pcm[7]=std::numeric_limits<float>::quiet_NaN();if(test==5)p->objects[0].type=2;
        if(test==6)p->objects[1]=p->objects[0];if(test==7)p->objects[0].gain=100;
        bool failed=false;try{broken.push(*p,[](auto&){return true;});if(test==8){p->sequence=1;p->count=0;broken.push(*p,[](auto&){return true;});}
            if(test==9){p->sequence=1;p->renderer++;broken.push(*p,[](auto&){return true;});}}
        catch(...){failed=true;}Check(failed);
        failed=false;try{broken.finish([](auto&){return true;});}catch(...){failed=true;}Check(failed);
    }
    InterlockedExchangePointer(&current,nullptr);
    std::cout<<"PASS Sonic actual callback copies, explicit device clock, PCM/gain/XYZ, EOS retirement, missing-object and 10 fail-closed contracts; nativeCapture=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
