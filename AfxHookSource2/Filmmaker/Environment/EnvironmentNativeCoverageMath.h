#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "EnvironmentDxbcHash.h"
namespace Filmmaker::Environment {
// Preserve the complete native PS (including discard and SV_Coverage), replacing
// only RGB on every unconditional return. Alpha remains native for alpha-to-coverage.
// Recompute the container checksum; D3D11 validates the result before use.
inline bool NativeCoverageProgram(const void* data,size_t size,std::vector<uint8_t>& output,bool black=false) {
    output.clear();
    if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t offset){uint32_t v;std::memcpy(&v,p+offset,4);return v;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=size)return false;
    const auto count=word(28);if(count>(size-32)/4)return false;
    output.assign(p,p+32+count*4);
    auto put=[&](size_t offset,uint32_t v){std::memcpy(output.data()+offset,&v,4);};
    auto append=[&](uint32_t v){auto offset=output.size();output.resize(offset+4);put(offset,v);};
    bool patched=false;
    for(uint32_t chunk=0;chunk<count;++chunk){
        const size_t start=word(32+chunk*4);
        if(start<32+count*4||start>size-8)return false;
        const size_t bytes=word(start+4);if(bytes>size-start-8)return false;
        const size_t destination=output.size();put(32+chunk*4,uint32_t(destination));
        if(std::memcmp(p+start,"SHEX",4)&&std::memcmp(p+start,"SHDR",4)){
            output.insert(output.end(),p+start,p+start+8+bytes);continue;
        }
        if(patched||bytes<8||bytes%4||word(start+8)!=0x50||word(start+12)!=bytes/4)return false;
        output.insert(output.end(),p+start,p+start+16);
        bool color=false,returned=false;
        for(size_t i=2;i<bytes/4;){
            const auto token=word(start+8+i*4),op=token&0x7ff;
            size_t length=(token>>24)&127;
            if(op==53){if(i+1>=bytes/4)return false;length=word(start+12+i*4);if(length<2)return false;}
            if(!length||length>bytes/4-i)return false;
            // Calls, conditional returns and interfaces need control-flow analysis.
            if(op==4||op==5||op==63||op==120)return false;
            if(op==101){
                if(length==3&&word(start+12+i*4)==0x001020f2&&word(start+16+i*4)==0)color=true;
                else return false;
            }
            if(op==62){
                if(length!=1)return false;
                // Black isolates color added AFTER material shading. The native
                // alpha/discard/coverage remain identical to the white diagnostic.
                const uint32_t rgb=black?0u:0x3f800000u;
                for(uint32_t v:{0x08000036u,0x00102072u,0u,0x00004002u,rgb,rgb,rgb,0x3f800000u})append(v);
                returned=true;
            }
            output.insert(output.end(),p+start+8+i*4,p+start+8+(i+length)*4);i+=length;
        }
        if(!color||!returned)return false;
        put(destination+4,uint32_t(output.size()-destination-8));
        put(destination+12,uint32_t((output.size()-destination-8)/4));patched=true;
    }
    put(24,uint32_t(output.size()));
    DxbcHash::ComputeHashRetail(output.data()+20,uint32_t(output.size()-20),output.data()+4);
    return patched;
}
}
