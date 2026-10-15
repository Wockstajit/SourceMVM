#pragma once
#include "../Relight/RelightIrradianceMath.h"
#include <algorithm>
namespace Filmmaker::Environment {
// The native mask row remains unchanged. Its mirrored occupancy row is enabled
// only for raster tiles, which are uniform across a pixel quad. World-projected
// first-person coordinates retain the original loop and derivative behavior.
inline bool NativeSparseClusterProgram(const void* data,size_t size,std::vector<uint8_t>& output) {
    using namespace Relight::IrradianceDxbc;
    output.clear();if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t at){uint32_t v;memcpy(&v,p+at,4);return v;};
    if(word(0)!=0x43425844u||word(24)!=size)return false;
    const unsigned chunks=word(28);if(chunks>(size-32)/4)return false;
    size_t program=0;Words code;
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4);
        if(at<32+chunks*4||at>size-8||word(at+4)>size-at-8)return false;
        if(word(at)==0x52444853u||word(at)==0x58454853u){
            if(program||word(at+4)<8||word(at+4)%4)return false;program=at;
            code.resize(word(at+4)/4);memcpy(code.data(),p+at+8,code.size()*4);
        }
    }
    if(!program||code[0]!=0x50||code[1]!=code.size())return false;
    struct Ins {size_t at,end;unsigned op;std::vector<Operand> args;};std::vector<Ins> ins;
    unsigned temps=0;size_t tempDecl=0;bool mask=false,cb3=false,cb4=false,rasterSelector=false;
    auto scalar=[&](const Operand& o){const auto t=code[o.start];const unsigned mode=(t>>2)&3;
        return mode==2?int((t>>4)&3):mode==1&&((t>>4)&255)%85==0?int(((t>>4)&255)/85):-1;};
    auto cb=[&](const Operand& o,unsigned slot,unsigned row,int lane){return o.type==8&&o.index==slot&&
        o.end==o.start+3&&code[o.start+2]==row&&scalar(o)==lane;};
    auto same=[&](const Operand& a,const Operand& b){return a.type==0&&b.type==0&&a.index==b.index&&scalar(a)>=0&&scalar(a)==scalar(b);};
    auto dst=[&](const Operand& d,const Operand& s){return d.type==0&&s.type==0&&d.index==s.index&&
        scalar(s)>=0&&((code[d.start]>>4)&15)==(1u<<scalar(s));};
    auto literal=[&](const Operand& o,uint32_t value){return o.type==4&&o.end==o.start+2&&code[o.start+1]==value;};
    for(size_t at=2;at<code.size();){const unsigned op=code[at]&2047;size_t n=(code[at]>>24)&127;
        if(op==53){if(at+1>=code.size())return false;n=code[at+1];}if(!n||n>code.size()-at)return false;
        Ins i{at,at+n,op,{}};
        if(op==104&&n==2){temps=code[at+1];tempDecl=at+1;}
        if(op==89&&n==4&&code[at+1]==0x00208e46u){cb3|=code[at+2]==3&&code[at+3]>=24;cb4|=code[at+2]==4&&code[at+3]>=3;}
        if(op==162&&n==4&&code[at+2]==13&&code[at+3]==4)mask=true;
        if((op<=87&&op!=53)||op==167){
            size_t c=at;uint32_t ext=code[c];while(ext>>31){if(++c>=i.end)return false;ext=code[c];}++c;
            while(c<i.end){Operand o;if(!ReadOperand(code,c,i.end,o))return false;i.args.push_back(o);}
        }
        if(op==55&&i.args.size()==4&&cb(i.args[1],4,2,0)&&i.args[3].type==1)rasterSelector=true;
        ins.push_back(std::move(i));at+=n;
    }
    if(!temps||temps>4094||!cb3||!cb4||!mask||!rasterSelector)return false;
    auto writes=[&](const Ins& i,const Operand& target){if(i.args.empty())return false;const auto& d=i.args[0];
        return d.type==0&&target.type==0&&d.index==target.index&&(code[d.start]&15)==2&&scalar(target)>=0&&
            (((code[d.start]>>4)&15)&(1u<<scalar(target)));};
    auto constRow=[&](const Operand& o,unsigned slot,unsigned row){return o.type==8&&o.index==slot&&
        ((code[o.start]>>25)&7)==0&&code[o.end-1]==row;};
    auto sourceLane=[&](const Operand& o,unsigned lane){const unsigned t=code[o.start],mode=(t>>2)&3;
        return mode==2?int((t>>4)&3):mode==1?int((t>>(4+2*lane))&3):-1;};
    // Require the contiguous shipped raster address sequence. The selector's
    // zero branch must be SV_Position.xy, followed only by viewport subtraction,
    // integer conversion, tile shift and the two native address imads.
    auto rasterRow=[&](size_t definition){if(definition<5)return false;
        const unsigned ops[]{55,0,28,85,35,35};
        std::vector<std::array<int,4>> symbols(temps);for(auto& row:symbols)row.fill(0);
        auto value=[&](const Operand& o,unsigned lane){const int c=sourceLane(o,lane);return o.type==0&&o.index<temps&&c>=0?symbols[o.index][c]:0;};
        for(unsigned step=0;step<6;++step){const auto& i=ins[definition-5+step];if(i.op!=ops[step]||i.args.empty())return false;
            const auto& d=i.args[0];if(d.type!=0||d.index>=temps||(code[d.start]&15)!=2)return false;
            std::array<int,4> result{};const unsigned dm=(code[d.start]>>4)&15;
            for(unsigned lane=0;lane<4;++lane)if(dm&(1u<<lane)){
                if(step==0){if(i.args.size()!=4||!cb(i.args[1],4,2,0)||i.args[3].type!=1)return false;
                    bool position=false;for(const auto& decl:ins)if(decl.op==100&&decl.end-decl.at==4&&code[decl.at+2]==i.args[3].index&&code[decl.at+3]==1)position=true;
                    if(!position)return false;const int c=sourceLane(i.args[3],lane);result[lane]=c==0?1:c==1?2:0;
                }else if(step==1){if(i.args.size()!=3||!constRow(i.args[2],1,20)||!(code[i.args[2].start]>>31))return false;
                    result[lane]=value(i.args[1],lane);if(sourceLane(i.args[2],lane)!=(result[lane]==1?0:result[lane]==2?1:-1))return false;
                }else if(step==2){if(i.args.size()!=2)return false;result[lane]=value(i.args[1],lane);}
                else if(step==3){if(i.args.size()!=3||!cb(i.args[2],3,22,0))return false;result[lane]=value(i.args[1],lane);}
                else if(step==4){if(i.args.size()!=4||value(i.args[1],lane)!=2||!cb(i.args[2],3,22,1)||value(i.args[3],lane)!=1)return false;result[lane]=3;}
                else {if(i.args.size()!=4||value(i.args[1],lane)!=3||!cb(i.args[2],3,19,2)||!cb(i.args[3],3,19,1))return false;result[lane]=4;}
            }
            for(unsigned lane=0;lane<4;++lane)if(dm&(1u<<lane))symbols[d.index][lane]=result[lane];
        }
        return true;
    };
    for(const auto& i:ins)for(const auto& o:i.args)if(constRow(o,3,19)){
        const auto t=code[o.start];const unsigned mode=(t>>2)&3;
        if(mode==2&&((t>>4)&3)==3)return false;
        if(mode==1)for(unsigned c=0;c<4;++c)if(((t>>(4+2*c))&3)==3)return false;
    }
    size_t first=0,last=0,afterBreak=0;Words indexSource,indexDest,tileSource,loadPrefix,loadOffset,loadResource;
    for(size_t j=0;j+7<ins.size();++j){
        const auto& loop=ins[j];const auto& bound=ins[j+1];const auto& stop=ins[j+2];
        if(loop.op!=48||bound.op!=80||bound.args.size()!=3||!cb(bound.args[2],3,19,2)||
           bound.args[1].type!=0||scalar(bound.args[1])<0||stop.op!=3||stop.args.size()!=1||!dst(bound.args[0],stop.args[0])||
           !(code[stop.at]&(1u<<18)))continue;
        const auto& ia=ins[j+3];const auto& la=ins[j+4];
        const bool packed=ins[j+5].op==167;
        const auto& ib=packed?ia:ins[j+5];const auto& lb=ins[packed?j+5:j+6];
        struct Lane {unsigned reg=~0u;int component=-1;};
        auto address=[&](const Ins& l,const Ins& add){Lane base;
            if(add.op!=30||add.args.size()!=3||l.op!=167||l.args.size()!=4||!literal(l.args[2],0)||l.args[3].type!=7||l.args[3].index!=13)return base;
            const auto& a=l.args[1];const auto& d=add.args[0];const int lane=scalar(a);
            if(a.type!=0||d.type!=0||a.index!=d.index||lane<0||!(((code[d.start]>>4)&15)&(1u<<lane)))return base;
            for(unsigned source=1;source<3;++source){const auto& w=add.args[source];const auto& r=add.args[3-source];
                if(w.type==0&&w.index==bound.args[1].index&&sourceLane(w,lane)==scalar(bound.args[1])&&
                   r.type==0&&r.end==r.start+2&&!(code[r.start]>>31))base={r.index,sourceLane(r,lane)};
            }
            return base;
        };
        const Lane bases[]{address(la,ia),address(lb,ib)};Lane tile;bool slice=false;
        for(const auto& base:bases){if(base.reg==~0u||base.component<0)continue;size_t definition=0;
            for(size_t k=0;k<j;++k)if(!ins[k].args.empty()){
                const auto& d=ins[k].args[0];if(d.type==0&&d.index==base.reg&&(code[d.start]&15)==2&&(((code[d.start]>>4)&15)&(1u<<base.component)))definition=k;
            }
            if(rasterRow(definition))tile=base;
            const auto& m=ins[definition];slice|=m.op==35&&m.args.size()==4&&cb(m.args[2],3,19,2)&&cb(m.args[3],3,19,0);
        }
        if(tile.reg==~0u||!slice)continue;
        unsigned depth=1;size_t end=j+1;bool valid=true;unsigned increments=0;
        for(;end<ins.size();++end){const auto& i=ins[end];
            if(i.op==48){++depth;continue;}
            if(i.op==22){if(!--depth)break;continue;}
            if(depth==1){
                if(i.op!=80&&i.op!=3&&i.op!=30&&i.op!=167&&i.op!=1&&i.op!=41&&i.op!=54){valid=false;break;}
                if(i.op==3&&end!=j+2){valid=false;break;}
                if(i.op==30&&i.args.size()==3&&dst(i.args[0],bound.args[1])){
                    if(!same(i.args[1],bound.args[1])||!literal(i.args[2],1)){valid=false;break;}
                    ++increments;indexDest.assign(code.begin()+i.args[0].start,code.begin()+i.args[0].end);
                }
            }
        }
        if(!valid||end>=ins.size()||increments!=1||first)return false;
        first=loop.at;last=ins[end].end;afterBreak=stop.end;
        indexSource.assign(code.begin()+bound.args[1].start,code.begin()+bound.args[1].end);
        tileSource=Temp(tile.reg,unsigned(tile.component)*85);
        loadPrefix.assign(code.begin()+la.at,code.begin()+la.args[0].start);
        loadOffset.assign(code.begin()+la.args[2].start,code.begin()+la.args[2].end);
        loadResource.assign(code.begin()+la.args[3].start,code.begin()+la.args[3].end);
    }
    if(!first||indexDest.empty())return false;
    auto uintLiteral=[](uint32_t v){return Words{0x00004001u,v};};
    auto load=[&](Words& out,const Words& d,const Words& a){const size_t start=out.size();out.insert(out.end(),loadPrefix.begin(),loadPrefix.end());
        for(const auto* o:std::array<const Words*,4>{&d,&a,&loadOffset,&loadResource})out.insert(out.end(),o->begin(),o->end());
        out[start]=(out[start]&0x80ffffffu)|(uint32_t(out.size()-start)<<24);};
    Words pre;
    Emit(pre,32,{Dest(temps,4),Cb(4,2,0),uintLiteral(0)});
    Emit(pre,32,{Dest(temps,8),Cb(3,22,0),uintLiteral(5)});
    Emit(pre,1,{Dest(temps,4),Temp(temps,170),Temp(temps,255)});
    Emit(pre,1,{Dest(temps,4),Temp(temps,170),Cb(3,19,255)});
    Emit(pre,31|(1u<<18),{Temp(temps,170)});
    Emit(pre,30,{Dest(temps,2),tileSource,Cb(3,19,255)});
    load(pre,Dest(temps,1),Temp(temps,85));
    // Always execute the final native word, even when empty. This preserves
    // its scratch-register writes and the observable loop exit state.
    Emit(pre,30,{Dest(temps,2),Cb(3,19,170),uintLiteral(0xffffffffu)});
    Emit(pre,41,{Dest(temps,2),uintLiteral(1),Temp(temps,85)});
    Emit(pre,60,{Dest(temps,1),Temp(temps,0),Temp(temps,85)});
    Emit(pre,21,{});
    Words select;
    Emit(select,31|(1u<<18),{Temp(temps,170)});
    Emit(select,3,{Temp(temps,0)}); // breakc_z; no remaining word
    Emit(select,136,{indexDest,Temp(temps,0)});
    Emit(select,30,{Dest(temps,2),Temp(temps,0),uintLiteral(0xffffffffu)});
    Emit(select,1,{Dest(temps,1),Temp(temps,0),Temp(temps,85)});
    Emit(select,21,{});
    Words patched{code[0],0};
    for(const auto& i:ins){
        if(i.at==first)patched.insert(patched.end(),pre.begin(),pre.end());
        if(i.op==104){patched.insert(patched.end(),{0x02000068u,temps+1});continue;}
        patched.insert(patched.end(),code.begin()+i.at,code.begin()+i.end);
        if(i.end==afterBreak)patched.insert(patched.end(),select.begin(),select.end());
    }
    patched[1]=unsigned(patched.size());output.assign(p,p+32+chunks*4);
    auto put=[&](size_t at,uint32_t v){memcpy(output.data()+at,&v,4);};
    for(unsigned i=0;i<chunks;++i){const size_t at=word(32+i*4),dest=output.size();put(32+i*4,unsigned(dest));
        if(at==program){output.insert(output.end(),p+at,p+at+8);output.resize(output.size()+patched.size()*4);memcpy(output.data()+dest+8,patched.data(),patched.size()*4);put(dest+4,unsigned(patched.size()*4));}
        else output.insert(output.end(),p+at,p+at+8+word(at+4));
    }
    put(24,unsigned(output.size()));DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);return true;
}
}
