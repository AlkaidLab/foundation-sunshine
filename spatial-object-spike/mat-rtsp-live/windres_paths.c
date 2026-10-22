// Build-only adapter for Sunshine's already-configured windows.rc. It includes
// only system winver.h and has literal version/icon data; none of the C++ target
// -D/-I options are used. Current windres corrupts their long quoted command line
// when invoking GCC. Compile this ONE resource without those unrelated options.
// Reject all other resource files rather than applying this policy generally.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <shlwapi.h>
#include <process.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
int wmain(int argc,wchar_t** argv) {
    if(argc<3)return 2;
    const wchar_t* input=argv[argc-2];const wchar_t* base=input;
    for(const wchar_t* p=input;*p;p++)if(*p==L'/'||*p==L'\\')base=p+1;
    if(_wcsicmp(base,L"windows.rc"))return 2;
    // windres subsequently spawns GCC itself. Its current forwarding loses the
    // quoting again, so use the existing 8.3 filename for that one input too.
    wchar_t short_input[32768];
    DWORD short_length=GetShortPathNameW(input,short_input,32768);
    if(!short_length || short_length>=32768 || wcschr(short_input,L' ')) {
        fwprintf(stderr,L"Resource build needs an existing space-free input alias\n");return 2;
    }
    wchar_t* filtered[]={L"C:/msys64/ucrt64/bin/windres.exe",L"-O",L"coff",L"--use-temp-file",
        short_input,argv[argc-1],NULL};
    argv=filtered;argc=6;
    // _wspawnv concatenates argv without CRT quoting. Build a correctly quoted
    // command line so source filenames and -D string literals survive forwarding.
    wchar_t* command=calloc(32768,sizeof(wchar_t));size_t n=0;
    if(!command)return 2;
#define PUT(c) do{if(n>=32766)return 2;command[n++]=(c);}while(0)
    for(int i=0;i<argc;i++) {
        if(i)PUT(L' ');PUT(L'"');
        const wchar_t* p=argv[i];
        while(*p) {
            unsigned slashes=0;while(*p==L'\\'){slashes++;p++;}
            unsigned count=(*p==L'"'||!*p)?slashes*2:slashes;
            for(unsigned j=0;j<count;j++)PUT(L'\\');
            if(!*p)break;
            if(*p==L'"')PUT(L'\\');PUT(*p++);
        }
        PUT(L'"');
    }
    STARTUPINFOW startup={.cb=sizeof(startup)};PROCESS_INFORMATION process={0};
    if(!CreateProcessW(argv[0],command,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process))return 2;
    WaitForSingleObject(process.hProcess,INFINITE);DWORD result=2;GetExitCodeProcess(process.hProcess,&result);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return (int)result;
}
