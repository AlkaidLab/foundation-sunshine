#pragma once
#include "cpp_mat_ipc_protocol.hpp"
#include "cpp_mat_stream_fake.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}
namespace mat_ipc {
using Json=nlohmann::json;namespace fs=std::filesystem;
class Handle {
    HANDLE value_{};
public:
    Handle()=default;explicit Handle(HANDLE h):value_(h){}
    ~Handle(){if(value_&&value_!=INVALID_HANDLE_VALUE)CloseHandle(value_);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value_(other.value_){other.value_=nullptr;}
    Handle& operator=(Handle&& other)noexcept{if(this!=&other){if(value_)CloseHandle(value_);value_=other.value_;other.value_=nullptr;}return *this;}
    HANDLE get()const{return value_;}
};
class View {
    Shared* value_{};
public:
    explicit View(HANDLE map){value_=static_cast<Shared*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));check(value_!=nullptr,"mapping view/size");}
    ~View(){UnmapViewOfFile(value_);}Shared& get(){return *value_;}
};
class Timer {
    Handle timer_;
public:
    Timer():timer_(CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS)){check(timer_.get()!=nullptr,"high-resolution timer unavailable");}
    void until(int64_t target){for(;;){auto left=target-qpc();if(left<=0)return;LARGE_INTEGER due{};due.QuadPart=-((left*10000000+hz()-1)/hz());check(SetWaitableTimer(timer_.get(),&due,0,nullptr,nullptr,FALSE)!=0,"timer set");check(WaitForSingleObject(timer_.get(),1000)==WAIT_OBJECT_0,"timer wait");}}
};
inline Json load(const fs::path& p){std::ifstream f(p);check(bool(f),"JSON read");Json j;f>>j;return j;}
inline void save(const fs::path& p,const Json& j){std::ofstream f(p);f<<j.dump(2)<<'\n';check(bool(f),"JSON write");}
inline std::string hashFile(const fs::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"hash read");AVSHA* s=av_sha_alloc();check(s&&av_sha_init(s,256)==0,"SHA init");std::array<uint8_t,65536>b;while(f){f.read(reinterpret_cast<char*>(b.data()),b.size());av_sha_update(s,b.data(),static_cast<size_t>(f.gcount()));}check(f.eof(),"hash I/O");uint8_t d[32];av_sha_final(s,d);av_free(s);std::ostringstream out;for(auto v:d)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(v);return out.str();}
template<class T>inline void write(std::ofstream& f,const T& v){f.write(reinterpret_cast<const char*>(&v),sizeof(v));check(bool(f),"binary write");}
inline void writePacket(std::ofstream& f,const Packet& p){write(f,p.header);for(unsigned n=0;n<p.header.calls;++n){write(f,p.calls[n].header);write(f,p.calls[n].pcm);}}
inline uint64_t creationTime(HANDLE process){FILETIME a,b,c,d;check(GetProcessTimes(process,&a,&b,&c,&d)!=0,"process identity time");return (static_cast<uint64_t>(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
inline std::wstring widen(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);check(n>0,"UTF8 path");std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),w.data(),n);return w;}
inline std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned slashes=0;for(auto c:s){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}out.append(slashes*2,L'\\');return out+L'\"';}
inline std::wstring executable(){std::wstring p(32768,0);auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size()));check(n&&n<p.size(),"exe path");p.resize(n);return p;}
inline std::wstring number(uint64_t n){return std::to_wstring(n);}
struct Child {Handle process;DWORD pid{};uint64_t created{};};
inline Child spawn(const std::vector<std::wstring>& args,std::vector<HANDLE> handles){
    auto exe=executable();std::wstring cmd=quote(exe);for(const auto& arg:args)cmd+=L" "+quote(arg);
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<uint8_t> memory(bytes);auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    check(InitializeProcThreadAttributeList(list,1,0,&bytes)!=0,"handle list init");
    struct Cleanup {LPPROC_THREAD_ATTRIBUTE_LIST p;~Cleanup(){DeleteProcThreadAttributeList(p);}} cleanup{list};
    check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),handles.size()*sizeof(HANDLE),nullptr,nullptr)!=0,"explicit inherited handles");
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=list;PROCESS_INFORMATION pi{};
    check(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=0,"spawn receiver");
    CloseHandle(pi.hThread);Child child{Handle(pi.hProcess),pi.dwProcessId,0};child.created=creationTime(child.process.get());return child;
}
struct Source {
    FileHeader header{};std::vector<uint8_t> bytes;std::vector<size_t> offsets;
    explicit Source(const fs::path& path){
        check(fs::file_size(path)<=16*1024*1024,"source size bound");std::ifstream f(path,std::ios::binary);check(bool(f),"source open");bytes.assign(std::istreambuf_iterator<char>(f),{});
        check(bytes.size()>=sizeof(header),"source header");memcpy(&header,bytes.data(),sizeof(header));
        check(memcmp(header.magic,"MATBRG1\0",8)==0&&header.version==1&&header.rate==48000&&header.frames==480&&header.records>=2&&header.records<=1000,"bridge source format");
        size_t at=sizeof(header);for(unsigned n=0;n<header.records;++n){check(at+sizeof(FrameHeader)<=bytes.size(),"frame header");FrameHeader h{};memcpy(&h,bytes.data()+at,sizeof(h));check(h.index==n&&h.frames==480&&h.calls<=64&&h.flags==unsigned(n+1==header.records),"source sequence/EOF");offsets.push_back(at);at+=sizeof(h)+h.calls*(sizeof(CallHeader)+quantum*4);check(at<=bytes.size(),"frame PCM");}check(at==bytes.size(),"source trailing bytes");
    }
    void copy(Packet& p,unsigned n,uint64_t domain)const{auto* at=bytes.data()+offsets.at(n);p.domain=domain;memcpy(&p.header,at,sizeof(p.header));at+=sizeof(p.header);for(unsigned j=0;j<p.header.calls;++j){memcpy(&p.calls[j].header,at,sizeof(CallHeader));at+=sizeof(CallHeader);memcpy(p.calls[j].pcm.data(),at,quantum*4);at+=quantum*4;}}
};
struct ProcessTime {Tick tick;int64_t start,finish;};
class TimedFake:public Encoder {
    Timer timer_;bool started_=false;int64_t next_=0;bool delayed_;
public:
    FakeEncoder fake;std::vector<ProcessTime> times;unsigned rebases=0;
    explicit TimedFake(bool delayed):delayed_(delayed){}
    void open(uint64_t domain)override{if(delayed_)timer_.until(qpc()+hz()*60/1000);fake.open(domain);}
    void submit(const Call& c,float v,float g)override{fake.submit(c,v,g);}
    void process(Tick t)override{auto current=qpc();if(!started_){started_=true;next_=current;}if(current-next_>hz()/500){next_=current;++rebases;}timer_.until(next_);auto start=qpc();next_+=hz()/100;if(delayed_&&t.phase==Phase::source&&t.sourceIndex==60)timer_.until(qpc()+hz()*40/1000);fake.process(t);times.push_back({t,start,qpc()});}
    void close(bool complete)override{fake.close(complete);}
};
inline void savePlan(const fs::path& path,const TimedFake& encoder){std::ofstream f(path,std::ios::binary);check(bool(f),"plan open");for(const auto& b:encoder.fake.blocks){write(f,static_cast<uint32_t>(b.tick.phase));write(f,b.tick.sourceIndex);write(f,static_cast<uint32_t>(b.calls.size()));for(const auto& c:b.calls){write(f,c.header.id);write(f,c.header.flags);write(f,c.header.xyz);write(f,c.pcm);}}f.close();check(bool(f),"plan close");}
} // namespace mat_ipc
