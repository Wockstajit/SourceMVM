#pragma once
// A CS2 pixel shader with its colour output scaled by two numbers we bind: the 3D skybox's unlit
// cards (Mirage's haze over the far city) at night. They are blended by their own alpha, so the
// blend state cannot dim them (EnvironmentNativeMaterialDraw.cpp, SkyboxUnlitDim); this variant
// writes the native colour times cb13[0].x and the native alpha times cb13[0].y at every return.
// Everything else (textures, discard, the blend) stays the shader's own. Unknown layouts fail
// closed: the draw then keeps the native shader.
#include "../Relight/RelightIrradianceMath.h"
#include <cstdint>
#include <cstring>
#include <vector>
namespace Filmmaker::Environment {
inline constexpr unsigned ScaledColorSlot = 13;
inline bool NativeScaledColorProgram(const void* data,size_t size,std::vector<uint8_t>& output) {
    using namespace Relight::IrradianceDxbc;
    output.clear();
    if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t offset){uint32_t v;std::memcpy(&v,p+offset,4);return v;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=size)return false;
    const unsigned chunks=word(28);if(chunks>(size-32)/4)return false;
    size_t program=0,programSize=0;
    for(unsigned i=0;i<chunks;++i){
        const size_t start=word(32+i*4);if(start<32+chunks*4||start>size-8)return false;
        const size_t bytes=word(start+4);if(bytes>size-start-8)return false;
        if(!std::memcmp(p+start,"SHEX",4)||!std::memcmp(p+start,"SHDR",4)){if(program)return false;program=start;programSize=bytes;}
    }
    if(!program||programSize<8||programSize%4||word(program+12)!=programSize/4)return false;
    Words code(programSize/4);std::memcpy(code.data(),p+program+8,programSize);
    if((code[0]>>16)!=0)return false; // a pixel shader only
    struct Instruction{size_t start,length;};std::vector<Instruction> instructions;
    unsigned temps=0;bool hasTemps=false,color=false;size_t lastConstant=0,declarationsEnd=2;
    auto declaration=[](unsigned op){return op==53||(op>=88&&op<=106)||(op>=143&&op<=162);};
    for(size_t i=2;i<code.size();){
        const auto token=code[i],op=token&2047;size_t length=(token>>24)&127;
        if(op==53){if(i+1>=code.size())return false;length=code[i+1];}
        if(!length||length>code.size()-i)return false;
        instructions.push_back({i,length});
        // Calls, conditional returns and interfaces would need control-flow analysis.
        if(op==4||op==5||op==63||op==120)return false;
        if(op==104){if(length!=2||hasTemps)return false;temps=code[i+1];hasTemps=true;}
        if(op==89){if(length!=4||code[i+2]==ScaledColorSlot)return false;lastConstant=i;}
        if(op==101&&length==3&&code[i+1]==0x001020f2u&&code[i+2]==0)color=true;
        if(declaration(op))declarationsEnd=i+length;
        i+=length;
    }
    if(!color||!lastConstant||temps>=4096)return false;
    const unsigned scratch=temps;
    Words patched{code[0],0};bool returned=false,wrote=false;
    for(const auto& ins:instructions){
        const auto op=code[ins.start]&2047;
        if(op==104){patched.insert(patched.end(),{0x02000068u,temps+1});continue;}
        Words w(code.begin()+ins.start,code.begin()+ins.start+ins.length);
        if(!declaration(op)){
            // Every write to o0 goes to the scratch register instead; outputs are never read.
            size_t cursor=ins.start+1;uint32_t ext=code[ins.start];
            while(ext&0x80000000u){if(cursor>=ins.start+ins.length)return false;ext=code[cursor++];}
            while(cursor<ins.start+ins.length){
                Operand o;if(!ReadOperand(code,cursor,ins.start+ins.length,o))return false;
                if(o.type!=2||o.index!=0)continue;
                const uint32_t token=code[o.start];
                if(((token>>20)&3)!=1||((token>>22)&7)!=0)return false; // o0 by an immediate index only
                size_t index=o.start+1;uint32_t operandExt=token;
                while(operandExt&0x80000000u){if(index>=o.end)return false;operandExt=code[index++];}
                if(index>=o.end)return false;
                w[o.start-ins.start]=token&~(255u<<12);w[index-ins.start]=scratch;wrote=true;
            }
            if(op==62){
                Emit(patched,56,{{0x00102072u,0},Temp(scratch,0x24),Cb(ScaledColorSlot,0,0x00)});
                Emit(patched,56,{{0x00102082u,0},Temp(scratch,0xff),Cb(ScaledColorSlot,0,0x55)});
                returned=true;
            }
        }
        patched.insert(patched.end(),w.begin(),w.end());
        if(ins.start==lastConstant)patched.insert(patched.end(),{0x04000059u,0x00208e46u,ScaledColorSlot,1});
        if(!hasTemps&&ins.start+ins.length==declarationsEnd)patched.insert(patched.end(),{0x02000068u,1});
    }
    if(!returned||!wrote)return false;
    patched[1]=uint32_t(patched.size());
    output.assign(p,p+32+chunks*4);
    auto put=[&](size_t n,uint32_t u){std::memcpy(output.data()+n,&u,4);};
    for(unsigned i=0;i<chunks;++i){
        const size_t start=word(32+i*4),bytes=word(start+4),destination=output.size();
        put(32+i*4,uint32_t(destination));
        if(start==program){
            output.insert(output.end(),p+start,p+start+8);output.resize(output.size()+patched.size()*4);
            std::memcpy(output.data()+destination+8,patched.data(),patched.size()*4);put(destination+4,uint32_t(patched.size()*4));
        }else output.insert(output.end(),p+start,p+start+8+bytes);
    }
    put(24,uint32_t(output.size()));
    DxbcHash::ComputeHashRetail(output.data()+20,uint32_t(output.size()-20),output.data()+4);
    return true;
}
} // namespace Filmmaker::Environment
