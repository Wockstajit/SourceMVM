#pragma once
#include "../Relight/RelightIrradianceMath.h"
#include "../Relight/RelightMath.h"
#include <cmath>
#include <map>

namespace Filmmaker::Environment {
// Five raw blocker depths locate the penumbra. Thirty-two comparisons avoid visible
// steps from sparse sampling near extended emitters. The pattern stays fixed.
// The previous twelve-comparison disk produced distinct silhouettes beneath lampshades.
inline constexpr unsigned NativeLampShadowSamples = 32;
// The disk compares one receiver depth against taps several texels away. A wall seen at a grazing
// angle by a side cube face (a sconce lighting the wall it hangs on) then shadows itself everywhere
// except inside the face looking straight at it: a hard square of light. Each tap therefore gets a
// receiver-plane tolerance. For a perspective face, a plane of slope s (depth change per lateral
// unit) changes stored depth by about (1-depth)*s*guard*(offset/half reach) at that tap's offset,
// independent of distance. s=4 covers receivers up to ~76 degrees from the face axis.
inline constexpr float NativeLampReceiverSlope = 4;
inline constexpr float NativeLampReceiverTolerance =
    NativeLampReceiverSlope * Relight::StaticFaceGuard * Relight::ShadowFaceBlendWidth;
// Native first-person lighting can bypass local shadows through a view flag. Owned
// lights use their own atlas, so its valid tile controls the lookup instead. Match the
// complete atlas-row / positive-scale / bypass-zero / conjunction / guard sequence;
// unrelated uses of the same view flag, including the sun and material shading, survive.
inline unsigned OwnNativeLocalShadowGuards(Relight::IrradianceDxbc::Words& code) {
    using namespace Relight::IrradianceDxbc;
    struct Ins {size_t at,end;unsigned op;std::vector<Operand> operands;};
    std::vector<Ins> ins;
    for(size_t at=2;at<code.size();){unsigned op=code[at]&2047;size_t n=(code[at]>>24)&127;
        if(op==53){if(at+1>=code.size())return 0;n=code[at+1];}if(!n||n>code.size()-at)return 0;
        Ins i{at,at+n,op,{}};
        if(op==167||op==49||op==32||op==1||op==31||op==71){size_t c=at;uint32_t ext=code[c];while(ext>>31)ext=code[++c];++c;
            while(c<i.end){Operand o;if(!ReadOperand(code,c,i.end,o))return 0;i.operands.push_back(o);}}
        ins.push_back(std::move(i));at+=n;
    }
    auto zero=[&](const Operand& o){return o.type==4&&o.end==o.start+2&&code[o.start+1]==0;};
    auto component=[&](const Operand& o){const auto t=code[o.start];const unsigned mode=(t>>2)&3;
        return mode==2?int((t>>4)&3):mode==1&&((t>>4)&255)%85==0?int(((t>>4)&255)/85):-1;};
    auto dstMatches=[&](const Operand& d,const Operand& s){const unsigned mask=(code[d.start]>>4)&15;
        return d.type==0&&s.type==0&&d.index==s.index&&component(s)>=0&&mask==(1u<<component(s));};
    struct Patch {size_t at,end;Words words;};std::vector<Patch> patches;
    for(size_t j=0;j+4<ins.size();++j){const auto& load=ins[j];const auto& positive=ins[j+1];const auto& bypass=ins[j+2];const auto& both=ins[j+3];const auto& guard=ins[j+4];
        if(load.op!=167||load.operands.size()!=4||load.operands[2].type!=4||code[load.operands[2].start+1]!=208||load.operands[3].type!=7||
           positive.op!=49||positive.operands.size()!=3||!zero(positive.operands[1])||positive.operands[2].type!=0||positive.operands[2].index!=load.operands[0].index||component(positive.operands[2])!=2||
           bypass.op!=32||bypass.operands.size()!=3||!zero(bypass.operands[2])||
           both.op!=1||both.operands.size()!=3||!((dstMatches(positive.operands[0],both.operands[1])&&dstMatches(bypass.operands[0],both.operands[2]))||(dstMatches(positive.operands[0],both.operands[2])&&dstMatches(bypass.operands[0],both.operands[1])))||
           guard.op!=31||guard.operands.size()!=1||!dstMatches(both.operands[0],guard.operands[0]))continue;
        bool comparison=false;
        for(size_t k=j+5;k<ins.size()&&k<j+20;++k)if(ins[k].op==71&&ins[k].operands.size()==5&&ins[k].operands[2].type==7&&ins[k].operands[2].index==6)comparison=true;
        if(comparison){const auto& dst=bypass.operands[0];Words w;
            Emit(w,54,{Words(code.begin()+dst.start,code.begin()+dst.end),Words{0x00004001u,0xffffffffu}});
            patches.push_back({bypass.at,bypass.end,std::move(w)});}
    }
    for(auto p=patches.rbegin();p!=patches.rend();++p){code.erase(code.begin()+p->at,code.begin()+p->end);code.insert(code.begin()+p->at,p->words.begin(),p->words.end());}
    code[1]=unsigned(code.size());return unsigned(patches.size());
}
// The 32 original comparisons remain the reference. Inside a single bilinear
// footprint, a comparison gather at the extreme receiver depths can prove that
// every tap is identically zero or one. Mixed footprints retain the full disk.
template<class Compare,class Gather>
inline void NativeLampDisk(Relight::IrradianceDxbc::Words& out,unsigned first,
    const Relight::IrradianceDxbc::Words& center,const Relight::IrradianceDxbc::Words& reference,
    const Relight::IrradianceDxbc::Words& destination,Compare compare,Gather gather,
    bool classify,float width=float(Relight::AtlasWidth),float height=float(Relight::AtlasHeight)) {
    using namespace Relight::IrradianceDxbc;
    const unsigned uv=first,sum=first+1,tap=first+2,span=first+3,source=first+4,
        footprint=first+5,blocker=first+6,coords=first+7,faceWeight=first+8,plane=first+9;
    Emit(out,0x4001f,{Temp(span,0)});
    Emit(out,54,{Dest(footprint,8),Literal(1,1,1,1)}); // full disk required
    if(classify){
        Emit(out,24,{Dest(blocker,1),Temp(source,255),Literal(-12345,-12345,-12345)});
        Emit(out,24,{Dest(blocker,2),Temp(source,255),Literal(-12346,-12346,-12346)});
        Emit(out,60,{Dest(footprint,4),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,29,{Dest(blocker,1),Temp(plane,0),Literal(0,0,0)});
        Emit(out,1,{Dest(footprint,4),Temp(footprint,170),Temp(blocker,0)});
        Emit(out,29,{Dest(blocker,3),Temp(span),Literal(0,0,0)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,1,{Dest(footprint,4),Temp(footprint,170),Temp(blocker,0)});
        Emit(out,54,{Dest(coords,3),center});Emit(out,54,{Dest(coords,12),Temp(coords,0x44)});
        Emit(out,50,{Dest(coords,15),Temp(span,0x44),Literal(-1,-1,1,1),Temp(coords)});
        // Conservatively include the required 8-bit subtexel rounding interval.
        constexpr float padding=1.f/256;
        Emit(out,50,{Dest(coords,15),Temp(coords),Literal(width,height,width,height),Literal(-.5f-padding,-.5f-padding,-.5f+padding,-.5f+padding)});
        Emit(out,29,{Dest(blocker,15),Temp(coords),Literal(0,0,0,0)});
        Emit(out,49,{Dest(tap,15),Temp(coords),Literal(width-1,height-1,width-1,height-1)});
        Emit(out,1,{Dest(blocker,15),Temp(blocker),Temp(tap)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,170)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,255)});
        Emit(out,1,{Dest(footprint,4),Temp(footprint,170),Temp(blocker,0)});
        Emit(out,27,{Dest(coords,15),Temp(coords)});
        Emit(out,32,{Dest(blocker,3),Temp(coords,0x44),Temp(coords,0xee)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,1,{Dest(footprint,4),Temp(footprint,170),Temp(blocker,0)});
        Emit(out,0x4001f,{Temp(footprint,170)});
        constexpr float firstRadius=.125f;
        const float lastRadius=std::sqrt((NativeLampShadowSamples-.5f)/NativeLampShadowSamples);
        Emit(out,50,{Dest(source,4),Temp(plane,0),Literal(-firstRadius,-firstRadius,-firstRadius),reference});
        Emit(out,50,{Dest(source,8),Temp(plane,0),Literal(-lastRadius,-lastRadius,-lastRadius,-lastRadius),reference});
        gather(Dest(tap,15),center,Temp(source,170));
        Emit(out,29,{Dest(blocker,15),Temp(tap),Literal(1,1,1,1)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,170)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,255)});
        Emit(out,0x4001f,{Temp(blocker,0)});
        Emit(out,54,{Dest(sum,1),Literal(float(NativeLampShadowSamples),0,0)});
        Emit(out,54,{Dest(footprint,8),Literal(0,0,0)});
        Emit(out,18,{});
        gather(Dest(tap,15),center,Temp(source,255));
        Emit(out,24,{Dest(blocker,15),Temp(tap),Literal(0,0,0,0)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,85)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,170)});
        Emit(out,1,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,255)});
        Emit(out,0x4001f,{Temp(blocker,0)});
        Emit(out,54,{Dest(sum,1),Literal(0,0,0)});Emit(out,54,{Dest(footprint,8),Literal(0,0,0)});
        Emit(out,21,{});Emit(out,21,{});Emit(out,21,{});
    }
    Emit(out,0x4001f,{Temp(footprint,255)});
    Emit(out,54,{Dest(sum,1),Literal(0,0,0)});
    for(unsigned k=0;k<NativeLampShadowSamples;++k){
        const float r=std::sqrt((k+.5f)/NativeLampShadowSamples),a=k*2.39996323f;
        Emit(out,50,{Dest(uv,3),Temp(span),Literal(r*std::cos(a),r*std::sin(a),0),center});
        Emit(out,50,{Dest(plane,2),Temp(plane,0),Literal(-r,-r,-r),reference});
        compare(Dest(tap,1),Temp(uv,0x04),Words{0x0010000Au|(1u<<4),plane});
        Emit(out,0,{Dest(sum,1),Temp(sum,0),Temp(tap,0)});
    }
    Emit(out,21,{});
    Emit(out,56,{Dest(sum,1),Temp(sum,0),Temp(faceWeight,255)});
    constexpr float weight=1.f/NativeLampShadowSamples;
    Emit(out,56,{destination,Temp(sum,0),Literal(weight,weight,weight,weight)});
    Emit(out,18,{});compare(Dest(sum,1),center,reference);
    Emit(out,56,{destination,Temp(sum,0),Temp(faceWeight,255)});Emit(out,21,{});
}
// Replace only the verified clustered-light nine-comparison kernel. The sun
// cascade loop and unfamiliar permutations retain their existing lookups.
inline unsigned SmoothNativeLampKernels(Relight::IrradianceDxbc::Words& code,bool classifySmallFilters=true) {
    using namespace Relight::IrradianceDxbc;
    struct Ins {size_t at,end;unsigned op;std::vector<Operand> operands;};
    std::vector<Ins> ins;unsigned temps=0;size_t declaration=0;
    for(size_t at=2;at<code.size();) {
        const auto token=code[at];const unsigned op=token&2047;
        size_t length=(token>>24)&127;if(op==53){if(at+1>=code.size())return 0;length=code[at+1];}
        if(!length||length>code.size()-at)return 0;
        Ins i{at,at+length,op,{}};
        if(op==104&&length==2){temps=code[at+1];declaration=at+1;}
        if(op==71||op==50||op==0||op==167) {
            size_t cursor=at;uint32_t ext=token;while(ext>>31){ext=code[++cursor];}++cursor;
            while(cursor<i.end){Operand o;if(!ReadOperand(code,cursor,i.end,o))return 0;i.operands.push_back(o);}
        }
        ins.push_back(std::move(i));at+=length;
    }
    if(!temps||temps>4084)return 0;
    struct Patch {size_t end;Words words;};std::map<size_t,Patch> patches;unsigned kernels=0;
    auto words=[&](const Operand& o){return Words(code.begin()+o.start,code.begin()+o.end);};
    for(size_t first=0;first<ins.size();++first) {
        const auto& sample=ins[first];if(sample.op!=71||sample.operands.size()!=5||
            sample.operands[2].type!=7||sample.operands[2].index!=6)continue;
        // The clustered kernel starts after a saturated depth bias and two UV adds.
        // Unlike the sun loop, its final coordinates derive from a light's atlas rect.
        size_t rect=first;bool found=false;
        for(size_t back=1;back<=5&&back<=first;++back){const auto& m=ins[first-back];
            if(m.op==50&&m.operands.size()==4&&m.operands[0].type==0&&
               m.operands[2].type==0&&m.operands[3].type==0){rect=first-back;found=true;break;}}
        if(!found)continue;
        std::vector<size_t> samples;unsigned depth=0;size_t end=first;bool valid=true;
        for(size_t j=first;j<ins.size()&&j<first+45;++j){const auto& i=ins[j];
            if(i.op==71){if(i.operands.size()!=5||words(i.operands[2])!=words(sample.operands[2])||
                words(i.operands[3])!=words(sample.operands[3])||words(i.operands[4])!=words(sample.operands[4])){valid=false;break;}samples.push_back(j);}
            if(i.op==31)++depth;
            if(i.op==21){if(!depth){valid=false;break;}if(!--depth){end=j;break;}}
        }
        if(!valid||samples.size()!=9||end<=samples.back()||samples.back()+1>=ins.size())continue;
        const auto& last=ins[samples.back()];const auto& mad=ins[samples.back()+1];
        if(mad.op!=50||mad.operands.size()!=4||mad.operands[2].type!=8||
           mad.operands[2].index!=3||code[mad.operands[2].end-1]!=1)continue;
        const auto& atlasRect=ins[rect].operands[2];
        Words radius=Temp(atlasRect.index,0xee); // z,w: half tile reach, including its guard
        Words out;
        const unsigned uv=temps,sum=temps+1,tap=temps+2,span=temps+3,
            source=temps+4,footprint=temps+5,blocker=temps+6,coords=temps+7,faceWeight=temps+8,
            plane=temps+9;
        // Keep face-relative NDC before the native atlas MAD overwrites it.
        Emit(out,54,{Dest(faceWeight,3),words(ins[rect].operands[1])});
        Emit(out,54,{Dest(faceWeight,8),Literal(1,1,1,1)});
        patches.emplace(ins[rect].at,Patch{ins[rect].at,std::move(out)});out.clear();
        // Capture the rect before the native UV adds overwrite its register.
        Emit(out,56,{Dest(span,3),radius,Cb(3,0,0)});
        // Rect w is negative. Abs keeps the circular footprint symmetric.
        // max(x,-x) supplies abs without an extended operand.
        Words neg=Temp(span);neg[0]|=0x80000000u;neg.insert(neg.begin()+1,0x41u);
        Emit(out,52,{Dest(span,3),Temp(span),neg});
        Emit(out,51,{Dest(span,3),Temp(span),Cb(3,0,0xa9)}); // y,z guard reach
        Emit(out,54,{Dest(footprint,12),radius});
        Words negFootprint=Temp(footprint);negFootprint[0]|=0x80000000u;negFootprint.insert(negFootprint.begin()+1,0x41u);
        Emit(out,52,{Dest(footprint,12),Temp(footprint),negFootprint});
        // Capture owned source coefficients before the native kernel overwrites
        // the light index with its flag bits. Clone the verified atlas-row load.
        size_t load=rect;bool sourceFound=false;
        for(size_t back=1;back<=12&&back<=rect;++back){const auto& i=ins[rect-back];
            if(i.op==167&&i.operands.size()==4&&i.operands[2].type==4&&
               code[i.operands[2].start+1]==208){load=rect-back;sourceFound=true;break;}}
        if(sourceFound){
            const auto& i=ins[load];Words fetch(code.begin()+i.at,code.begin()+i.operands[0].start);
            for(const auto& o:{Dest(source,15),words(i.operands[1]),Words{0x00004001u,336},words(i.operands[3])})fetch.insert(fetch.end(),o.begin(),o.end());
            fetch[0]=(fetch[0]&~0x7f000000u)|(uint32_t(fetch.size())<<24);
            patches.emplace(i.end,Patch{i.end,std::move(fetch)});
        }
        patches.emplace(ins[rect].end,Patch{ins[rect].end,std::move(out)});
        out.clear();
        const auto center=words(last.operands[1]),reference=words(sample.operands[4]);
        // plane.x: receiver-plane tolerance per unit of disk radius; zero unless computed below.
        Emit(out,54,{Dest(plane,1),Literal(0,0,0)});
        if(sourceFound){
            Emit(out,24,{Dest(tap,1),Temp(source,255),Literal(-12345,-12345,-12345)});
            Emit(out,24,{Dest(uv,1),Temp(source,255),Literal(-12346,-12346,-12346)});
            Emit(out,60,{Dest(tap,1),Temp(tap,0),Temp(uv,0)});
            Emit(out,1,{Dest(tap,1),Temp(tap,0),Cb(3,0,0)});
            Emit(out,0x4001f,{Temp(tap,0)});
            Emit(out,56,{Dest(coords,12),Temp(source,0xaa),Literal(0,0,1.f/Relight::AtlasWidth,1.f/Relight::AtlasHeight)});
            // Search the projected source footprint, bounded to this tile's guard.
            Emit(out,50,{Dest(tap,1),reference,Temp(source,0),Literal(0,0,0)});
            Words negative=Temp(tap,0);negative[0]|=0x80000000u;negative.insert(negative.begin()+1,0x41u);
            Emit(out,0,{Dest(tap,1),Temp(source,0x55),negative});
            Emit(out,56,{Dest(footprint,3),Temp(footprint,0xee),Temp(tap,0)});
            Words negSpan=Temp(footprint);negSpan[0]|=0x80000000u;negSpan.insert(negSpan.begin()+1,0x41u);
            Emit(out,52,{Dest(footprint,3),Temp(footprint),negSpan});
            Emit(out,51,{Dest(footprint,3),Temp(footprint),Temp(coords,0xee)});
            Emit(out,54,{Dest(blocker,3),Literal(0,0,0)});
            for(unsigned k=0;k<5;++k){
                const float r=k? .8f:0,a=(k-1)*1.570796327f;
                Emit(out,50,{Dest(uv,3),Temp(footprint),Literal(r*std::cos(a),r*std::sin(a),0),center});
                Emit(out,56,{Dest(coords,3),Temp(uv),Literal(float(Relight::AtlasWidth),float(Relight::AtlasHeight),0)});
                Emit(out,27,{Dest(coords,3),Temp(coords)}); // ftoi texel coordinates
                Emit(out,54,{Dest(uv,12),Words{0x00004001u,0}});
                // Raw depth needs no additional sampler or mutable native state.
                Emit(out,54,{Dest(uv,3),Temp(coords)});
                Emit(out,0x8000002du,{{0x800000c2u,0x00155543u},Dest(tap,1),Temp(uv),words(sample.operands[2])});
                Emit(out,49,{Dest(tap,2),Temp(tap,0),reference});
                Emit(out,55,{Dest(tap,1),Temp(tap,0x55),Temp(tap,0),Literal(0,0,0)});
                Emit(out,0,{Dest(blocker,1),Temp(blocker,0),Temp(tap,0)});
                Emit(out,55,{Dest(tap,2),Temp(tap,0x55),Literal(1,1,1),Literal(0,0,0)});
                Emit(out,0,{Dest(blocker,2),Temp(blocker,0x55),Temp(tap,0x55)});
            }
            Emit(out,54,{Dest(tap,8),Temp(blocker,0x55)});
            Emit(out,52,{Dest(blocker,2),Temp(blocker,0x55),Literal(1,1,1)});
            Emit(out,14,{Dest(blocker,1),Temp(blocker,0),Temp(blocker,0x55)});
            Words negBlocker=Temp(blocker,0);negBlocker[0]|=0x80000000u;negBlocker.insert(negBlocker.begin()+1,0x41u);
            Emit(out,0,{Dest(tap,1),reference,negBlocker});
            Emit(out,52,{Dest(tap,1),Temp(tap,0),Literal(0,0,0)});
            Emit(out,56,{Dest(tap,1),Temp(tap,0),Temp(source,0)});
            // Existing artistic angle dial is a multiplier around its 1 degree default.
            Emit(out,56,{Dest(tap,1),Temp(tap,0),Cb(3,0,0)});
            Emit(out,56,{Dest(tap,1),Temp(tap,0),Literal(57.2943f,57.2943f,57.2943f)});
            Emit(out,56,{Dest(span,3),Temp(footprint,0xee),Temp(tap,0)});
            Words neg=Temp(span);neg[0]|=0x80000000u;neg.insert(neg.begin()+1,0x41u);
            Emit(out,52,{Dest(span,3),Temp(span),neg});
            Emit(out,51,{Dest(span,3),Temp(span),Temp(coords,0xee)});
            Emit(out,55,{Dest(span,3),Temp(tap,255),Temp(span),Literal(0,0,0)});
            // plane.x = tolerance * saturate(1-depth) * (disk radius / half tile reach).
            Emit(out,14,{Dest(uv,3),Temp(span),Temp(footprint,0xee)});
            Emit(out,52,{Dest(uv,1),Temp(uv,0),Temp(uv,0x55)});
            Emit(out,50|0x2000,{Dest(plane,1),reference,Literal(-1,-1,-1),Literal(1,1,1)});
            Emit(out,56,{Dest(plane,1),Temp(plane,0),Temp(uv,0)});
            constexpr float tolerance=NativeLampReceiverTolerance;
            Emit(out,56,{Dest(plane,1),Temp(plane,0),Literal(tolerance,tolerance,tolerance)});
            Emit(out,21,{});
        }
        if(sourceFound){
            // Normalize the three positive cube-axis weights. At an edge each
            // adjacent face contributes one half; at a corner all three sum to one.
            Emit(out,24,{Dest(tap,1),Temp(source,255),Literal(-12346,-12346,-12346)});
            Emit(out,0x4001f,{Temp(tap,0)});
            Emit(out,56,{Dest(faceWeight,3),Temp(faceWeight),Literal(Relight::ShadowFaceBlendWidth,Relight::ShadowFaceBlendWidth,0)});
            Emit(out,54,{Dest(faceWeight,4),Literal(1,1,1)});
            Words negative=Temp(faceWeight);negative[0]|=0x80000000u;negative.insert(negative.begin()+1,0x41u);
            Emit(out,52,{Dest(faceWeight,7),Temp(faceWeight),negative});
            Emit(out,52,{Dest(tap,1),Temp(faceWeight,0),Temp(faceWeight,0x55)});
            Emit(out,52,{Dest(tap,1),Temp(tap,0),Temp(faceWeight,0xaa)});
            Emit(out,14,{Dest(faceWeight,7),Temp(faceWeight),Temp(tap,0)});
            constexpr float inverse=1/(1-Relight::ShadowFaceBlendStart);
            Emit(out,50|0x2000,{Dest(faceWeight,7),Temp(faceWeight),Literal(inverse,inverse,inverse),Literal(-Relight::ShadowFaceBlendStart*inverse,-Relight::ShadowFaceBlendStart*inverse,-Relight::ShadowFaceBlendStart*inverse)});
            Emit(out,56,{Dest(sum,7),Temp(faceWeight),Temp(faceWeight)});
            Emit(out,50,{Dest(uv,7),Temp(faceWeight),Literal(-2,-2,-2),Literal(3,3,3)});
            Emit(out,56,{Dest(faceWeight,7),Temp(sum),Temp(uv)});
            Emit(out,16,{Dest(tap,1),Temp(faceWeight),Literal(1,1,1)});
            Emit(out,14,{Dest(faceWeight,8),Temp(faceWeight,0xaa),Temp(tap,0)});
            Emit(out,21,{});
        }
        auto compare=[&](const Words& destination,const Words& coordinates,const Words& depth){
            Words cmp(code.begin()+sample.at,code.begin()+sample.operands[0].start);
            for(const auto& o:{destination,coordinates,words(sample.operands[2]),words(sample.operands[3]),depth})cmp.insert(cmp.end(),o.begin(),o.end());
            cmp[0]=(cmp[0]&~0x7f000000u)|(uint32_t(cmp.size())<<24);out.insert(out.end(),cmp.begin(),cmp.end());
        };
        auto gather=[&](const Words& destination,const Words& coordinates,const Words& depth){
            auto sampler=words(sample.operands[3]);sampler[0]=(sampler[0]&~0xfffu)|0xau; // gather selects R explicitly
            Emit(out,0x8000007eu,{{0x800000c2u,0x00155543u},destination,coordinates,
                Words{0x00107e46u,6},sampler,depth});
        };
        // Unfamiliar sample controls/offsets keep the original comparison disk.
        const bool simpleSample=sample.operands[0].start==sample.at+3&&code[sample.at+1]==0x800000c2u&&code[sample.at+2]==0x00155543u;
        NativeLampDisk(out,temps,center,reference,words(mad.operands[0]),compare,gather,classifySmallFilters&&sourceFound&&simpleSample);
        patches.emplace(sample.at,Patch{ins[end].end,std::move(out)});first=end;++kernels;
    }
    if(patches.empty())return 0;
    code[declaration]=temps+10;
    Words output;size_t cursor=0;for(const auto& [at,p]:patches){output.insert(output.end(),code.begin()+cursor,code.begin()+at);
        output.insert(output.end(),p.words.begin(),p.words.end());cursor=p.end;}
    output.insert(output.end(),code.begin()+cursor,code.end());output[1]=unsigned(output.size());code=std::move(output);
    return kernels;
}
}
