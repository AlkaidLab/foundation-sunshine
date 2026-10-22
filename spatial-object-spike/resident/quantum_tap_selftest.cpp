#include "quantum_tap.cpp"
#include <stdexcept>

static void RequireTest(bool value,const char* what) {if(!value)throw std::runtime_error(what);}
static HRESULT __fastcall FakeSubmit(void*,const void*,const void*,const float*,uint32_t,float){return S_OK;}
static HRESULT __fastcall FakeEncode(void*,uint32_t,uint32_t,BYTE*,uint32_t* bytes){*bytes=0;return S_OK;}
int main() {
    Ring* ring=nullptr;
    try {
        ring=static_cast<Ring*>(VirtualAlloc(nullptr,sizeof(Ring),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        RequireTest(ring!=nullptr,"allocate own ring");
        void* encoder=reinterpret_cast<void*>(0x123400);
        candidate=encoder;currentRing=ring;originalSubmit=FakeSubmit;originalEncode=FakeEncode;
        uint8_t info[12]{};uint64_t host=77;memcpy(info+4,&host,8);
        struct Properties {uint32_t id,frames,type,flags;float x,y,z,volume;} props{42,480,1,6,1,2,3,.5f};
        static_assert(sizeof(Properties)==32);float pcm[480];for(unsigned i=0;i<480;++i)pcm[i]=float(i)/1000;
        HookSubmit(encoder,info,&props,pcm,480,.25f);uint32_t bytes=99;
        HookEncode(encoder,480,30720,nullptr,&bytes);
        RequireTest(ring->write==2 && ring->slots[0].kind==kQuantumObject && ring->slots[0].encoder==0x123400
            && ring->slots[0].record.host==77 && ring->slots[0].propertyFlags==6
            && !memcmp(ring->slots[0].samples,pcm,sizeof(pcm)),"actual hook object copy");
        RequireTest(ring->slots[1].kind==kQuantumProcess && ring->slots[1].inputFrames==480
            && !ring->slots[1].record.frames && ring->slots[1].result==S_OK
            && !ring->slots[1].producedBytes && ring->slots[1].capacity==30720,"zero-output Process still seals a pass");
        HookSubmit(reinterpret_cast<void*>(0x567800),info,&props,pcm,480,1);
        RequireTest(ring->write==2,"foreign encoder not mixed into chosen clock");
        HookSubmit(encoder,info,&props,nullptr,480,1);
        RequireTest(ring->write==3 && ring->silent==1 && ring->slots[2].samples[479]==0,"null PCM becomes captured silence");
        ring->writer=1;CaptureBoundary(ring,encoder,480,30720,&bytes,S_OK);ring->writer=0;
        RequireTest(ring->droppedBusy==1 && lossEpoch==1,"busy marker explicitly invalidates loss epoch");
        CaptureBoundary(ring,encoder,480,30720,nullptr,S_OK);
        RequireTest(ring->invalid==1 && lossEpoch==2,"invalid produced pointer refused");
        ring->read=0;ring->write=kSlots;CaptureBoundary(ring,encoder,480,30720,&bytes,S_OK);
        RequireTest(ring->droppedFull==1 && lossEpoch==3,"full marker never overwrites");
        currentRing=nullptr;VirtualFree(ring,0,MEM_RELEASE);ring=nullptr;
        puts("{\"passed\":true,\"checks\":7,\"scope\":\"own-process fake originals and private ring; no hooks installed\",\"audiodg_accessed\":false,\"vendor_called\":false}");return 0;
    } catch(const std::exception& e) {currentRing=nullptr;if(ring)VirtualFree(ring,0,MEM_RELEASE);printf("FAILED %s\n",e.what());return 1;}
}
