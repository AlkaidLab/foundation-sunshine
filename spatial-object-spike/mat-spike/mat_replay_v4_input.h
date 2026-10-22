#pragma once
#include "mat_replay_v4_protocol.h"
#include <cmath>
#include <cstring>

struct ReplayProperties {uint32_t id,offset,type,flags;float x,y,z,volume;BYTE extra[0xb4-32];};
static_assert(sizeof(ReplayProperties)==0xb4);
using ReplaySubmit=HRESULT(__fastcall*)(void*,const void*,const void*,const float*,UINT32,float);
// Same routine is used by native helper and own-process fake interface tests.
inline HRESULT SubmitReplayBlock(const ReplayBlock& block,void* encoder,const void* stream,ReplaySubmit submit){
    for(uint32_t i=0;i<block.count;++i){const auto& in=block.objects[i];ReplayProperties p{};p.id=in.id;p.type=1;p.flags=in.flags;p.x=in.xyz[0];p.y=in.xyz[1];p.z=in.xyz[2];p.volume=1;
        HRESULT hr=submit(encoder,stream,&p,in.pcm,480,1.f);if(hr!=S_OK)return hr;
    }return S_OK;
}
inline bool ReplayInputsValid(const ReplayState* s){
    if(!s||s->magic!=kReplayMagic||s->version!=kReplayVersion)return false;
    uint32_t total=0;
    for(uint32_t ci=0;ci<kReplayCases;++ci){const auto& c=s->cases[ci];
        if(c.domain!=ci+1||c.sourceBlocks<1||c.sourceBlocks>350||c.prerollBlocks!=20||c.drainBlocks!=30||c.paceMs!=10
            ||c.prerollBefore>=c.sourceBlocks||c.blocks!=c.sourceBlocks+50||c.blocks>kReplayBlocks)return false;
        total+=c.blocks;uint32_t ids[64]{},alive[64]{},used=0,active=0,source=0,warm=0,drain=0;
        for(uint32_t b=0;b<c.blocks;++b){const auto& block=c.input[b];if(block.count>20)return false;
            uint32_t phase=b<c.prerollBefore?2:b<c.prerollBefore+20?1:b<c.prerollBlocks+c.sourceBlocks?2:3;
            if(block.phase!=phase)return false;
            if(phase==2){if(block.sourceIndex!=source++)return false;}
            else if(block.sourceIndex!=UINT32_MAX)return false;
            if(phase==1)++warm;if(phase==3){++drain;if(block.count||active)return false;}
            if(phase==2&&block.sourceIndex<c.prerollBefore&&block.count)return false;
            if(phase==1&&block.count!=c.input[c.prerollBefore].count)return false;
            for(uint32_t o=0;o<block.count;++o){const auto& object=block.objects[o];
                if(!object.id||(object.flags!=7&&object.flags!=6&&object.flags!=8))return false;
                for(uint32_t p=0;p<o;++p)if(block.objects[p].id==object.id)return false;
                for(float x:object.xyz)if(!std::isfinite(x)||fabsf(x)>10000)return false;
                for(float x:object.pcm)if(!std::isfinite(x)||fabsf(x)>1||(object.flags==8&&x!=0)||(phase==1&&x!=0))return false;
                if(phase==1){const auto& first=c.input[c.prerollBefore].objects[o];if(object.id!=first.id||memcmp(object.xyz,first.xyz,12)||object.flags!=(warm==1?7u:6u))return false;}
                if(phase==2&&block.sourceIndex==c.prerollBefore){const auto& first=c.input[c.prerollBefore];if(block.count!=first.count||object.id!=first.objects[o].id||object.flags!=6||memcmp(object.xyz,first.objects[o].xyz,12))return false;}
                if(phase==2&&object.flags==7)return false;
                uint32_t slot=0;while(slot<used&&ids[slot]!=object.id)++slot;
                if(slot==used){if(used==64||object.flags!=7)return false;ids[used++]=object.id;}
                if(object.flags==7){if(alive[slot]||++active>20)return false;alive[slot]=1;}
                else{if(!alive[slot])return false;if(object.flags==8){alive[slot]=0;--active;}}
            }
        }
        if(source!=c.sourceBlocks||warm!=20||drain!=30||active||!c.input[c.prerollBefore].count)return false;
    }
    return total<=650; // <=6.5s scheduled work; no unbounded fixture growth.
}
