#define NOMINMAX
#include "position_cache.h"
#include "metadata_trace.h"
#include <fstream>
#include <string>
#include <cassert>
#include <cstdio>
// Compare the production cache with independently reconstructed Python CSV from
// the previous full ingress/sanitize/submit lifecycle capture, including creation.
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    FILE* binary{};_wfopen_s(&binary,argv[1],L"rb");assert(binary);
    std::ifstream csv(argv[2]);assert(csv);std::string line;std::getline(csv,line);
    PositionCache cache;Record r{};PropertyTrace trace{};uint64_t count=0,dynamic=0;
    while(fread(&r,sizeof(r),1,binary)==1) {
        assert(fread(&trace,sizeof(trace),1,binary)==1 && trace.flags==15);
        assert(std::getline(csv,line));
        unsigned long long sequence{},host{};long long qpc{};unsigned id{},type{},flags{};char state[80]{};float x{},y{},z{};
        int fields=sscanf_s(line.c_str(),"%llu,%lld,0x%llx,%u,%u,0x%x,%79[^,],%f,%f,%f",
            &sequence,&qpc,&host,&id,&type,&flags,state,static_cast<unsigned>(sizeof(state)),&x,&y,&z);
        assert(fields==7 || fields==10);
        uint32_t observedFlags{};memcpy(&observedFlags,trace.submitted+12,4);
        assert(sequence==r.sequence && host==r.host && id==r.objectId && type==r.type && flags==observedFlags);
        auto out=cache.Apply(r,observedFlags,0);++count;
        if(type!=1) continue;
        ++dynamic;
        if(!strcmp(state,"UNKNOWN_INITIAL_POSITION")) assert(out.positionState==0 && std::isnan(out.audio.x));
        else {
            assert(fields==10 && fabs(out.audio.x-x)<1e-5 && fabs(out.audio.y-y)<1e-5 && fabs(out.audio.z-z)<1e-5);
            if(!strcmp(state,"POSITION_UPDATE")) assert(out.positionState==1);
            else if(!strcmp(state,"CACHED_OBSERVED_POSITION")) assert(out.positionState==2);
            else assert(out.positionState==3);
        }
    }
    assert(feof(binary) && !std::getline(csv,line));fclose(binary);
    printf("PASS fixture records=%llu dynamic=%llu updates=%llu cached=%llu unknown=%llu creates=%llu\n",count,dynamic,cache.updates,cache.cached,cache.unknown,cache.creates);
}
