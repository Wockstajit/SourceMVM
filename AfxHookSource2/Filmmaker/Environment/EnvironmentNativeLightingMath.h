#pragma once
#include <array>
#include "EnvironmentConstantPatchMath.h"
#include "../Relight/RelightIrradianceMath.h"

namespace Filmmaker::Environment {
inline constexpr unsigned NativeLightingConstantSlot=11;
inline constexpr unsigned NativeLightingRows=1992;
struct NativeSharedLightingInfo {
    unsigned count=0;
    std::array<ConstantPatchBatch::Range,96> required{};
};
// The native ABI has one owned reflection/probe record and four owned sun
// cascades. Their dynamic indices are bounded by the list/count rows we supply.
// Rows outside these records continue reading the engine's unchanged b3.
inline unsigned NativeOwnedLightingMask(unsigned row) {
    if(row<4||(row>=7&&row<=9)||(row>=18&&row<=23)||
       (row>=25&&row<=33)||(row>=1177&&row<=1182)||row==1945||row==1947||
       (row>=1954&&row<=1971)||(row>=1988&&row<=1991))return 15;
    if(row==6)return 3;
    if(row==24)return 2;
    if(row==1946||row==1953)return 7;
    return 0;
}
inline ConstantPatchBatch::Range NativeOwnedDynamicRange(unsigned base) {
    if(base>=25&&base<=33)return {25*16,34*16};
    if(base>=1177&&base<=1182)return {1177*16,1183*16};
    if(base>=1956&&base<=1959)return {1956*16,1972*16};
    if(base==1988)return {1988*16,1992*16};
    return {};
}
inline bool NativeSharedLightingProgram(const void* data,size_t size,std::vector<uint8_t>& output,
                                        NativeSharedLightingInfo& info) {
    using namespace Relight::IrradianceDxbc;
    output.clear();info={};if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t at){uint32_t v;std::memcpy(&v,p+at,4);return v;};
    if(word(0)!=0x43425844u||word(24)!=size)return false;
    const unsigned chunks=word(28);if(chunks>(size-32)/4)return false;
    size_t program=0;Words code;
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4);
        if(at<32+chunks*4||at>size-8||word(at+4)>size-at-8)return false;
        if(word(at)==0x52444853u||word(at)==0x58454853u){
            if(program||word(at+4)<8||word(at+4)%4)return false;program=at;
            code.resize(word(at+4)/4);std::memcpy(code.data(),p+at+8,code.size()*4);
        }
    }
    if(!program||code[1]!=code.size()||(code[0]>>16)!=0)return false;
    size_t lastConstant=0;std::vector<size_t> replacements;
    std::array<bool,NativeLightingRows*4> needed{};
    for(size_t at=2;at<code.size();){
        const unsigned op=code[at]&2047;size_t count=(code[at]>>24)&127;
        if(op==53){if(at+1>=code.size())return false;count=code[at+1];}
        if(!count||count>code.size()-at)return false;
        const size_t end=at+count;
        if(op==89){
            if(count!=4||code[at+1]!=0x00208e46u)return false;
            if(code[at+2]==NativeLightingConstantSlot)return false;
            lastConstant=end;at=end;continue;
        }
        if(op==53){at=end;continue;}
        std::vector<size_t> candidates;
        for(size_t j=at+1;j+2<end;++j){const auto token=code[j];
            if(((token>>12)&255)!=8||((token>>20)&3)!=2||((token>>22)&7)!=0)continue;
            size_t index=j+1;uint32_t extension=token;
            while(extension>>31){if(index>=end)return false;extension=code[index++];}
            if(index+1<end&&code[index]==3)candidates.push_back(j);
        }
        if(!candidates.empty()){
            if(op>86)return false; // Ordinary arithmetic/control/sample operands, through xor.
            size_t cursor=at;uint32_t extension=code[cursor];
            while(extension>>31){if(++cursor>=end)return false;extension=code[cursor];}++cursor;
            size_t found=0;
            while(cursor<end){Operand operand;if(!ReadOperand(code,cursor,end,operand))return false;
                if(std::find(candidates.begin(),candidates.end(),operand.start)==candidates.end())continue;
                ++found;const auto token=code[operand.start];size_t index=operand.start+1;uint32_t ext=token;
                while(ext>>31)ext=code[index++];
                const unsigned rep=(token>>25)&7;
                if(rep!=0&&rep!=3)return false;
                const auto row=code[index+1];if(row>=NativeLightingRows)return false;
                if(rep==3){
                    const auto range=NativeOwnedDynamicRange(row);if(!range.end)return false;
                    std::fill(needed.begin()+range.begin/4,needed.begin()+range.end/4,true);
                    replacements.push_back(index);continue;
                }
                if(operand.end!=index+2||(token&3)!=2)return false;
                const unsigned mode=(token>>2)&3;unsigned used=0;
                if(mode==1){for(unsigned component=0;component<4;++component)used|=1u<<((token>>(4+2*component))&3);}
                else if(mode==2)used=1u<<((token>>4)&3);else return false;
                const unsigned owned=NativeOwnedLightingMask(row);
                if(!(used&owned))continue;
                if(used&~owned)return false; // A mixed native/owned vector keeps the clone path.
                for(unsigned c=0;c<4;++c)if(used&(1u<<c))needed[row*4+c]=true;
                replacements.push_back(index);
            }
            if(found!=candidates.size())return false;
        }
        at=end;
    }
    if(!lastConstant||replacements.empty())return false;
    for(size_t at=0;at<needed.size();){
        if(!needed[at]){++at;continue;}const auto begin=at;while(at<needed.size()&&needed[at])++at;
        if(info.count==info.required.size())return false;info.required[info.count++]={unsigned(begin*4),unsigned(at*4)};
    }
    for(auto index:replacements)code[index]=NativeLightingConstantSlot;
    code.insert(code.begin()+lastConstant,{0x04000859u,0x00208e46u,NativeLightingConstantSlot,NativeLightingRows});code[1]=unsigned(code.size());
    output.assign(p,p+32+chunks*4);
    auto put=[&](size_t at,uint32_t value){std::memcpy(output.data()+at,&value,4);};
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4),destination=output.size();put(32+i*4,unsigned(destination));
        if(at==program){output.insert(output.end(),p+at,p+at+8);output.resize(output.size()+code.size()*4);
            std::memcpy(output.data()+destination+8,code.data(),code.size()*4);put(destination+4,unsigned(code.size()*4));}
        else output.insert(output.end(),p+at,p+at+8+word(at+4));
    }
    put(24,unsigned(output.size()));DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    return true;
}
inline bool NativeSharedLightingCovered(const NativeSharedLightingInfo& info,
                                        const std::vector<ConstantPatchBatch::Range>& ranges) {
    if(!info.count||info.count>info.required.size())return false;
    for(unsigned i=0;i<info.count;++i){auto remaining=info.required[i];
        while(remaining.begin<remaining.end){unsigned end=remaining.begin;
            for(const auto& range:ranges)if(range.begin<=remaining.begin&&range.end>end)end=range.end;
            if(end==remaining.begin)return false;remaining.begin=end;
        }
    }
    return true;
}
}
