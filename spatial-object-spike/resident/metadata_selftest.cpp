#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "protocol.h"
#include "metadata_trace.h"
using Sanitize=HRESULT (__fastcall*)(const void*,uint32_t,uint32_t,void*);
int main() {
    HMODULE module=LoadLibraryExW(L"C:\\Windows\\System32\\audioeng.dll",nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module) { printf("Load error %lu\n",GetLastError());return 1; }
    auto* base=reinterpret_cast<BYTE*>(module);auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->FileHeader.TimeDateStamp!=kTimestamp || nt->OptionalHeader.SizeOfImage!=kImageSize) return 1;
    auto sanitize=reinterpret_cast<Sanitize>(base+0x2a2bc);
    for(uint32_t size:{uint32_t(0x20),uint32_t(0xb4)}) {
        BYTE input[0xb4]{},output[0xb4]{},info[24]{};
        uint32_t id=123456,frames=480,type=1,flags=6;
        float xyzv[]={.25f,-.5f,1.75f,.37f};
        memcpy(input,&id,4);memcpy(input+4,&frames,4);memcpy(input+8,&type,4);memcpy(input+12,&flags,4);memcpy(input+16,xyzv,16);
        if(size==0xb4) for(uint32_t i=32;i<size;++i)input[i]=static_cast<BYTE>(i);
        PropertyTrace before{},captured{};
        TraceBefore(before,input,size,480,output);
        HRESULT hr=sanitize(input,size,480,output);
        TraceAfter(before,hr,output);TraceSubmit(captured,before,info,output);
        if(FAILED(hr) || captured.flags!=15 || memcmp(captured.input,input,size) || memcmp(captured.sanitized,output,180) || memcmp(captured.submitted,output,180) || memcmp(captured.submitted+16,xyzv,16)) return 1;
        printf("PASS bytes=%u flags=%u xyz=(%.2f,%.2f,%.2f) volume=%.2f extraBytesPreserved=%d\n",size,captured.flags,xyzv[0],xyzv[1],xyzv[2],xyzv[3],size==0xb4?!memcmp(captured.submitted+32,input+32,size-32):1);
    }
    FreeLibrary(module);return 0;
}
