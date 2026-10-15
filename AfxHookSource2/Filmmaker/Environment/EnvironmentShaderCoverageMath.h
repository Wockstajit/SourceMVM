#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Filmmaker::Environment {
// Exact, audited CS2 depth-prepass opacity program: t12.a at TEXCOORD0,
// s0, cutoff b0[0].w, edge strength b0[1].x, MSAA mode b1[1].z.
// Matching the instruction stream, rather than a color shader's alpha=1,
// matters because CS2 applies cutouts before its opaque color pass.
// Unknown versions retain the existing fallback until their formula is verified.
inline bool NativeDepthCutoutCoverage(const void* code,size_t size) {
    if(!code||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const unsigned char*>(code);
    auto word=[&](size_t offset){uint32_t value;std::memcpy(&value,p+offset,4);return value;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=size)return false;
    const uint32_t chunks=word(28);if(chunks>(size-32)/4)return false;
    bool found=false,matched=false;
    for(uint32_t i=0;i<chunks;++i){
        size_t offset=word(32+i*4);if(offset<32+size_t(chunks)*4||offset>size-8)return false;
        const size_t bytes=word(offset+4);if(bytes>size-offset-8)return false;
        if(std::memcmp(p+offset,"SHEX",4)&&std::memcmp(p+offset,"SHDR",4))continue;
        if(found)return false;found=true;
        if(bytes!=844)continue;
        uint64_t hash=14695981039346656037ull;
        for(size_t b=0;b<bytes;++b){hash^=p[offset+8+b];hash*=1099511628211ull;}
        matched=hash==0x78fdee0c215ee4f6ull;
    }
    return found&&matched;
}
// Bounded DXBC token walk without compiler/disassembler calls on native creation
// workers. Unknown containers/programs fail closed here.
// Token encoding: Microsoft DirectXShaderCompiler d3d12TokenizedProgramFormat.hpp.
inline bool OpaquePixelShaderCoverage(const void* code,size_t size,uint32_t* rejection=nullptr,bool* constantAlphaOne=nullptr,uint32_t* vertexAlphaInput=nullptr) {
    auto reject=[&](uint32_t reason){if(rejection)*rejection=reason;return false;};
    if(rejection)*rejection=0;
    if(constantAlphaOne)*constantAlphaOne=false;
    if(vertexAlphaInput)*vertexAlphaInput=~0u;
    if(!code||size<32||size>16*1024*1024)return reject(1);
    const auto* data=static_cast<const unsigned char*>(code);
    auto word=[&](size_t offset){uint32_t value;std::memcpy(&value,data+offset,4);return value;};
    if(std::memcmp(data,"DXBC",4)||word(24)!=size)return reject(1);
    const uint32_t chunks=word(28);
    if(chunks>(size-32)/4)return reject(1);
    bool found=false,alphaOne=false;uint32_t alphaInput=~0u;
    for(uint32_t chunk=0;chunk<chunks;++chunk){
        size_t start=word(32+chunk*4);
        if(start<32+size_t(chunks)*4||start>size-8)return reject(1);
        const size_t bytes=word(start+4);
        if(bytes>size-start-8)return reject(1);
        if(std::memcmp(data+start,"SHDR",4)&&std::memcmp(data+start,"SHEX",4))continue;
        if(found||bytes<8||bytes%4)return reject(1);
        found=true;start+=8;
        const uint32_t version=word(start),length=word(start+4),major=(version>>4)&15;
        if((version>>16)!=0||(major!=4&&major!=5)||length!=bytes/4)return reject(1);
        bool straightLine=true,previousAlphaOne=false;
        // A terminal, unconditional MOV can preserve native vertex coverage even
        // when RGB lighting contains branches. Require one final return and no
        // calls/conditional returns; track lexical nesting so a branch-local
        // assignment can never be mistaken for an unconditional one.
        bool terminalSafe=true;unsigned nesting=0;
        uint32_t previous[5]{},beforePrevious[5]{};size_t previousCount=0,beforePreviousCount=0;
        for(size_t i=2;i<length;){
            const uint32_t token=word(start+i*4),opcode=token&0x7ff;
            size_t count=(token>>24)&0x7f;
            constexpr uint32_t discard=13,customData=53,dclOutput=101,dclOutputSgv=102,dclOutputSiv=103;
            if(opcode==discard)return reject(13);
            if(opcode==customData){
                if(i+1>=length)return reject(1);
                count=word(start+(i+1)*4);
                if(count<2)return reject(1);
            }
            if(!count||count>length-i)return reject(1);
            if(opcode==dclOutput||opcode==dclOutputSgv||opcode==dclOutputSiv){
                size_t operand=i+1;uint32_t extended=token;
                while(extended&0x80000000u){
                    if(operand>=i+count)return reject(1);
                    extended=word(start+operand++*4);
                }
                if(operand>=i+count)return reject(1);
                const uint32_t type=(word(start+operand*4)>>12)&0xff;
                // Only ordinary color outputs can be replayed as opaque depth.
                // Coverage, depth and stencil outputs require their native PS.
                if(type!=2)return reject(1000+type);
            }

            // A blend state does not imply transparent coverage: some native PS
            // variants end with an unconditional alpha=1. Prove only a simple straight-line
            // program whose last instruction before its sole RET writes literal one
            // into output zero's alpha. Unknown opcodes/control flow fail this proof.
            const bool arithmetic=opcode<=1||(opcode>=11&&opcode<=12)||(opcode>=14&&opcode<=17)||
                (opcode>=24&&opcode<=30)||(opcode>=32&&opcode<=43)||(opcode>=45&&opcode<=47)||
                (opcode>=49&&opcode<=61)||(opcode>=64&&opcode<=75)||(opcode>=77&&opcode<=90)||
                (opcode>=95&&opcode<=106);
            if(opcode==31||opcode==48||opcode==76)++nesting;
            if(opcode==21||opcode==22||opcode==23){if(!nesting)terminalSafe=false;else --nesting;}
            if(opcode==4||opcode==5||opcode==44||opcode==63||opcode==120)terminalSafe=false;
            if(opcode==62){
                if(terminalSafe&&!nesting&&token==0x0100003eu&&i+count==length&&previousCount==5&&previous[0]==0x05000036u){
                    const bool outputAlpha=(previous[1]&~0xf0u)==0x00102002u&&(previous[1]&0x80u)&&previous[2]==0;
                    // MOV o0.w, vN.w (or the identity vector MOV).
                    if(outputAlpha&&(previous[3]==0x0010103au||previous[3]==0x00101e46u)&&previous[4]<32)
                        alphaInput=previous[4];
                    // MOV rN.w, vM.w; MOV o0.xyzw, rN.xyzw.
                    if(outputAlpha&&previous[3]==0x00100e46u&&beforePreviousCount==5&&
                       beforePrevious[0]==0x05000036u&&beforePrevious[1]==0x00100082u&&
                       beforePrevious[2]==previous[4]&&beforePrevious[3]==0x0010103au&&beforePrevious[4]<32)
                        alphaInput=beforePrevious[4];
                }
                terminalSafe=false;
                alphaOne=straightLine&&previousAlphaOne&&token==0x0100003eu&&i+count==length;
                straightLine=false; // any later code, second RET or early return is unsupported
            }else if(!arithmetic)straightLine=false;
            bool literalAlpha=false;
            if((token==0x05000036u||token==0x08000036u)&&count>=5){
                const uint32_t destination=word(start+(i+1)*4);
                // Four-component masked output, one immediate register index, o0.w included.
                if((destination&~0xf0u)==0x00102002u&&(destination&0x80u)&&word(start+(i+2)*4)==0){
                    const uint32_t operand=word(start+(i+3)*4);
                    if(count==5&&operand==0x00004001u)literalAlpha=word(start+(i+4)*4)==0x3f800000u;
                    if(count==8&&operand==0x00004002u)literalAlpha=word(start+(i+7)*4)==0x3f800000u;
                }
            }
            previousAlphaOne=literalAlpha;
            beforePreviousCount=previousCount;std::memcpy(beforePrevious,previous,sizeof(previous));
            previousCount=count;
            if(count==5)for(size_t j=0;j<5;++j)previous[j]=word(start+(i+j)*4);
            i+=count;
        }
    }
    if(found&&constantAlphaOne)*constantAlphaOne=alphaOne;
    if(found&&vertexAlphaInput)*vertexAlphaInput=alphaInput;
    return found?true:reject(2);
}
}
