#pragma once
#include "../Relight/RelightIrradianceMath.h"
#include <algorithm>
#include <cmath>

namespace Filmmaker::SmokeAppearance {
inline constexpr unsigned ConstantSlot = 11;
struct Parameters {
    float density=.85f, wispy=0.f, speed=1.5f, scaleLow=3.f, scaleHigh=7.f;
    float strengthLow=.5f, strengthHigh=.35f, noise=.8f, boil=-.3f, rayStep=6.f;
    float spread=1.f, resolution=32.f;
};
struct Parameter { const char* name; float Parameters::* member; float minimum, maximum; unsigned row, lane; };
struct ProgramInfo { bool smoke=false, composite=false; unsigned controls=0, reads=0, geometry=0, samples=0, table=~0u; };
inline constexpr Parameter Controls[] = {
    {"density",&Parameters::density,0,20,0,3}, {"wispy",&Parameters::wispy,0,1,8,0},
    {"speed",&Parameters::speed,0,10,1,0}, {"scale_low",&Parameters::scaleLow,.05f,20,2,3},
    {"scale_high",&Parameters::scaleHigh,.05f,20,3,0},
    {"strength_low",&Parameters::strengthLow,0,1,1,1}, {"strength_high",&Parameters::strengthHigh,0,1,1,2},
    {"noise",&Parameters::noise,0,20,0,2}, {"boil",&Parameters::boil,-1,1,5,1},
    {"ray_step",&Parameters::rayStep,.5f,20,5,0},
    {"spread",&Parameters::spread,.25f,2.f,9,0},
    {"resolution",&Parameters::resolution,4,64,9,2}
};
inline bool Set(Parameters& p,const Parameter& c,float value) {
    if(!std::isfinite(value))return false;
    p.*(c.member)=std::clamp(value,c.minimum,c.maximum);
    if(c.member==&Parameters::resolution)p.resolution=std::round(p.resolution);
    return true;
}
inline float VoxelSize(const Parameters& p){return 640.f*p.spread/p.resolution;}
inline unsigned VoxelCount(const Parameters& p){const auto n=unsigned(p.resolution);return n*n*n;}
inline bool SetVoxelSize(Parameters& p,float size){
    if(!std::isfinite(size)||size<=0)return false;
    return Set(p,Controls[11],640.f*p.spread/size);
}
inline std::array<float,44> Constants(const Parameters& p) {
    std::array<float,44> result{};
    for(const auto& c:Controls)result[c.row*4+c.lane]=p.*(c.member);
    // Thin smoke needs the native colour and extra-opacity integrators to agree.
    result[0]=(std::min)(p.density/.85f,1.f);result[3]=(std::max)(p.density,.85f);
    result[37]=1.f/p.spread;result[38]=p.resolution/32.f;result[39]=32.f/p.resolution;
    result[40]=p.resolution==32.f?0.f:1.f;result[41]=p.spread==1.f?0.f:1.f;
    return result;
}
// Native density is a colour-coverage threshold. Below its shipped value, also
// scale the per-sample alpha and extra-opacity integrators before compositing.
// The same operation reaches the moment and colour paths, keeping premultiplied
// colour/alpha consistent. Match the complete density smoothstep, not registers.
inline bool ThinDensity(Relight::IrradianceDxbc::Words& code) {
    using namespace Relight::IrradianceDxbc;
    struct Instruction {size_t at,length;unsigned op;std::vector<Operand> args;};
    std::vector<Instruction> instructions;
    for(size_t at=2;at<code.size();) {
        const unsigned op=code[at]&2047;size_t length=(code[at]>>24)&127;
        if(op==53)length=code[at+1];
        size_t cursor=at+1;uint32_t ext=code[at];
        while(ext&0x80000000u)ext=code[cursor++];
        Instruction ins{at,length,op,{}};
        if(op<88&&op!=53)while(cursor<at+length){Operand arg;if(!ReadOperand(code,cursor,at+length,arg))return false;ins.args.push_back(arg);}
        instructions.push_back(std::move(ins));at+=length;
    }
    auto cb=[&](const Operand& arg,unsigned slot,unsigned row,unsigned lane){
        if(arg.type!=8||arg.index!=slot)return false;
        const auto token=code[arg.start];const size_t index=arg.start+1+((token&0x80000000u)?1:0);
        if(arg.end!=index+2||code[index+1]!=row)return false;
        const unsigned mode=(token>>2)&3;
        return mode==2?((token>>4)&3)==lane:mode==1&&((token>>4)&255)==lane*0x55u;
    };
    std::vector<std::pair<size_t,Words>> insertions;unsigned opacity=0,coverage=0;
    for(size_t i=0;i<instructions.size();++i){const auto& ins=instructions[i];
        if(ins.op!=56||ins.args.size()!=3||ins.args[0].type!=0)continue;
        bool extra=cb(ins.args[2],0,6,3),density=cb(ins.args[2],ConstantSlot,0,3);
        const Instruction* target=nullptr;
        if(extra){target=&ins;++opacity;}
        if(density){
            if(i+6>=instructions.size())return false;
            constexpr unsigned ops[]={14,14,56,50,56,56};
            for(unsigned j=0;j<6;++j)if(instructions[i+j+1].op!=ops[j])return false;
            target=&instructions[i+6];++coverage;
        }
        if(!target)continue;
        if(target->args.size()!=3||target->args[0].type!=0)return false;
        const auto& dest=target->args[0];unsigned mask=(code[dest.start]>>4)&15,lane=0;
        if(!mask||(mask&(mask-1)))return false;
        while((1u<<lane)!=mask)++lane;
        Words scaling;Emit(scaling,56,{Dest(dest.index,mask),Temp(dest.index,lane*0x55u),Cb(ConstantSlot,0,0)});
        insertions.push_back({target->at+target->length,std::move(scaling)});
    }
    // Direct scalar consumers need no extra-opacity correction. Partial native
    // branches are unsupported: a colour-only patch would recreate black smoke.
    if((opacity||coverage)&&(opacity!=2||coverage!=2))return false;
    std::sort(insertions.begin(),insertions.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    for(const auto& item:insertions)code.insert(code.begin()+item.first,item.second.begin(),item.second.end());
    return true;
}
// Redirect only the ten scalar appearance reads to our own CB. Native material
// constants, colour, light queries, volume data, outputs and control flow survive.
// Match the shader ABI and operand use, never a hash or a guessed instruction offset.
// Resample the native atlas in local coordinates. The atlas allocation, packing,
// simulation and bullet/HE data stay native. Bounds and masks use the same scale.
inline unsigned Geometry(Relight::IrradianceDxbc::Words& code,unsigned table,bool expandScreenClip=false) {
    using namespace Relight::IrradianceDxbc;
    unsigned first=~0u;size_t temps=0;
    for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t length=(code[at]>>24)&127;
        if(op==53)length=code[at+1];
        if(op==104){temps=at+1;first=code[temps];}at+=length;
    }
    if(first==~0u||first>4090)return 0;
    auto raw=[&](const Operand& a){return Words(code.begin()+a.start,code.begin()+a.end);};
    auto cbRow=[&](const Operand& a,unsigned& row)->bool{
        if(a.type!=8||a.index!=table)return false;
        auto token=code[a.start];size_t ix=a.start+1+((token&0x80000000u)?1:0);
        const unsigned rep=(token>>25)&7;
        if(((token>>20)&3)!=2||((token>>22)&7)!=0||(rep!=2&&rep!=3)||ix+2>=a.end)return false;
        row=rep==2?0:code[ix+1];return true;
    };
    auto plainCb=[&](const Operand& a,unsigned row){auto v=raw(a);if(v[0]&0x80000000u)v.erase(v.begin()+1);
        if(((v[0]>>25)&7)==2){v[0]=(v[0]&~(7u<<25))|(3u<<25);v.insert(v.begin()+2,row);}else v[2]=row;
        v[0]=(v[0]&~0x80000fffu)|0xe46u;return v;};
    auto negate=[](Words v){v[0]|=0x80000000u;v.insert(v.begin()+1,0x41u);return v;};
    auto substituted=[&](const Operand& a,unsigned r){Words v{(code[a.start]&0x80000fffu)|0x00100000u};
        if(v[0]&0x80000000u)v.push_back(code[a.start+1]);v.push_back(r);return v;};
    auto literal=[&](const Operand& a,unsigned mask,float value){if(a.type!=4||(code[a.start]&3)!=2)return false;
        for(unsigned lane=0;lane<4;++lane)if(mask&(1u<<lane))if(code[a.start+1+lane]!=Float(value))return false;
        return true;};
    // Identify the native pair by its shared dynamic index. An arbitrary array
    // access with offset zero may be a computed center/metadata index instead.
    auto relative=[&](const Operand& a){const size_t ix=a.start+1+((code[a.start]&0x80000000u)?1:0);
        const size_t from=ix+1+((((code[a.start]>>25)&7)==3)?1:0);return Words(code.begin()+from,code.begin()+a.end);};
    std::vector<Words> lower,upper;
    for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t length=(code[at]>>24)&127;if(op==53)length=code[at+1];
        size_t cursor=at+1;uint32_t ext=code[at];while(ext&0x80000000u)ext=code[cursor++];
        if(op<88&&op!=53)while(cursor<at+length){Operand a;if(!ReadOperand(code,cursor,at+length,a))return 0;unsigned row;
            if(cbRow(a,row)){if(row==0)lower.push_back(relative(a));if(row==16)upper.push_back(relative(a));}}
        at+=length;
    }
    Words out{code[0],0};unsigned changed=0;
    for(size_t at=2;at<code.size();){const unsigned op=code[at]&2047;size_t length=(code[at]>>24)&127;
        if(op==53)length=code[at+1];size_t cursor=at+1;uint32_t ext=code[at];while(ext&0x80000000u)ext=code[cursor++];
        std::vector<Operand> args;
        if(op<88&&op!=53)while(cursor<at+length){Operand a;if(!ReadOperand(code,cursor,at+length,a))return 0;args.push_back(a);}
        Words before,after;std::vector<std::pair<Operand,Words>> replacements;
        unsigned k=0;
        for(size_t i=1;i<args.size();++i){const auto& a=args[i];unsigned row=~0u;
            if(!cbRow(a,row)||row>=32||(row!=0&&row!=16))continue;
            const auto index=relative(a);const auto& paired=row==0?upper:lower;
            if(std::find(paired.begin(),paired.end(),index)==paired.end())continue;
            // Preserve W (native volume IDs/metadata) and the original swizzle/modifier.
            const unsigned dst=first+k++,center=first+4,delta=first+5;
            auto original=plainCb(a,row);
            Emit(before,0,{Dest(center,15),plainCb(a,0),plainCb(a,16)});
            Emit(before,56,{Dest(center,15),Temp(center),Literal(.5f,.5f,.5f,.5f)});
            Emit(before,0,{Dest(delta),original,negate(Temp(center))});
            Emit(before,54,{Dest(dst,15),original});
            Emit(before,50,{Dest(dst),Temp(delta),Cb(ConstantSlot,9,0),Temp(center)});
            Emit(before,55,{Dest(dst),Cb(ConstantSlot,10,85),Temp(dst),original});
            replacements.push_back({a,substituted(a,dst)});++changed;
        }
        // The ray marcher rejects rays against the union before it visits individual
        // volumes. Grow that union conservatively; scaling only each box still clips.
        for(size_t i=1;i<args.size();++i){const auto& a=args[i];
            if(a.type!=8||a.index!=table)continue;
            const size_t ix=a.start+1+((code[a.start]&0x80000000u)?1:0);
            if(a.end!=ix+2||(code[ix+1]!=96&&code[ix+1]!=97))continue;
            const unsigned row=code[ix+1],dst=first+k++,center=first+4,delta=first+5;
            Emit(before,0,{Dest(center),Cb(table,96),Cb(table,97)});
            Emit(before,56,{Dest(center),Temp(center),Literal(.5f,.5f,.5f)});
            Emit(before,0,{Dest(delta),Cb(table,row),negate(Temp(center))});
            Emit(before,54,{Dest(dst,15),Cb(table,row)});
            // Shrinking each separate box must not shrink away the space between boxes.
            Emit(before,52,{Dest(delta,8),Cb(ConstantSlot,9,0),Literal(1,1,1,1)});
            Emit(before,50,{Dest(dst),Temp(delta),Temp(delta,255),Temp(center)});
            replacements.push_back({a,substituted(a,dst)});++changed;
        }
        if(op==0&&args.size()==3&&args[0].type==0){const auto& dest=args[0];const unsigned mask=(code[dest.start]>>4)&15;
            if(mask==7||mask==14)for(unsigned i=1;i<=2;++i){unsigned row=~0u;if(!cbRow(args[i],row)||row!=32)continue;
                const bool negative=(code[args[i].start]&0x80000000u)&&((code[args[i].start+1]>>6)&3)==1;
                if(negative){Emit(after,56,{raw(dest),Temp(dest.index),Cb(ConstantSlot,9,85)});++changed;}
                else if(args[3-i].type==0){const auto& offset=args[3-i];
                    Emit(before,56,{Dest(first+3,15),raw(offset),Cb(ConstantSlot,9,0)});
                    // Scaling only XYZ leaves this branch's packed scalar untouched.
                    Emit(before,55,{Dest(first+3,15),Literal(mask&1?1.f:0.f,mask&2?1.f:0.f,mask&4?1.f:0.f,mask&8?1.f:0.f),Temp(first+3),raw(offset)});
                    replacements.push_back({offset,Temp(first+3)});++changed;
                }
            }
        }
        // Rebuild candidates from the scaled per-volume ray boxes. The native
        // occupancy mask is refined with fixed-size DDA steps and otherwise cuts
        // away the added volume. Scene depth remains in the original integrator.
        if(code[0]==0x50&&op==45&&args.size()==3&&args[0].type==0&&args[2].type==7&&args[2].index==18){
            Emit(after,31|0x40000u,{Cb(ConstantSlot,10,85)});
            Emit(after,54,{Dest(first+3,1),{0x00004001u,0}});
            for(unsigned i=0;i<16;++i){
                Emit(after,0,{Dest(first),Cb(table,i),Cb(table,i+16)});
                Emit(after,56,{Dest(first),Temp(first),Literal(.5f,.5f,.5f)});
                Emit(after,0,{Dest(first+1),Cb(table,i),negate(Temp(first))});
                Emit(after,50,{Dest(first+1),Temp(first+1),Cb(ConstantSlot,9,0),Temp(first)});
                Emit(after,0,{Dest(first+2),Cb(table,i+16),negate(Temp(first))});
                Emit(after,50,{Dest(first+2),Temp(first+2),Cb(ConstantSlot,9,0),Temp(first)});
                Emit(after,0,{Dest(first+1),Temp(first+1),negate(Cb(1,26))});
                Emit(after,0,{Dest(first+2),Temp(first+2),negate(Cb(1,26))});
                const Words ray{0x00101e46u,1};
                Emit(after,14,{Dest(first+1),Temp(first+1),ray});
                Emit(after,14,{Dest(first+2),Temp(first+2),ray});
                Emit(after,51,{Dest(first),Temp(first+1),Temp(first+2)});
                Emit(after,52,{Dest(first+1),Temp(first+1),Temp(first+2)});
                Emit(after,52,{Dest(first+4,1),Temp(first,0),Temp(first,85)});
                Emit(after,52,{Dest(first+4,1),Temp(first+4,0),Temp(first,170)});
                Emit(after,51,{Dest(first+4,2),Temp(first+1,0),Temp(first+1,85)});
                Emit(after,51,{Dest(first+4,2),Temp(first+4,85),Temp(first+1,170)});
                Emit(after,29,{Dest(first+4,1),Temp(first+4,85),Temp(first+4,0)});
                Emit(after,49,{Dest(first+4,2),Literal(0,0,0),Temp(first+4,85)});
                Emit(after,49,{Dest(first+4,4),Literal(float(i),float(i),float(i)),Cb(table,0,255)});
                Emit(after,1,{Dest(first+4,1),Temp(first+4,0),Temp(first+4,85)});
                Emit(after,1,{Dest(first+4,1),Temp(first+4,0),Temp(first+4,170)});
                Emit(after,1,{Dest(first+4,1),Temp(first+4,0),{0x00004001u,1u<<i}});
                Emit(after,60,{Dest(first+3,1),Temp(first+3,0),Temp(first+4,0)});
            }
            if(code[0]==0x50)Emit(after,13,{Temp(first+3,0)});
            Emit(after,86,{raw(args[0]),Temp(first+3,0)});Emit(after,21,{});++changed;
        }
        // Half-resolution mask variants also retain a CPU-authored X clip interval.
        // The scaled mask boxes supply coverage; an old interval must not cut it off.
        if(expandScreenClip&&op==49&&args.size()==3&&args[0].type==0){
            for(unsigned i=1;i<=2;++i){const auto& a=args[i];const size_t ix=a.start+1+((code[a.start]&0x80000000u)?1:0);
                if(a.type!=8||a.index!=0||a.end!=ix+2||code[ix+1]!=0||args[3-i].type!=1)continue;
                const unsigned mask=(code[args[0].start]>>4)&15;unsigned lane=0;while(lane<4&&!(mask&(1u<<lane)))++lane;
                Emit(after,55,{raw(args[0]),Cb(ConstantSlot,10,85),Literal(0,0,0,0),Temp(args[0].index,lane*0x55u)});++changed;
            }
        }
        out.insert(out.end(),before.begin(),before.end());
        const size_t start=out.size();out.insert(out.end(),code.begin()+at,code.begin()+at+length);
        std::sort(replacements.begin(),replacements.end(),[](const auto& a,const auto& b){return a.first.start>b.first.start;});
        for(const auto& r:replacements){const size_t from=start+r.first.start-at,to=start+r.first.end-at;
            out.erase(out.begin()+from,out.begin()+to);out.insert(out.begin()+from,r.second.begin(),r.second.end());}
        if(op!=53)out[start]=(out[start]&~0x7f000000u)|(unsigned(out.size()-start)<<24);
        out.insert(out.end(),after.begin(),after.end());at+=length;
    }
    if(!changed)return 0;
    // Declaration precedes all instructions, so its offset is unchanged.
    out[temps]=first+6;out[1]=unsigned(out.size());code=std::move(out);return changed;
}
inline constexpr unsigned WarpTextureSlot=119, WarpSamplerSlot=13;
// Interpolate eight logical cell-centre samples rather than rounding the ray to
// one cell. Keep volume packing and native RGBA/frame blending intact.
inline unsigned FilterAtlas(Relight::IrradianceDxbc::Words& code,const std::vector<unsigned>& slots,unsigned densitySlot) {
    using namespace Relight::IrradianceDxbc;
    unsigned first=~0u;size_t temps=0,lastResource=0,lastSampler=0;
    for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t n=(code[at]>>24)&127;if(op==53)n=code[at+1];
        if(op==104){temps=at+1;first=code[temps];}if(op==88)lastResource=at;if(op==90)lastSampler=at;at+=n;}
    if(first==~0u||first>4078||!lastResource||!lastSampler||slots.empty())return 0;
    auto neg=[](Words v){v[0]|=0x80000000u;v.insert(v.begin()+1,0x41u);return v;};
    Words out{code[0],0};unsigned changed=0;
    for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t n=(code[at]>>24)&127;if(op==53)n=code[at+1];
        size_t cursor=at+1;uint32_t ext=code[at];while(ext&0x80000000u)ext=code[cursor++];const size_t operands=cursor;
        std::vector<Operand> args;if(op==72)while(cursor<at+n){Operand a;if(!ReadOperand(code,cursor,at+n,a))return 0;args.push_back(a);}
        auto raw=[&](const Operand& a){return Words(code.begin()+a.start,code.begin()+a.end);};
        if(op==72&&args.size()==5&&args[0].type==0&&args[2].type==7&&std::find(slots.begin(),slots.end(),args[2].index)!=slots.end()){
            auto sample=[&](Words dest,Words coord,Words resource,Words sampler){const size_t start=out.size();
                out.insert(out.end(),code.begin()+at,code.begin()+operands);
                for(const auto& v:{dest,coord,resource,sampler,raw(args[4])})out.insert(out.end(),v.begin(),v.end());
                out[start]=(out[start]&~0x7f000000u)|(unsigned(out.size()-start)<<24);};
            const unsigned coord=first,local=first+1,base=first+2,lower=first+3,weight=first+4,one=first+5,point=first+6,value=first+7,sum=first+8,w=first+9,warp=first+10;
            Emit(out,54,{Dest(coord,15),raw(args[1])});
            Emit(out,54,{Dest(warp,8),Literal(1,1,1,1)});
            Emit(out,31|0x40000u,{Cb(ConstantSlot,10,170)});
            // The collision lookup has 64 cells/side and two guard cells per slot.
            Emit(out,56,{Dest(base,1),Temp(coord,0),Literal(542.f/34.f,0,0)});
            Emit(out,65,{Dest(base,1),Temp(base,0)});
            // Decode this slot using integer bits (no exp/floor rounding at bit boundaries).
            Emit(out,28,{Dest(local,2),Temp(base,0)});
            Emit(out,28,{Dest(local,4),Cb(ConstantSlot,10,255)});
            Emit(out,85,{Dest(local,4),Temp(local,170),Temp(local,85)});
            Emit(out,1,{Dest(local,2),Temp(local,170),Words{0x00004001u,1u}});
            Emit(out,31|0x40000u,{Temp(local,85)});
            Emit(out,56,{Dest(local,1),Temp(coord,0),Literal(542,0,0)});
            Emit(out,50,{Dest(local,1),Temp(base,0),Literal(-34,0,0),Temp(local,0)});
            Emit(out,50,{Dest(point,1),Temp(local,0),Literal(2,0,0),Literal(0,0,0)});
            Emit(out,50,{Dest(point,1),Temp(base,0),Literal(66,0,0),Temp(point,0)});
            Emit(out,56,{Dest(point,1),Temp(point,0),Literal(1.f/1054.f,0,0)});
            Emit(out,54,{Dest(point,6),Temp(coord)});
            sample(Dest(warp,15),Temp(point),{0x00107e46u,WarpTextureSlot},{0x00106000u,WarpSamplerSlot});
            // Invalid/unprepared slots store the identity map, not another volume.
            Emit(out,54,{Dest(coord),Temp(warp)});Emit(out,21,{});Emit(out,21,{});
            Emit(out,31|0x40000u,{Cb(ConstantSlot,10,0)});
            Emit(out,56,{Dest(local),Temp(coord),Literal(542,32,32)});
            Emit(out,56,{Dest(base,1),Temp(local,0),Literal(1.f/34.f,0,0)});
            Emit(out,65,{Dest(base,1),Temp(base,0)});
            Emit(out,56,{Dest(base,1),Temp(base,0),Literal(34,0,0)});
            Emit(out,0,{Dest(local,1),Temp(local,0),neg(Temp(base,0))});
            Emit(out,50,{Dest(local),Temp(local),Cb(ConstantSlot,9,170),Literal(-.5f,-.5f,-.5f)});
            Emit(out,65,{Dest(lower),Temp(local)});
            Emit(out,0,{Dest(weight),Temp(local),neg(Temp(lower))});
            Emit(out,0,{Dest(one),Literal(1,1,1),neg(Temp(weight))});
            Emit(out,54,{Dest(sum,15),Literal(0,0,0,0)});
            for(unsigned corner=0;corner<8;++corner){
                Emit(out,0,{Dest(point),Temp(lower),Literal(.5f+(corner&1?1:0),.5f+(corner&2?1:0),.5f+(corner&4?1:0))});
                Emit(out,56,{Dest(point),Temp(point),Cb(ConstantSlot,9,255)});
                Emit(out,52,{Dest(point),Temp(point),Literal(.5f,.5f,.5f)});
                Emit(out,51,{Dest(point),Temp(point),Literal(31.5f,31.5f,31.5f)});
                Emit(out,0,{Dest(point,1),Temp(point,0),Temp(base,0)});
                Emit(out,56,{Dest(point),Temp(point),Literal(1.f/542.f,1.f/32.f,1.f/32.f)});
                sample(Dest(value,15),Temp(point),raw(args[2]),raw(args[3]));
                Emit(out,55,{Dest(w),Literal(corner&1?1.f:0.f,corner&2?1.f:0.f,corner&4?1.f:0.f),Temp(weight),Temp(one)});
                Emit(out,56,{Dest(w,1),Temp(w,0),Temp(w,85)});Emit(out,56,{Dest(w,1),Temp(w,0),Temp(w,170)});
                Emit(out,50,{Dest(sum,15),Temp(value),Temp(w,0),Temp(sum)});
            }
            Emit(out,54,{raw(args[0]),Temp(sum)});Emit(out,18,{});
            sample(raw(args[0]),Temp(coord),raw(args[2]),raw(args[3]));Emit(out,21,{});
            // Fade density at collision boundaries while preserving lighting intensity.
            // Fading both produces an opaque black fringe on partially covered cells.
            if(args[2].index==densitySlot)Emit(out,56,{raw(args[0]),Temp(args[0].index),Temp(warp,255)});++changed;
        }else out.insert(out.end(),code.begin()+at,code.begin()+at+n);
        if(at==lastResource)out.insert(out.end(),{0x04002858u,0x00107000u,WarpTextureSlot,0x00005555u});
        if(at==lastSampler)out.insert(out.end(),{0x0300005au,0x00106000u,WarpSamplerSlot});
        at+=n;
    }
    if(!changed)return 0;
    // Locate declaration again: inserted declarations may precede dcl_temps.
    for(size_t at=2;at<out.size();){unsigned op=out[at]&2047;size_t n=(out[at]>>24)&127;if(op==53)n=out[at+1];if(op==104){out[at+1]=first+11;break;}at+=n;}
    out[1]=unsigned(out.size());code=std::move(out);return changed;
}
inline bool Program(const void* data,size_t bytes,std::vector<uint8_t>& output,unsigned* reads=nullptr,ProgramInfo* info=nullptr) {
    using namespace Relight::IrradianceDxbc;
    output.clear();if(reads)*reads=0;if(info)*info={};
    if(!data||bytes<36||bytes>16*1024*1024)return false;
    const auto* src=static_cast<const uint8_t*>(data);
    auto word=[&](size_t at){uint32_t u;std::memcpy(&u,src+at,4);return u;};
    if(std::memcmp(src,"DXBC",4)||word(24)!=bytes)return false;
    const unsigned count=word(28);if(count>(bytes-32)/4)return false;
    size_t chunk=0;Words code;
    for(unsigned i=0;i<count;++i){size_t at=word(32+i*4);
        if(at<32+count*4||at>bytes-8||word(at+4)>bytes-at-8)return false;
        if(!std::memcmp(src+at,"SHEX",4)||!std::memcmp(src+at,"SHDR",4)){
            if(chunk||word(at+4)<8||word(at+4)%4)return false;
            chunk=at;code.resize(word(at+4)/4);std::memcpy(code.data(),src+at+8,code.size()*4);
        }
    }
    if(!chunk||code[1]!=code.size()||(code[0]!=0x50&&code[0]!=0x50050&&code[0]!=0x10050))return false;
    const bool compute=code[0]==0x50050;
    const bool vertex=code[0]==0x10050;
    bool material=false,view=false,table4=false,table5=false,volume13=false,volume14=false,volume15=false,volume16=false,volume17=false;
    std::vector<unsigned> atlasSlots;
    unsigned found=0,n=0,table=~0u;size_t lastCb=0;unsigned cbRows[14]={};bool volume1=false,volume12=false;
    for(size_t at=2;at<code.size();){uint32_t token=code[at];const unsigned op=token&2047;
        size_t length=(token>>24)&127;
        if(op==53){if(at+1>=code.size())return false;length=code[at+1];}
        if(!length||length>code.size()-at)return false;
        size_t cursor=at+1;uint32_t ext=token;
        while(ext&0x80000000u){if(cursor>=at+length)return false;ext=code[cursor++];}
        if(op==89){
            if(length!=4)return false;
            const unsigned slot=code[at+2],rows=code[at+3];
            if(slot==ConstantSlot)return false;
            if(slot<14)cbRows[slot]=rows;
            if(slot==0&&rows==9)material=true;
            if(slot==1&&rows==29)view=true;
            if(slot==4&&rows==156)table4=true;
            if(slot==5&&rows==156)table5=true;
            lastCb=at+length;
        }
        if(op==90&&length==3&&code[at+2]==WarpSamplerSlot)return false;
        if(op==88&&length>=4&&code[at+2]==WarpTextureSlot)return false;
        if(op==88&&length>=4&&((token>>11)&31)==5){
            const auto slot=code[at+2];if(slot==1||slot==12||(slot>=13&&slot<=15))atlasSlots.push_back(slot);
            if(code[at+2]==13)volume13=true;
            if(code[at+2]==14)volume14=true;
            if(code[at+2]==15)volume15=true;
            if(code[at+2]==16)volume16=true;
            if(code[at+2]==17)volume17=true;
            if(code[at+2]==1)volume1=true;
            if(code[at+2]==12)volume12=true;
        }
        // Declarations contain plain integers as well as operands. SM5 compute
        // declarations extend beyond the original SM4 declaration opcode range.
        if(op<88&&op!=53){
            while(cursor<at+length){Operand arg;if(!ReadOperand(code,cursor,at+length,arg))return false;
                if(!material||arg.type!=8||arg.index!=0)continue;
                const uint32_t operand=code[arg.start];
                // Two immediate indices, optionally an operand modifier, scalar swizzle.
                const size_t indexWord=arg.start+1+((operand&0x80000000u)?1:0);
                if(((operand>>20)&3)!=2||((operand>>22)&63)!=0||arg.end!=indexWord+2)return false;
                const unsigned row=code[indexWord+1],mode=(operand>>2)&3;
                unsigned lane;
                if(mode==2)lane=(operand>>4)&3;
                else if(mode==1){const unsigned swizzle=(operand>>4)&255;
                    lane=swizzle&3;if(swizzle!=lane*0x55u)continue;
                }else continue;
                for(unsigned i=0;i<10;++i){const auto& c=Controls[i];
                    if(row==c.row&&lane==c.lane){code[indexWord]=ConstantSlot;found|=1u<<i;++n;break;}}
            }
        }
        at+=length;
    }
    // Legacy folded-constant variants and unknown layouts deliberately stay native.
    // MBOIT's opacity/moment pass binds the volume at CB4/t14; the
    // colour pass uses CB5/t15. They must use identical appearance values.
    const bool volume=compute?((table4&&volume13)||(table5&&(volume14||volume15||volume16||volume17)))
        :((table4&&volume14)||(table5&&(volume15||volume16)));
    const bool appearance=!vertex&&material&&view&&volume&&found==0x3ffu;
    if(appearance)table=table5?5:4;
    else if(vertex&&cbRows[0]==25&&cbRows[1]==32)table=1; // instanced smoke mask boxes
    else if(!vertex&&!compute&&volume1){for(unsigned i=1;i<=3;++i)
        if(cbRows[i]==64&&(cbRows[0]==29||cbRows[1]==29))table=i;}
    else if(!vertex&&!compute&&volume12&&cbRows[0]==1&&cbRows[1]==27&&cbRows[3]==12&&cbRows[4]==156)table=4;
    else if(!vertex&&!compute&&volume12&&cbRows[0]==1&&cbRows[1]==27&&cbRows[3]==1946&&cbRows[4]==12&&cbRows[5]==156)table=5;
    // Redirected material reads in unrelated/partial programs must not escape.
    if(!appearance&&found)return false;
    if(!lastCb||table==~0u){
        if(info&&!vertex&&!compute&&cbRows[0]==27&&cbRows[1]==38){
            unsigned resources=0;
            for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t n=(code[at]>>24)&127;if(op==53)n=code[at+1];
                if(op==88&&n>=4){unsigned slot=code[at+2],dim=(code[at]>>11)&31;if(slot==12&&dim==6)resources|=1;if(slot==16&&dim==3)resources|=2;if(slot==17&&dim==3)resources|=4;}at+=n;}
            if(resources==7){info->smoke=true;info->composite=true;}
        }
        return false;
    }
    const unsigned geometry=Geometry(code,table,!vertex&&!compute&&volume1&&cbRows[0]==1&&cbRows[table]==64);
    if(!appearance){atlasSlots.clear();if(volume1)atlasSlots.push_back(1);else if(volume12)atlasSlots.push_back(12);}
    const unsigned densitySlot=atlasSlots.empty()?~0u:*std::min_element(atlasSlots.begin(),atlasSlots.end());
    const unsigned samples=FilterAtlas(code,atlasSlots,densitySlot);
    if(info){info->smoke=appearance||geometry;info->controls=found;info->reads=n;info->geometry=geometry;info->samples=samples;info->table=table;}
    if(!appearance&&!geometry)return false;
    if(appearance&&!ThinDensity(code))return false;
    // Added resource/sampler declarations can shift a differently ordered CB list.
    lastCb=0;for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t length=(code[at]>>24)&127;if(op==53)length=code[at+1];if(op==89)lastCb=at+length;at+=length;}
    code.insert(code.begin()+lastCb,{0x04000059u,0x00208000u,ConstantSlot,11});
    code[1]=unsigned(code.size());
    output.assign(src,src+32+count*4);
    auto put=[&](size_t at,uint32_t value){std::memcpy(output.data()+at,&value,4);};
    for(unsigned i=0;i<count;++i){const size_t at=word(32+i*4),dest=output.size();put(32+i*4,unsigned(dest));
        output.insert(output.end(),src+at,src+at+8);
        if(at==chunk){output.resize(output.size()+code.size()*4);std::memcpy(output.data()+dest+8,code.data(),code.size()*4);put(dest+4,unsigned(code.size()*4));}
        else output.insert(output.end(),src+at+8,src+at+8+word(at+4));
    }
    put(24,unsigned(output.size()));
    Environment::DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    if(reads)*reads=n;return true;
}
}
