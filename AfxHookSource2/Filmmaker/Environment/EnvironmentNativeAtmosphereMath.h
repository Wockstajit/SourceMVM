#pragma once
#include "EnvironmentDxbcHash.h"
#include "EnvironmentNativeShadowMath.h"
#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace Filmmaker::Environment {
// Shipped forward programs apply map gradient/cubemap fog after lighting. The
// cubemap can reintroduce precomputed scene color; the gradient also survives a
// zero-light diagnostic. Our atmosphere pass owns both when relighting is active.
// Recognize the material fog guard AND its opacity operations. Other material
// branches, including alpha/discard and emission, remain byte-for-byte intact.
// The cascade loop ends with the engine's own screen-space shadow hook:
//
//   if_nz cb4[1].y
//     sample_l  rS, <screen uv>, tN, sM         // N and the sampler vary per permutation
//     min       rV, rS, rV                      // (or min rV, rV, rS)
//   endif
//
// `if_nz` keeps the engine's flag; here the flag becomes cb3[ScreenShadowFlagRow].y (a row we
// patch for every owned draw). A valid cached sample replaces the native value; alpha below
// one means the cache crossed a depth/MSAA edge, so retain the native cascade result.
// tN supplies visibility in RGB and cache validity in alpha. A draw that supplies no
// texture sets the flag to zero and keeps the native cascade filter. Returns the slot N, or 0
// when the block does not have exactly this shape (the block is then left alone).
inline constexpr uint32_t ScreenShadowFlagRow=1953;
// lampCube: every lamp kernel in the program takes cube shadow records (SmoothNativeLampKernels).
inline bool NativeAtmosphereProgram(const void* data,size_t size,std::vector<uint8_t>& output,bool ownedAo=false,bool* ownedCascades=nullptr,uint32_t* screenShadowSlot=nullptr,bool* lampCube=nullptr) {
    if(lampCube)*lampCube=false;
    if(ownedCascades)*ownedCascades=false;
    if(screenShadowSlot)*screenShadowSlot=0;
    output.clear();if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t at){uint32_t value;std::memcpy(&value,p+at,4);return value;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=size)return false;
    const unsigned chunks=word(28);if(chunks>(size-32)/4)return false;
    size_t program=0,programBytes=0;
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4);
        if(at<32+chunks*4||at>size-8)return false;
        const size_t bytes=word(at+4);if(bytes>size-at-8)return false;
        if(!std::memcmp(p+at,"SHEX",4)||!std::memcmp(p+at,"SHDR",4)){
            if(program)return false;program=at;programBytes=bytes;
        }
    }
    if(!program||programBytes<8||programBytes%4)return false;
    std::vector<uint32_t> code(programBytes/4);std::memcpy(code.data(),p+program+8,programBytes);
    if(code[0]!=0x50||code[1]!=code.size())return false;
    struct Instruction {size_t at,count;};std::vector<Instruction> instructions;
    for(size_t i=2;i<code.size();){const auto token=code[i],op=token&2047;
        size_t length=(token>>24)&127;
        if(op==53){if(i+1>=code.size())return false;length=code[i+1];if(length<2)return false;}
        if(!length||length>code.size()-i)return false;
        instructions.push_back({i,length});i+=length;
    }
    size_t tempsAt=0;unsigned scratch=0;
    for(const auto ins:instructions)if((code[ins.at]&2047)==104&&ins.count==2){tempsAt=ins.at+1;scratch=code[tempsAt];}
    struct Replacement {size_t at,count;std::vector<uint32_t> words;};
    std::vector<Replacement> replacements;
    size_t guard=0;
    for(size_t j=0;j<instructions.size();++j){const auto ins=instructions[j];
        // The material flag moves with each family/permutation (observed at
        // b0[0].z, b0[1].y, b0[2].x, b0[3].y and b0[28].y). Identify the fog
        // operations themselves instead of assuming one material buffer layout.
        if(ins.count!=4||code[ins.at]!=0x0404001fu||code[ins.at+2]!=0)continue;
        if((code[ins.at+1]&~0x30u)!=0x0020800au)continue;
        unsigned depth=1;bool opacity=false,closed=false,lighting=false;
        for(size_t k=j+1;k<instructions.size();++k){const auto nested=instructions[k];
            const auto op=code[nested.at]&2047;
            if(op==31)++depth;
            if(op==21){if(!--depth){closed=true;break;}}
            // An enclosing material/lighting branch may contain a fog child.
            // Never disable that parent and accidentally discard illumination.
            if(op==167)lighting=true; // ld_structured: native light/probe records
            if(op==56&&nested.count==8)for(unsigned operand:{3u,5u})
                if(code[nested.at+operand]==0x0020803au&&code[nested.at+operand+1]==4&&
                   (code[nested.at+operand+2]==25||code[nested.at+operand+2]==33))opacity=true;
        }
        if(!closed||!opacity||lighting)continue;
        if(guard)return false;guard=ins.at;
    }
    // Some permutations evaluate the surface twice (two shading blocks), each with its own
    // verified AO and screen-shadow guard. Every verified copy is patched; none is guessed.
    std::vector<size_t> aoGuards;
    if(ownedAo)for(size_t j=0;j<instructions.size();++j){const auto ins=instructions[j];
        // Verified forward-material SSAO guard: cb4[1].x. Require an actual t7
        // sample in that branch; other view flags are unrelated (including fog).
        if(ins.count!=4||code[ins.at]!=0x0404001fu||code[ins.at+1]!=0x0020800au||
           code[ins.at+2]!=4||code[ins.at+3]!=1)continue;
        unsigned depth=1;bool sample=false,closed=false;
        for(size_t k=j+1;k<instructions.size();++k){const auto nested=instructions[k];
            const auto op=code[nested.at]&2047;
            if(op==31)++depth;
            if(op==21){if(!--depth){closed=true;break;}}
            if(op>=69&&op<=74)for(size_t at=nested.at+1;at+1<nested.at+nested.count;++at)
                if(((code[at]>>12)&255)==7&&((code[at]>>20)&3)==1&&
                   ((code[at]>>22)&7)==0&&code[at+1]==7)sample=true;
        }
        if(!closed||!sample)continue;
        aoGuards.push_back(ins.at);
    }
    size_t cascadeDeclaration=0;std::vector<size_t> sunGuards,screenGuards;unsigned matrixRows=0;bool rect=false,count=false,selectors=false;
    uint32_t screenSlot=0;bool screenConflict=false;
    if(ownedCascades){
        for(const auto ins:instructions){
            // The selector rows are icb[0..3]. Some permutations append further rows for
            // unrelated lookups; only the four the cascade loop indexes must be identity.
            if((code[ins.at]&2047)==53&&ins.count>=18){
                bool identity=true;for(unsigned i=0;i<16;++i)
                    identity&=code[ins.at+2+i]==((i%5)==0?0x3f800000u:0u);
                selectors|=identity;
            }
            if(code[ins.at]==0x04000859u&&ins.count==4&&code[ins.at+1]==0x00208e46u&&
               code[ins.at+2]==3&&code[ins.at+3]==1990)cascadeDeclaration=ins.at+3;
            if(code[ins.at]==0x0404001fu&&ins.count==4&&code[ins.at+1]==0x0020800au&&
               code[ins.at+2]==3&&code[ins.at+3]==1953)count=true;
            for(size_t at=ins.at+1;at+3<ins.at+ins.count;++at)
                if((code[at]&0xfffff000u)==0x06208000u&&code[at+1]==3){
                    if(code[at+2]>=1956&&code[at+2]<=1959)matrixRows|=1u<<(code[at+2]-1956);
                    if(code[at+2]==1988)rect=true;
                }
        }
        if(cascadeDeclaration&&matrixRows==15&&rect&&count&&selectors){
            // The shipped shader loops over matrices and has four ICB selector
            // entries, but its two atlas rects ended at row 1989. Extend only the
            // verified layout so four owned rects remain inside the declaration.
            code[cascadeDeclaration]=1992;*ownedCascades=true;
            for(size_t j=0;j<instructions.size();++j){const auto ins=instructions[j];
                if(ins.count!=4||code[ins.at]!=0x0404001fu||code[ins.at+1]!=0x0020801au||
                   code[ins.at+2]!=4||code[ins.at+3]!=1)continue;
                unsigned depth=1;bool sample=false,minimum=false,closed=false;
                unsigned samples=0,minimums=0,others=0;size_t sampleAt=0,sampleSize=0,sampleResource=0,minAt=0;uint32_t slot=0;
                for(size_t k=j+1;k<instructions.size();++k){const auto nested=instructions[k];const auto op=code[nested.at]&2047;
                    if(op==31)++depth;if(op==21&&! --depth){closed=true;break;}
                    if(op==51){minimum=true;++minimums;minAt=nested.at;}
                    else if(op!=56&&!(op>=69&&op<=74))++others; // only the screen-uv mul is expected besides the sample
                    if(op>=69&&op<=74)for(size_t at=nested.at+1;at+1<nested.at+nested.count;++at)
                        if(((code[at]>>12)&255)==7&&((code[at]>>20)&3)==1&&((code[at]>>22)&7)==0){
                            if(code[at+1]==18||code[at+1]==19)sample=true;
                            if(op==72){slot=code[at+1];sampleAt=nested.at;sampleSize=nested.count;sampleResource=at;++samples;break;}
                        }
                }
                if(!closed||!minimum)continue;
                // Preferred: take the block over (see the comment on ScreenShadowFlagRow).
                bool owned=false;
                if(samples==1&&minimums==1&&!others&&slot){
                    auto afterExtensions=[&](size_t at){while(code[at]>>31)++at;return at+1;};
                    const size_t dst=afterExtensions(sampleAt);
                    const uint32_t mask=(code[dst]>>4)&15;
                    const bool plainTemp=((code[dst]>>12)&255)==0&&((code[dst]>>20)&3)==1&&!(code[dst]>>31)&&
                        mask&&!(mask&(mask-1));
                    const size_t m=minAt;
                    auto source=[&](size_t at){return ((code[at]>>12)&255)==0&&((code[at]>>20)&3)==1&&
                        !(code[at]>>31)&&((code[at]>>2)&3)==2;};
                    if(plainTemp&&((code[m]>>24)&127)==7&&!(code[m]>>31)&&source(m+3)&&source(m+5)){
                        unsigned component=0;while(!((mask>>component)&1))++component;
                        const uint32_t reg=code[dst+1];
                        const bool first=code[m+4]==reg&&((code[m+3]>>4)&3)==component;
                        const bool second=code[m+6]==reg&&((code[m+5]>>4)&3)==component;
                        if(first!=second&&tempsAt&&scratch<4095&&((code[sampleResource]>>2)&3)==1){
                            const size_t keep=first?m+3:m+5,fallback=first?m+5:m+3;
                            // Read all four cache channels into one spare register. Preserve
                            // the original scalar register, using cache red for visibility
                            // even if this native permutation originally read another channel.
                            std::vector<uint32_t> sampleCode(code.begin()+sampleAt,code.begin()+sampleAt+sampleSize);
                            sampleCode[dst-sampleAt]=0x001000f2u;sampleCode[dst-sampleAt+1]=scratch;
                            sampleCode[sampleResource-sampleAt]=(code[sampleResource]&~0xff0u)|0xe40u;
                            sampleCode.insert(sampleCode.end(),{0x05000036u,code[dst],reg,0x0010000au,scratch});
                            replacements.push_back({sampleAt,sampleSize,std::move(sampleCode)});
                            // Validity is linearly filtered with visibility. Reject even a
                            // partial footprint on an ambiguous silhouette texel (alpha >= .99999).
                            replacements.push_back({m,7,{0x0700001du,0x00100082u,scratch,0x0010003au,scratch,0x00004001u,0x3f7fff58u,
                                0x09000037u,code[m+1],code[m+2],0x0010003au,scratch,code[keep],code[keep+1],code[fallback],code[fallback+1]}});
                            owned=true;
                        }
                    }
                }
                if(owned){
                    if(screenSlot&&screenSlot!=slot)screenConflict=true;
                    screenSlot=slot;screenGuards.push_back(ins.at);
                }else if(sample)sunGuards.push_back(ins.at);
            }
        }
    }
    if(!guard&&aoGuards.empty()&&!(ownedCascades&&*ownedCascades))return false;
    // Two screen-shadow blocks reading different textures cannot share one owned flag.
    if(screenConflict)return false;
    for(auto at:screenGuards){code[at+2]=3;code[at+3]=ScreenShadowFlagRow;}
    if(screenShadowSlot&&ownedCascades&&*ownedCascades&&!screenGuards.empty())*screenShadowSlot=screenSlot;
    if(!replacements.empty())code[tempsAt]=scratch+1;
    // Process from the end so resizing an instruction cannot shift another patch.
    std::vector<std::pair<size_t,uint32_t>> guards;
    if(guard)guards.push_back({guard,0});
    for(auto at:aoGuards)guards.push_back({at,1});
    for(auto at:sunGuards)guards.push_back({at,0});
    for(const auto& [at,value]:guards)replacements.push_back({at,4,{0x0304001fu,0x00004001u,value}});
    std::sort(replacements.begin(),replacements.end(),[](const auto& a,const auto& b){return a.at>b.at;});
    for(const auto& patch:replacements){
        code.erase(code.begin()+patch.at,code.begin()+patch.at+patch.count);
        code.insert(code.begin()+patch.at,patch.words.begin(),patch.words.end());
    }
    code[1]=unsigned(code.size());
    OwnNativeLocalShadowGuards(code);
    SmoothNativeLampKernels(code,true,NativeLampAtlasPyramid,lampCube);
    output.assign(p,p+32+chunks*4);
    auto put=[&](size_t at,uint32_t value){std::memcpy(output.data()+at,&value,4);};
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4),bytes=word(at+4),destination=output.size();
        put(32+i*4,unsigned(destination));
        if(at==program){output.insert(output.end(),p+at,p+at+8);output.resize(output.size()+code.size()*4);
            std::memcpy(output.data()+destination+8,code.data(),code.size()*4);put(destination+4,unsigned(code.size()*4));}
        else output.insert(output.end(),p+at,p+at+8+bytes);
    }
    put(24,unsigned(output.size()));DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    return true;
}
} // namespace Filmmaker::Environment
