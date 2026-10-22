#include "quantum_v4_tap.cpp"
#include "quantum_v4_assembler.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>
namespace fs=std::filesystem;
static void require4(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
template<class F>static void rejects4(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}require4(rejected,"expected rejection");}
struct Properties4 {uint32_t id,frames,type,flags;float x,y,z,volume;};
static_assert(sizeof(Properties4)==32);
struct Connection4 {void* buffer;uint32_t frames,flags;};
struct Encoder4 {void** table;void* implementation;};
struct Renderer4 {void* unused;Encoder4* encoder;};
static unsigned sourceObjects4=1,sourceFlags4=6,submitCalls4=0,processCalls4=0,destroyCalls4=0,resetCalls4=0;
static float sourceX4=1;
static HRESULT submitResult4=S_OK,resetResult4=S_OK;
static bool mutate4=true,reenter4=false,omitProcess4=false,checkPending4=false;
static bool stopInOriginal4=false;
static bool raiseInOriginal4=false;
static q4::Assembler* duringOriginal4=nullptr;
static uint64_t duringRead4=0;
static float lastMutated4=0;
static Properties4 lastProperties4{};
static HRESULT __fastcall FakeSubmit4(void* encoder,const void* info,const void* properties,const float* samples,uint32_t frames,float gain) {
    ++submitCalls4;
    if(raiseInOriginal4){raiseInOriginal4=false;RaiseException(0xe0040001,0,0,nullptr);}
    if(checkPending4&&duringOriginal4) {
        auto* ring=currentRing4();q4::Quantum out;
        while(duringRead4<static_cast<uint64_t>(ring->write)) {
            auto& slot=ring->slots[duringRead4++%kSlots];require4(!duringOriginal4->push(slot.header,slot.pcm,out),"worker committed a pending snapshot before original returned");
        }
    }
    if(reenter4){reenter4=false;HookSubmit4(encoder,info,properties,samples,frames,gain);}
    if(stopInOriginal4){stopInOriginal4=false;InterlockedExchange(&currentRing4()->stop,1);}
    if(mutate4) {
        auto* p=const_cast<Properties4*>(static_cast<const Properties4*>(properties));
        if(samples) {
            float combined=p->volume*gain;auto* writable=const_cast<float*>(samples);
            for(unsigned i=0;i<frames;++i)writable[i]*=combined;
            lastMutated4=writable[frames-1];
        }
        p->x=99;p->volume=.3f;p->flags=15;lastProperties4=*p;
        uint64_t changed=999;memcpy(static_cast<BYTE*>(const_cast<void*>(info))+4,&changed,8);
    }
    return submitResult4;
}
static HRESULT __fastcall FakeProcess4(void*,uint32_t,uint32_t,BYTE*,uint32_t* bytes){++processCalls4;*bytes=0;return S_OK;}
static HRESULT __fastcall FakeExtended4(void*,const void*,const void*,const void*,const float*,uint32_t,float){return S_OK;}
static HRESULT __fastcall FakeReset4(void*){++resetCalls4;return resetResult4;}
static void __fastcall FakeDestroy4(void*){++destroyCalls4;}
static void __fastcall FakeRender4(void* object,uint32_t,void* input,uint32_t,void*) {
    auto* renderer=static_cast<Renderer4*>(object);auto* connection=*static_cast<Connection4**>(input);
    for(unsigned n=0;n<sourceObjects4;++n) {
        uint8_t info[12]{};uint64_t host=77;memcpy(info+4,&host,8);
        Properties4 p{42+n,connection->frames,1,sourceFlags4,sourceX4,2,3,.5f};float samples[kSamples];
        for(unsigned i=0;i<connection->frames;++i)samples[i]=float(i)*.001f;
        auto result=HookSubmit4(renderer->encoder,info,&p,samples,connection->frames,.25f);
        require4(result==submitResult4,"hook changed original HRESULT");
    }
    if(!omitProcess4){uint32_t produced=0;HookProcess4(renderer->encoder,connection->frames,30720,nullptr,&produced);}
}
struct Fixture4 {
    q4::Ring* ring=nullptr;void* table[7]{};
    Encoder4 a{},b{};Renderer4 rendererA{},rendererB{};
    Fixture4() {
        ring=static_cast<q4::Ring*>(VirtualAlloc(nullptr,sizeof(q4::Ring),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));require4(ring,"allocate own ring");
        table[2]=reinterpret_cast<void*>(FakeSubmit4);table[3]=reinterpret_cast<void*>(FakeExtended4);table[5]=reinterpret_cast<void*>(FakeProcess4);table[6]=reinterpret_cast<void*>(FakeReset4);
        targets4[1]=table[2];targets4[2]=table[3];targets4[3]=table[5];targets4[4]=table[6];
        a={table,reinterpret_cast<void*>(0x111100)};b={table,reinterpret_cast<void*>(0x222200)};
        rendererA={nullptr,&a};rendererB={nullptr,&b};
        originalRender4=FakeRender4;originalSubmit4=FakeSubmit4;originalExtended4=FakeExtended4;
        originalProcess4=FakeProcess4;originalReset4=FakeReset4;originalDestroy4=FakeDestroy4;
        sourceObjects4=1;sourceFlags4=6;sourceX4=1;submitCalls4=processCalls4=destroyCalls4=resetCalls4=0;
        submitResult4=resetResult4=S_OK;mutate4=true;reenter4=omitProcess4=checkPending4=stopInOriginal4=raiseInOriginal4=false;duringOriginal4=nullptr;duringRead4=0;
        recorder4.start(ring);InterlockedExchangePointer(&current4,ring);
    }
    ~Fixture4(){InterlockedExchangePointer(&current4,nullptr);VirtualFree(ring,0,MEM_RELEASE);}
    void render(Renderer4& renderer,uint32_t frames=480) {Connection4 connection{nullptr,frames,1};void* inputs[]={&connection};HookRender4(&renderer,1,inputs,0,nullptr);}
    void finish(){recorder4.finish();}
    std::vector<q4::Quantum> parse(){q4::Assembler parser;std::vector<q4::Quantum> output;for(LONG64 i=0;i<ring->write;++i){q4::Quantum q;auto& slot=ring->slots[i%kSlots];if(parser.push(slot.header,slot.pcm,q))output.push_back(std::move(q));}parser.finish();return output;}
};
static bool catchOriginalSeh4(Renderer4* renderer) {
    bool caught=false;Connection4 connection{nullptr,480,1};void* inputs[]={&connection};
    __try {HookRender4(renderer,1,inputs,0,nullptr);}
    __except(GetExceptionCode()==0xe0040001?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=true;}
    return caught;
}
struct Event4 {q4::Header header;std::vector<float> pcm;};
static std::vector<Event4> events4(Fixture4& f) {
    std::vector<Event4> result;
    for(LONG64 i=0;i<f.ring->write;++i){const auto& s=f.ring->slots[i];result.push_back({s.header,{s.pcm,s.pcm+s.header.audio.frames}});}
    return result;
}
static std::vector<q4::Quantum> parseEvents4(std::vector<Event4> events) {
    q4::Assembler parser;std::vector<q4::Quantum> output;
    for(size_t i=0;i<events.size();++i){auto& event=events[i];event.header.audio.sequence=i;event.header.audio.qpc=1000+i;q4::Quantum q;if(parser.push(event.header,event.pcm.data(),q))output.push_back(std::move(q));}
    parser.finish();return output;
}
int main(int argc,char** argv) {
    unsigned tests=0;
    auto test=[&](const char* name,auto fn){try{fn();++tests;}catch(...){printf("FAILED %s\n",name);throw;}};
    try {
        test("before_PCM_and_metadata_survive_original_inplace_mutation",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto q=f.parse();require4(q.size()==1&&q[0].objects.size()==1,"missing pass");auto& o=q[0].objects[0];require4(lastProperties4.x==99&&lastMutated4!=o.pcm.back(),"fake original did not mutate");require4(o.source.audio.host==77&&o.source.audio.x==1&&o.source.audio.volume==.5f&&o.source.properties==6,"post-call metadata leaked");for(unsigned i=0;i<480;++i)require4(o.pcm[i]==float(i)*.001f,"post-gain PCM leaked");require4(o.resultSeen&&o.source.result==E_PENDING&&submitCalls4==1&&processCalls4==1,"status not separately paired");});
        test("worker_cannot_commit_until_result_and_end",[]{Fixture4 f;q4::Assembler parser;duringOriginal4=&parser;checkPending4=true;f.render(f.rendererA);f.finish();unsigned completed=0;for(uint64_t i=duringRead4;i<static_cast<uint64_t>(f.ring->write);++i){auto& s=f.ring->slots[i];q4::Quantum q;completed+=parser.push(s.header,s.pcm,q);}parser.finish();require4(completed==1,"incomplete or duplicate commit");});
        test("normal_stop_drains_admitted_pass_and_admits_no_new_pass",[]{Fixture4 f;stopInOriginal4=true;f.render(f.rendererA);auto events=f.ring->write;require4(f.ring->stop&&!f.ring->invalid&&captureAdmissions4==0,"stop interrupted admitted pass");f.render(f.rendererA);require4(f.ring->write==events&&!f.ring->invalid,"new pass admitted after stop");f.finish();auto q=f.parse();require4(q.size()==1&&q[0].objects[0].resultSeen,"stop lost paired return or Process");});
        test("original_SEH_propagates_but_releases_pass_TLS_and_lifetime_counters",[]{Fixture4 f;raiseInOriginal4=true;require4(catchOriginalSeh4(&f.rendererA),"original exception swallowed");require4(active4==0&&captureAdmissions4==0&&pass4==nullptr&&f.ring->stop&&f.ring->invalid,"exception leaked capture lifetime");f.finish();rejects4([&]{f.parse();});});
        test("new_renderer_and_encoder_both_captured",[]{Fixture4 f;f.render(f.rendererA);sourceX4=9;f.render(f.rendererB);f.finish();auto q=f.parse();require4(q.size()==2&&q[0].domain!=q[1].domain&&q[0].sampleStart==0&&q[1].sampleStart==0,"domains joined");require4(q[0].objects[0].state.audio.x==1&&q[1].objects[0].state.audio.x==9,"cache cross-talk");});
        test("reset_same_address_new_domain_unknown_XYZ",[]{Fixture4 f;f.render(f.rendererA);HookReset4(&f.a);sourceFlags4=2;sourceX4=0;f.render(f.rendererA);f.finish();auto q=f.parse();require4(q.size()==2&&q[0].domain!=q[1].domain&&q[1].sampleStart==0&&q[1].objects[0].state.positionState==0&&std::isnan(q[1].objects[0].state.audio.x)&&resetCalls4==1,"reset reused stale coordinates");});
        test("destroy_same_addresses_new_generation",[]{Fixture4 f;f.render(f.rendererA);HookDestroy4(&f.a);sourceFlags4=2;f.render(f.rendererA);f.finish();auto q=f.parse();require4(q.size()==2&&q[0].domain!=q[1].domain&&q[1].objects[0].state.positionState==0&&destroyCalls4==1,"destroy/address reuse merged");});
        test("implementation_cookie_change_splits_domain",[]{Fixture4 f;f.render(f.rendererA);f.a.implementation=reinterpret_cast<void*>(0x333300);f.render(f.rendererA);f.finish();auto q=f.parse();require4(q.size()==2&&q[0].domain!=q[1].domain,"implementation cookie rebound");});
        test("failed_reset_invalidates_without_claiming_native_success",[]{Fixture4 f;f.render(f.rendererA);resetResult4=E_FAIL;require4(HookReset4(&f.a)==E_FAIL,"reset result changed");sourceFlags4=2;f.render(f.rendererA);f.finish();auto q=f.parse();require4(q[1].objects[0].state.positionState==0,"failed reset kept stale state");});
        test("empty_pass_advances_only_its_own_clock",[]{Fixture4 f;f.render(f.rendererA);sourceObjects4=0;f.render(f.rendererA,240);f.render(f.rendererB,720);f.finish();auto q=f.parse();require4(q.size()==3&&q[1].sampleStart==480&&q[1].frames==240&&q[1].objects.empty()&&q[2].sampleStart==0,"empty/cross-domain sample clock");});
        test("failed_submit_preserves_hresult_and_rejects_snapshot",[]{Fixture4 f;submitResult4=E_FAIL;f.render(f.rendererA);require4(f.ring->stop&&f.ring->invalid&&submitCalls4==1,"failed original swallowed");f.finish();rejects4([&]{f.parse();});});
        test("reentrant_submit_fails_closed_without_overwriting",[]{Fixture4 f;reenter4=true;f.render(f.rendererA);require4(f.ring->stop&&f.ring->invalid&&submitCalls4==2,"reentry not detected/original not forwarded");f.finish();rejects4([&]{f.parse();});});
        test("missing_Process_never_commits",[]{Fixture4 f;omitProcess4=true;f.render(f.rendererA);require4(f.ring->stop&&f.ring->invalid,"missing marker silently accepted");f.finish();rejects4([&]{f.parse();});});
        test("queue_full_never_overwrites_pre_snapshot",[]{Fixture4 f;f.ring->write=kSlots;f.render(f.rendererA);require4(f.ring->droppedFull&&f.ring->stop&&f.ring->write==kSlots,"queue full overwrite");});
        test("publication_contention_never_waits",[]{Fixture4 f;f.ring->writer=1;f.render(f.rendererA);f.ring->writer=0;require4(f.ring->droppedBusy&&f.ring->stop&&!f.ring->write,"writer lock waited or overwrote");});
        test("unbound_arming_is_counted_then_post_arming_fails",[]{Fixture4 f;recorder4.unbound();require4(f.ring->ignoredBeforeFirstRender==1&&!f.ring->stop,"arming not distinguished");f.render(f.rendererA);recorder4.unbound();require4(f.ring->unboundAfterArming==1&&f.ring->stop,"unbound post-arm silently ignored");});
        test("same_encoder_concurrent_pass_rejected",[]{Fixture4 f;q4::Pass first,second;require4(recorder4.begin(1,2,3,480,first),"first pass");require4(!recorder4.begin(1,2,3,480,second)&&f.ring->stop,"concurrent encoder not rejected");recorder4.end(first);});
        test("active_domain_registry_capacity_bounded",[]{Fixture4 f;for(unsigned i=0;i<q4::domainCapacity;++i){q4::Pass p;require4(recorder4.begin(100+i,200+i,300+i,480,p),"early registry full");uint32_t bytes=0;recorder4.processed(p,480,30720,&bytes,S_OK);recorder4.end(p);}q4::Pass extra;require4(!recorder4.begin(999,999,999,480,extra)&&f.ring->stop,"unbounded registry growth");});
        test("per_pass_object_capacity_drops_explicitly_but_still_forwards_original",[]{Fixture4 f;sourceObjects4=q4::objectCapacity+1;f.render(f.rendererA);require4(f.ring->stop&&f.ring->invalid&&submitCalls4==q4::objectCapacity+1,"object capacity swallowed original or grew without bound");f.finish();rejects4([&]{f.parse();});});
        test("retired_registry_slots_can_be_reused_without_reusing_domain_ID",[]{Fixture4 f;for(unsigned i=0;i<20;++i){f.render(f.rendererA);HookDestroy4(&f.a);}f.finish();auto q=f.parse();require4(q.size()==20&&q.front().domain==1&&q.back().domain==20,"lifetime slot/ID reuse");});
        test("zero_frame_render_fails_instead_of_guessing",[]{Fixture4 f;sourceObjects4=0;f.render(f.rendererA,0);require4(f.ring->invalid&&f.ring->stop,"zero render frame semantics guessed");});
        test("worker_rejects_missing_return_even_with_resequenced_transport",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e.erase(e.begin()+3);rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_duplicate_return",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e.insert(e.begin()+4,e[3]);rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_wrong_call_id",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[3].header.call=2;rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_failed_return",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[3].header.result=E_FAIL;rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_post_call_snapshot_tag",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[2].header.flags=q4::PostCall;rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_render_Process_frame_mismatch",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[4].header.inputFrames=240;rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_domain_end_during_open_pass",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);std::swap(e[4],e[6]);rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_cross_thread_result",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[3].header.audio.tid+=1;rejects4([&]{parseEvents4(e);});});
        test("worker_rejects_EOF_without_domain_closure",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e.pop_back();rejects4([&]{parseEvents4(e);});});
        test("short_packet_offset_stays_unknown",[]{Fixture4 f;f.render(f.rendererA);f.finish();auto e=events4(f);e[2].header.audio.frames=240;e[2].pcm.resize(240);auto q=parseEvents4(e);require4(!q[0].fullSpanLayoutReady&&q[0].objects[0].offset==UINT32_MAX,"short offset fabricated");});
        test("interleaved_domains_and_threads_keep_independent_clocks",[]{
            Fixture4 f;f.render(f.rendererA);sourceX4=9;f.render(f.rendererB);f.finish();auto e=events4(f);
            const unsigned order[]={0,1,2,6,7,8,9,10,11,3,4,5,12,13};std::vector<Event4> mixed;
            for(auto index:order){auto item=e.at(index);item.header.audio.tid=item.header.domain==1?101:202;mixed.push_back(std::move(item));}
            auto q=parseEvents4(mixed);require4(q.size()==2&&q[0].domain==2&&q[1].domain==1&&q[0].sampleStart==0&&q[1].sampleStart==0,"interleaved domains collapsed into one clock");
        });
        if(argc==3&&std::string(argv[1])=="--fixture-output") {
            fs::path root=fs::absolute(argv[2]);require4(!fs::exists(root)&&fs::create_directories(root),"fixture output must be new");
            Fixture4 f;sourceObjects4=2;f.render(f.rendererA);sourceObjects4=1;sourceX4=9;f.render(f.rendererB);
            HookReset4(&f.a);sourceFlags4=2;sourceX4=0;f.render(f.rendererA);HookDestroy4(&f.b);HookDestroy4(&f.a);f.finish();
            auto q=f.parse();require4(q.size()==3&&f.ring->domainsOpened==3&&f.ring->domainsClosed==3,"fixture domains");
            std::ofstream output(root/"events.bin",std::ios::binary);
            for(LONG64 i=0;i<f.ring->write;++i){const auto& slot=f.ring->slots[i];output.write(reinterpret_cast<const char*>(&slot.header),sizeof(slot.header));output.write(reinterpret_cast<const char*>(slot.pcm),slot.header.audio.frames*4);}output.close();require4(bool(output),"fixture write");
            std::ofstream description(root/"expected.json");description<<"{\"source_kind\":\"synthetic-mutating-originals\",\"protocol_version\":4,\"header_bytes\":160,\"records\":"<<f.ring->write<<",\"domains\":3,\"quanta\":3,\"objects\":4,\"native_verified\":false,\"pcm\":\"F32(sample_index * 0.001f), before mutation\",\"volume\":0.5,\"call_gain\":0.25}\n";
            description.close();require4(bool(description),"fixture description write");
            std::ofstream format(root/"format.json");format<<"{\"version\":4,\"header_bytes\":160,\"source_kind\":\"synthetic-mutating-originals\",\"pcm_stage\":\"before-original-submit\",\"native_verified\":false}\n";format.close();require4(bool(format),"fixture format write");
        }
        printf("{\"passed\":true,\"tests\":%u,\"scope\":\"own-process real hook wrappers with deliberately mutating fake originals\",\"audiodg_accessed\":false,\"native_verified\":false}\n",tests);return 0;
    } catch(const std::exception& e){printf("FAILED %s\n",e.what());return 1;}
}
