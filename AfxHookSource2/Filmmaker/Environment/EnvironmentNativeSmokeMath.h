#pragma once
#include "../Relight/RelightIrradianceMath.h"

namespace Filmmaker::Environment {
// Link a lighting query into the two native smoke ray-march branches. Recognition
// follows operands and declarations, not shader hashes. Unexpected layouts retain
// the native shader. Density, alpha, ray steps and MRT outputs remain untouched.
inline bool NativeSmokeProgram(const void* native,size_t nativeSize,const void* kernel,size_t kernelSize,
                              std::vector<uint8_t>& output,unsigned* queryCount=nullptr){
    using namespace Relight::IrradianceDxbc;
    output.clear();if(queryCount)*queryCount=0;
    struct Program{const uint8_t* p=nullptr;size_t bytes=0,chunk=0;unsigned chunks=0;Words code;};
    auto read=[](const void* data,size_t bytes,Program& result){
        if(!data||bytes<36||bytes>16*1024*1024)return false;
        result.p=static_cast<const uint8_t*>(data);result.bytes=bytes;
        auto word=[&](size_t at){uint32_t u;std::memcpy(&u,result.p+at,4);return u;};
        if(std::memcmp(result.p,"DXBC",4)||word(24)!=bytes)return false;
        result.chunks=word(28);if(result.chunks>(bytes-32)/4)return false;
        for(unsigned i=0;i<result.chunks;++i){size_t at=word(32+i*4);
            if(at<32+result.chunks*4||at>bytes-8)return false;size_t length=word(at+4);if(length>bytes-at-8)return false;
            if(!std::memcmp(result.p+at,"SHEX",4)||!std::memcmp(result.p+at,"SHDR",4)){
                if(result.chunk||length<8||length%4)return false;result.chunk=at;result.code.resize(length/4);std::memcpy(result.code.data(),result.p+at+8,length);
            }
        }
        return result.chunk&&result.code[0]==0x50&&result.code[1]==result.code.size();
    };
    Program source,helper;if(!read(native,nativeSize,source)||!read(kernel,kernelSize,helper))return false;
    struct Instruction{size_t at,length,operands;unsigned op;std::vector<Operand> args;};
    auto instructions=[](const Words& code,std::vector<Instruction>& out){
        for(size_t at=2;at<code.size();){auto token=code[at];unsigned op=token&2047;size_t length=(token>>24)&127;
            if(op==53){if(at+1>=code.size())return false;length=code[at+1];}
            if(!length||length>code.size()-at)return false;
            Instruction ins{at,length,at+1,op,{}};
            uint32_t ext=token;while(ext&0x80000000u){if(ins.operands>=at+length)return false;ext=code[ins.operands++];}
            if((op<88||op>106)&&op!=53&&op!=162){size_t cursor=ins.operands;
                while(cursor<at+length){Operand arg;if(!ReadOperand(code,cursor,at+length,arg))return false;ins.args.push_back(arg);}}
            out.push_back(std::move(ins));at+=length;
        }return true;
    };
    std::vector<Instruction> original,lighting;if(!instructions(source.code,original)||!instructions(helper.code,lighting))return false;
    unsigned temps=0,helperTemps=0;bool smokeTable=false,volume=false;
    size_t lastCb=0,lastSampler=0,lastResource=0;
    for(const auto& ins:original){const auto at=ins.at;
        if(ins.op==104)temps=source.code[at+1];
        if(ins.op==89){lastCb=at;if(ins.length==4&&source.code[at+2]==5&&source.code[at+3]==156)smokeTable=true;
            if(source.code[at+2]==12||source.code[at+2]==13)return false;}
        if(ins.op==90){lastSampler=at;if(source.code[at+2]>=14)return false;}
        if(ins.op==88||ins.op==162){lastResource=at;if(source.code[at+2]>=120)return false;
            if(ins.op==88&&source.code[at+2]==15&&((source.code[at]>>11)&31)==5)volume=true;}
    }
    Words cb,samplers,resources;
    for(const auto& ins:lighting){auto& dest=ins.op==89?cb:ins.op==90?samplers:resources;
        if(ins.op==104)helperTemps=helper.code[ins.at+1];
        if(ins.op==89||ins.op==90||ins.op==88||ins.op==162)dest.insert(dest.end(),helper.code.begin()+ins.at,helper.code.begin()+ins.at+ins.length);
        if(ins.op==105)return false; // no indexable helper temporaries
    }
    if(!temps||!helperTemps||temps+helperTemps+3>4096||!smokeTable||!volume)return false;
    Words patched{source.code[0],0};unsigned queries=0;unsigned worldRegister=~0u;
    auto cbOperand=[&](const Operand& arg,unsigned slot,unsigned row){
        const size_t rowWord=arg.start+((source.code[arg.start]&0x80000000u)?3:2);
        return arg.type==8&&rowWord<arg.end&&arg.index==slot&&source.code[rowWord]==row;
    };
    // Capture the native world normal before the native sun-shadow block reuses it.
    const unsigned normalTemp=temps+helperTemps,resultTemp=normalTemp+1,detailTemp=normalTemp+2;
    unsigned normalCaptures=0,detailCaptures=0;
    for(const auto& ins:original){
        if(ins.op==104){patched.insert(patched.end(),{0x02000068u,temps+helperTemps+3});continue;}
        // Native smoke's point position is used against cb5[index+32] (volume centre).
        if(ins.op==0&&ins.args.size()==3&&ins.args[1].type==0&&cbOperand(ins.args[2],5,32)&&
           ((source.code[ins.args[0].start]>>4)&15)==7)worldRegister=ins.args[1].index;
        bool query=false;
        if(ins.op==50&&ins.args.size()==4&&ins.args[0].type==0&&ins.args[1].type==0&&
           cbOperand(ins.args[2],0,4)&&ins.args[3].type==0&&
           (ins.args[3].index==ins.args[0].index||ins.args[1].index==ins.args[0].index)&&
           ((source.code[ins.args[2].start]>>4)&255)==0){query=true;}
        if(query){
            if(worldRegister==~0u||normalCaptures!=queries+1||detailCaptures!=queries+1)return false;
            // Retain native density/self-occlusion as a scalar, dividing out the
            // old voxel illumination. Its baked hue and brightness are discarded.
            Emit(patched,16,{Dest(detailTemp,2),Temp(ins.args[1].index),Literal(.2126f,.7152f,.0722f)});
            Emit(patched,52,{Dest(detailTemp,1),Temp(detailTemp,0),Literal(.001f,.001f,.001f)});
            Emit(patched,14,{Dest(detailTemp,2),Temp(detailTemp,85),Temp(detailTemp,0)});
            Emit(patched,52,{Dest(detailTemp,2),Temp(detailTemp,85),Literal(.12f,.12f,.12f)});
            Emit(patched,51,{Dest(detailTemp,2),Temp(detailTemp,85),Literal(1.2f,1.2f,1.2f)});
            for(const auto& function:lighting){
                if((function.op>=88&&function.op<=106)||function.op==162||function.op==62||function.op==53)continue;
                Words body(helper.code.begin()+function.at,helper.code.begin()+function.at+function.length);
                for(const auto& arg:function.args){const size_t at=arg.start-function.at;
                    if(arg.type==0){if(arg.end-arg.start<2||arg.end-arg.start>3||arg.index>=helperTemps)return false;body[arg.end-function.at-1]+=temps;}
                    else if(arg.type==1){if(arg.end-arg.start<2||arg.end-arg.start>3||arg.index>1)return false;
                        body[at]=(body[at]&~0xff000u);body[arg.end-function.at-1]=arg.index?normalTemp:worldRegister;}
                    else if(arg.type==2){if(arg.end-arg.start!=2||arg.index!=0)return false;
                        body[at]=(body[at]&~0xff000u);body[at+1]=resultTemp;}
                }
                patched.insert(patched.end(),body.begin(),body.end());
            }
            Emit(patched,56,{Dest(ins.args[0].index),Temp(resultTemp),Temp(detailTemp,85)});++queries;
        }else patched.insert(patched.end(),source.code.begin()+ins.at,source.code.begin()+ins.at+ins.length);
        if(ins.op==16&&ins.args.size()==3&&cbOperand(ins.args[2],3,1945)&&ins.args[1].type==0){
            Emit(patched,54,{Dest(normalTemp),Temp(ins.args[1].index)});++normalCaptures;
        }
        if(ins.op==72&&ins.args.size()==5&&ins.args[0].type==0&&ins.args[2].type==7&&ins.args[2].index==15){
            Emit(patched,16,{Dest(detailTemp,1),Temp(ins.args[0].index),Literal(.2126f,.7152f,.0722f)});++detailCaptures;
        }
        if(ins.at==lastCb)patched.insert(patched.end(),cb.begin(),cb.end());
        if(ins.at==lastSampler)patched.insert(patched.end(),samplers.begin(),samplers.end());
        if(ins.at==lastResource)patched.insert(patched.end(),resources.begin(),resources.end());
    }
    if(queries!=2)return false;patched[1]=unsigned(patched.size());
    output.assign(source.p,source.p+32+source.chunks*4);
    auto put=[&](size_t at,uint32_t value){std::memcpy(output.data()+at,&value,4);};
    for(unsigned i=0;i<source.chunks;++i){uint32_t at,length;std::memcpy(&at,source.p+32+i*4,4);std::memcpy(&length,source.p+at+4,4);
        const size_t dest=output.size();put(32+i*4,unsigned(dest));
        output.insert(output.end(),source.p+at,source.p+at+8);
        if(at==source.chunk){output.resize(output.size()+patched.size()*4);std::memcpy(output.data()+dest+8,patched.data(),patched.size()*4);put(dest+4,unsigned(patched.size()*4));}
        else output.insert(output.end(),source.p+at+8,source.p+at+8+length);
    }
    put(24,unsigned(output.size()));DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    if(queryCount)*queryCount=queries;return true;
}
}
