#define NOMINMAX
#include "position_cache.h"
#include <cassert>
#include <cstdio>
int main() {
    PositionCache c;Record r{};r.host=1;r.objectId=1;r.type=1;r.frames=480;
    auto apply=[&](uint32_t f,uint64_t loss=0){++r.sequence;return c.Apply(r,f,loss);};
    auto a=apply(2);assert(a.positionState==position_state::Unknown && std::isnan(a.audio.x));
    r.x=3;r.y=-2;r.z=7;r.volume=.4f;
    auto b=apply(6);assert(b.positionState==position_state::Updated && b.audio.x==3 && c.recovered==1);
    r.x=r.y=r.z=0;auto d=apply(2);assert(d.positionState==position_state::Cached && d.audio.x==3 && d.audio.y==-2);
    auto e=apply(2,1);assert(e.positionState==position_state::Unknown && std::isnan(e.audio.x) && e.generation!=d.generation);
    r.x=5;auto f=apply(4,1);assert(f.audio.x==5 && f.audio.volume==.4f);
    auto g=apply(8,1);assert(g.positionState==position_state::Cached);
    auto h=apply(2,1);assert(h.positionState==position_state::Unknown && h.generation!=g.generation);
    auto i=apply(1,1);assert(i.positionState==position_state::DefaultOrigin && i.audio.x==0 && i.audio.volume==1);
    r.x=9;auto j=apply(7,1);assert(j.positionState==position_state::Updated && j.audio.x==9 && j.generation!=i.generation);
    r.type=2;auto k=apply(2,1);assert(k.positionState==position_state::StaticChannel && std::isnan(k.audio.x));
    r.type=1;r.objectId=2;auto l=apply(2,1);assert(l.positionState==position_state::Unknown);
    for(uint32_t n=3;n<5000;++n) {r.objectId=n;apply(2,1);}
    assert(c.peak==4096 && c.evictions>0);
    puts("PASS late attach / delta / create / end / ID reuse / gap / static / bounded cache");
}
