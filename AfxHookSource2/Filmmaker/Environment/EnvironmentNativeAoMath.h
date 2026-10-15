#pragma once
#include <algorithm>
#include "../Relight/RelightIrradianceMath.h"

namespace Filmmaker::Environment {
inline constexpr unsigned NativeAoConstantSlot=12;
// Move verified reads of the AO addressing triple from native b4[16] to a
// shared b12[0]. All other view constants, including row 16.w, stay native.
// Ambiguous addressing, mixed xyz/w reads, or an occupied b12 keeps the cloned-view path.
inline bool NativeSharedAoProgram(const void* data,size_t size,std::vector<uint8_t>& output) {
    using namespace Relight::IrradianceDxbc;
    output.clear();if(!data||size<32||size>16*1024*1024)return false;
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
    for(size_t at=2;at<code.size();){
        const unsigned op=code[at]&2047;size_t count=(code[at]>>24)&127;
        if(op==53){if(at+1>=code.size())return false;count=code[at+1];}
        if(!count||count>code.size()-at)return false;
        const size_t end=at+count;
        if(op==89){
            if(count!=4||code[at+1]!=0x00208e46u)return false;
            if(code[at+2]==NativeAoConstantSlot)return false;
            lastConstant=end;at=end;continue;
        }
        if(op==53){at=end;continue;} // immediate-constant payload is not instructions
        std::vector<size_t> candidates;
        for(size_t j=at+1;j+2<end;++j){const auto token=code[j];
            if(((token>>12)&255)!=8||((token>>20)&3)!=2||((token>>22)&7)!=0)continue;
            size_t index=j+1;uint32_t extension=token;
            while(extension>>31){if(index>=end)return false;extension=code[index++];}
            if(index+1>=end||code[index]!=4)continue;
            // Relative b4 indexing might reach row 16: do not split that program.
            if(((token>>25)&7)!=0)return false;
            if(code[index+1]==16)candidates.push_back(j);
        }
        if(!candidates.empty()){
            // These ordinary arithmetic/control/sample instructions have operands
            // only. Declarations and unfamiliar opcodes conservatively fall back.
            if(op>84)return false;
            size_t cursor=at;uint32_t extension=code[cursor];
            while(extension>>31){if(++cursor>=end)return false;extension=code[cursor];}++cursor;
            size_t found=0;
            while(cursor<end){Operand operand;if(!ReadOperand(code,cursor,end,operand))return false;
                if(std::find(candidates.begin(),candidates.end(),operand.start)==candidates.end())continue;
                const auto token=code[operand.start];const unsigned mode=(token>>2)&3;
                if((token&3)!=2)return false;
                // Scalar w reads remain on the native buffer; only xyz belong
                // to the AO repair. Mixed xyz/w vectors keep the clone fallback.
                if((mode==2&&((token>>4)&3)==3)||(mode==1&&((token>>4)&255)==255)){++found;continue;}
                if(mode==1){for(unsigned component=0;component<4;++component)
                    if(((token>>(4+2*component))&3)==3)return false;}
                else if(mode!=2)return false;
                size_t index=operand.start+1;uint32_t ext=token;
                while(ext>>31)ext=code[index++];
                if(operand.end!=index+2)return false;
                replacements.push_back(index);++found;
            }
            if(found!=candidates.size())return false;
        }
        at=end;
    }
    if(!lastConstant||replacements.empty())return false;
    for(auto index:replacements){code[index]=NativeAoConstantSlot;code[index+1]=0;}
    code.insert(code.begin()+lastConstant,{0x04000059u,0x00208e46u,NativeAoConstantSlot,1});
    code[1]=unsigned(code.size());
    output.assign(p,p+32+chunks*4);
    auto put=[&](size_t at,uint32_t value){std::memcpy(output.data()+at,&value,4);};
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4),destination=output.size();
        put(32+i*4,unsigned(destination));
        if(at==program){output.insert(output.end(),p+at,p+at+8);output.resize(output.size()+code.size()*4);
            std::memcpy(output.data()+destination+8,code.data(),code.size()*4);put(destination+4,unsigned(code.size()*4));}
        else output.insert(output.end(),p+at,p+at+8+word(at+4));
    }
    put(24,unsigned(output.size()));DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    return true;
}
}
