#include "encoded_tap.cpp"
#include <cassert>
#include <cstdio>
int main() {
    auto* r=static_cast<EncodedRing*>(VirtualAlloc(nullptr,sizeof(EncodedRing),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));assert(r);
    BYTE data[kByteCapacity];for(uint32_t i=0;i<kByteCapacity;++i) data[i]=static_cast<BYTE>(i*17+3);
    uint32_t produced=61440;Capture(r,reinterpret_cast<void*>(1),480,61440,data,&produced,S_OK);
    assert(r->write==1 && r->bytes==61440 && !memcmp(r->slots[0].data,data,produced));
    produced=0;Capture(r,reinterpret_cast<void*>(1),480,61440,nullptr,&produced,S_OK);assert(r->write==2 && !r->slots[1].record.bytes);
    produced=61441;Capture(r,nullptr,480,61440,data,&produced,S_OK);assert(r->invalid==1 && r->write==2);
    Capture(r,nullptr,480,61440,data,nullptr,S_OK);assert(r->invalid==2 && r->write==2);
    produced=4;for(uint32_t n=2;n<kByteSlots;++n) Capture(r,nullptr,480,61440,data,&produced,S_OK);
    Capture(r,nullptr,480,61440,data,&produced,S_OK);assert(r->dropped==1 && r->write==kByteSlots);
    VirtualFree(r,0,MEM_RELEASE);puts("PASS bounded byte copy / zero output / invalid pointers and lengths / full ring drop");
}
