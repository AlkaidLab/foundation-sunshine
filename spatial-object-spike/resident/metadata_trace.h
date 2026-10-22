#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
constexpr uint32_t kTraceMagic=0x4d545243, kTraceFloatOffset=1664;
#pragma pack(push,1)
struct PropertyTrace {
    uint32_t magic, flags, inputBytes, maxFrames;
    int32_t sanitizeResult;
    uint32_t reserved;
    uint64_t sourcePointer, destinationPointer;
    int64_t sanitizeQpc;
    BYTE streamInfo[24];
    BYTE input[0xb4], sanitized[0xb4], submitted[0xb4];
    BYTE padding[28];
};
#pragma pack(pop)
static_assert(sizeof(PropertyTrace)==640);
// Flags: raw copied=1, output copied=2, destination/object binding=4,
// submitted properties copied=8. Full submitted 0xb4 is read only if bound.
static void TraceBefore(PropertyTrace& t,const void* input,uint32_t bytes,uint32_t maxFrames,void* output) {
    memset(&t,0,sizeof(t)); t.magic=kTraceMagic; t.inputBytes=bytes; t.maxFrames=maxFrames;
    t.sourcePointer=reinterpret_cast<uint64_t>(input); t.destinationPointer=reinterpret_cast<uint64_t>(output);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now); t.sanitizeQpc=now.QuadPart;
    if(input && (bytes==0x20 || bytes==0xb4)) { memcpy(t.input,input,bytes);t.flags|=1; }
}
static void TraceAfter(PropertyTrace& t,HRESULT hr,const void* output) {
    t.sanitizeResult=hr;
    if(SUCCEEDED(hr) && output) { memcpy(t.sanitized,output,0xb4);t.flags|=2; }
}
static void TraceSubmit(PropertyTrace& out,const PropertyTrace& prior,const void* info,const void* props) {
    out=prior;
    out.magic=kTraceMagic;
    bool match=(prior.flags&3)==3 && prior.destinationPointer==reinterpret_cast<uint64_t>(props) && !memcmp(prior.sanitized,props,4);
    if(match) out.flags|=4;
    memcpy(out.streamInfo,info,24);
    memcpy(out.submitted,props,match?0xb4:0x20);out.flags|=8;
}
