#pragma once
// Replace the baked irradiance query inside a native material program. The rest of the
// program (normal maps, BRDF, layers, alpha tests, emission and blending) stays native.
// No lightmap UVs or offline irradiance are used by the replacement query.
#include "../Environment/EnvironmentDxbcHash.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace Filmmaker::Relight {
inline constexpr unsigned IrradianceWorldSlot = 124, IrradianceDistanceSlot = 125, IrradianceTextureSlot = 126, IrradianceSamplerSlot = 15,
                          IrradianceConstantSlot = 13;
inline constexpr float IrradianceNormalEpsilon = .0001f;
struct IrradianceProgramInfo { unsigned queries=0, position=~0u, normal=~0u, evaluations=0; };
namespace IrradianceDxbc {
using Words=std::vector<uint32_t>;
inline uint32_t Float(float x){uint32_t u;std::memcpy(&u,&x,4);return u;}
inline Words Dest(unsigned r,unsigned mask=7){return {0x00100002u|(mask<<4),r};}
inline Words Temp(unsigned r,unsigned swizzle=0xe4){return {0x00100006u|(swizzle<<4),r};}
inline Words Input(unsigned r,unsigned swizzle=0xe4){return {0x00101006u|(swizzle<<4),r};}
inline Words Cb(unsigned slot,unsigned row,unsigned swizzle=0xe4){return {0x00208006u|(swizzle<<4),slot,row};}
inline Words Literal(float x,float y,float z,float w=0){return {0x00004002u,Float(x),Float(y),Float(z),Float(w)};}
inline void Emit(Words& out,unsigned op,std::initializer_list<Words> operands){
    const size_t start=out.size();out.push_back(op);
    for(const auto& v:operands)out.insert(out.end(),v.begin(),v.end());
    out[start]|=uint32_t(out.size()-start)<<24;
}
inline void SampleVolume(Words& out,unsigned destination,unsigned coordinates,unsigned slot=IrradianceTextureSlot,unsigned mask=7){
    // Match compiled SM5 sampling, including the explicit resource dimension,
    // float return types and scalar LOD operand.
    Emit(out,0x80000048u,{{0x80000142u,0x00155543u},Dest(destination,mask),Temp(coordinates,0x24),
        {0x00107e46u,slot},{0x00106000u,IrradianceSamplerSlot},{0x00004001u,0}});
}
struct Operand {size_t start=0,end=0;unsigned type=0,index=~0u;};
inline bool ReadOperand(const Words& code,size_t& cursor,size_t end,Operand& o,unsigned depth=0){
    if(cursor>=end||depth>4)return false;
    o.start=cursor;const auto token=code[cursor++];o.type=(token>>12)&255;
    uint32_t extended=token;
    while(extended&0x80000000u){if(cursor>=end)return false;extended=code[cursor++];}
    if(o.type==4||o.type==5){const unsigned components=(token&3)==1?1:(token&3)==2?4:0;
        if(!components)return false;cursor+=components*(o.type==5?2:1);
    }else{
        const unsigned dims=(token>>20)&3;
        for(unsigned i=0;i<dims;++i){
            const unsigned rep=(token>>(22+3*i))&7;
            if(rep==0||rep==1||rep==3||rep==4){if(cursor>=end)return false;
                if(i==0&&rep==0)o.index=code[cursor];cursor+=rep==1||rep==4?2:1;}
            if(rep==2||rep==3||rep==4){Operand nested;if(!ReadOperand(code,cursor,end,nested,depth+1))return false;}
            else if(rep>4)return false;
        }
    }
    o.end=cursor;return cursor<=end;
}
inline void SimpleQuery(Words& out,unsigned first,const Words& position,const Words& normal,const Words& destination,unsigned resultSwizzle){
    // cb13: world->unit volume scale/bias, texel-centre clamps. Six ambient-cube
    // bands are stacked along W, matching the native probe representation.
    const unsigned uv=first,n=first+1,weights=first+2,sign=first+3,
                   positive=first+4,negative=first+5,sum=first+6,coords=first+7;
    Emit(out,0,{Dest(uv),position,Cb(4,35)});
    Emit(out,50,{Dest(uv),Temp(uv),Cb(13,0),Cb(13,1)});
    Emit(out,52,{Dest(uv),Temp(uv),Cb(13,2)});
    Emit(out,51,{Dest(uv),Temp(uv),Cb(13,3)});
    Emit(out,16,{Dest(n,8),normal,normal});
    Emit(out,52,{Dest(n,8),Temp(n,255),Literal(IrradianceNormalEpsilon,IrradianceNormalEpsilon,IrradianceNormalEpsilon,IrradianceNormalEpsilon)});
    Emit(out,68,{Dest(n,8),Temp(n,255)});
    Emit(out,56,{Dest(n),normal,Temp(n,255)});
    Emit(out,56,{Dest(weights),Temp(n),Temp(n)});
    Emit(out,29,{Dest(sign),Temp(n),Literal(0,0,0)});
    Emit(out,54,{Dest(sum,15),Literal(0,0,0,1)});
    for(unsigned axis=0;axis<3;++axis){
        const unsigned scalar=axis*0x55;
        Emit(out,56,{Dest(coords),Temp(uv),Literal(1,1,1.f/6)});
        Emit(out,0,{Dest(coords,4),Temp(coords,170),Literal(float(axis)/6,float(axis)/6,float(axis)/6)});
        SampleVolume(out,positive,coords,IrradianceWorldSlot);
        Emit(out,0,{Dest(coords,4),Temp(coords,170),Literal(.5f,.5f,.5f)});
        SampleVolume(out,negative,coords,IrradianceWorldSlot);
        Emit(out,55,{Dest(positive),Temp(sign,scalar),Temp(positive),Temp(negative)});
        Emit(out,50,{Dest(sum),Temp(positive),Temp(weights,scalar),Temp(sum)});
    }
    Emit(out,54,{destination,Temp(sum,resultSwizzle)});
}
inline void WorldQuery(Words& out,unsigned first,const Words& position,const Words& normal,const Words& destination,unsigned resultSwizzle){
    const unsigned uv=first,n=first+1,weights=first+2,sign=first+3,sum=first+4,
        coords=first+5,base=first+6,fraction=first+7,cage=first+8,delta=first+9,
        distance=first+10,moments=first+11,weight=first+12,total=first+13,value=first+14,color=first+15;
    Emit(out,0x4001f,{{0x0020803au,13,3}}); // distance-aware DDGI binding present
    Emit(out,0,{Dest(uv),position,Cb(4,35)});
    Emit(out,50,{Dest(uv),Temp(uv),Cb(13,0),Cb(13,1)});
    Emit(out,52,{Dest(uv),Temp(uv),Cb(13,2)});Emit(out,51,{Dest(uv),Temp(uv),Cb(13,3)});
    Emit(out,16,{Dest(n,8),normal,normal});Emit(out,52,{Dest(n,8),Temp(n,255),Literal(.0001f,.0001f,.0001f,.0001f)});
    Emit(out,68,{Dest(n,8),Temp(n,255)});Emit(out,56,{Dest(n),normal,Temp(n,255)});
    Emit(out,56,{Dest(weights),Temp(n),Temp(n)});Emit(out,29,{Dest(sign),Temp(n),Literal(0,0,0)});
    // Surface bias in the normal direction, in probe cells, keeps the cage on its receiver's side.
    Emit(out,50,{Dest(uv),Temp(n),Literal(.35f/64,.35f/64,.35f/64),Temp(uv)});
    Emit(out,50,{Dest(base),Temp(uv),Literal(64,64,64),Literal(0,0,0)});
    Emit(out,26,{Dest(fraction),Temp(base)});
    Words negativeFraction=Temp(fraction);negativeFraction[0]|=0x80000000u;negativeFraction.insert(negativeFraction.begin()+1,0x41u);
    Emit(out,0,{Dest(base),Temp(base),negativeFraction});
    // base is the nearest probe index; fraction is the centred receiver offset.
    Emit(out,0,{Dest(fraction),Temp(fraction),Literal(-.5f,-.5f,-.5f)});
    // total.x: visibility-weighted sum; total.y: kernel weight of the cage's VALID probes.
    Emit(out,54,{Dest(sum,15),Literal(0,0,0,1)});Emit(out,54,{Dest(total,3),Literal(0,0,0)});
    // Keep the wider kernel in one loop body. Unrolling 27 full visibility queries
    // makes native shader compilation disproportionately expensive on map reload.
    auto offsets=[&](){
        Emit(out,56,{Dest(coords,4),Temp(total,170),Literal(1.f/9,1.f/9,1.f/9)});
        Emit(out,65,{Dest(coords,4),Temp(coords,170)});
        Emit(out,56,{Dest(coords,2),Temp(total,170),Literal(1.f/3,1.f/3,1.f/3)});
        Emit(out,65,{Dest(coords,2),Temp(coords,85)});
        Emit(out,50,{Dest(coords,1),Temp(coords,85),Literal(-3,-3,-3),Temp(total,170)});
        Emit(out,50,{Dest(coords,2),Temp(coords,170),Literal(-3,-3,-3),Temp(coords,85)});
        Emit(out,0,{Dest(coords),Temp(coords),Literal(-1,-1,-1)});
    };
    Emit(out,54,{Dest(total,4),Literal(0,0,0)});
    Emit(out,48,{}); // loop
    Emit(out,29,{Dest(total,8),Temp(total,170),Literal(27,27,27,27)});
    Emit(out,0x40003,{Temp(total,255)}); // breakc nonzero
    {
        offsets();
        Emit(out,0,{Dest(cage),Temp(base),Temp(coords)});
        Emit(out,0,{Dest(cage),Temp(cage),Literal(.5f,.5f,.5f)});
        Emit(out,56,{Dest(cage),Temp(cage),Literal(1.f/64,1.f/64,1.f/64)});
        Emit(out,52,{Dest(cage),Temp(cage),Cb(13,2)});Emit(out,51,{Dest(cage),Temp(cage),Cb(13,3)});
        Words negativeCage=Temp(cage);negativeCage[0]|=0x80000000u;negativeCage.insert(negativeCage.begin()+1,0x41u);
        Emit(out,0,{Dest(delta),Temp(uv),negativeCage});Emit(out,56,{Dest(delta),Temp(delta),Literal(64,64,64)});
        // All bands share an origin. Decode it before choosing the receiver's
        // direction band; relocation can change that direction and its sign.
        Emit(out,56,{Dest(coords),Temp(cage),Literal(1,1,1.f/6)});
        SampleVolume(out,moments,coords,IrradianceDistanceSlot,8);
        Words negCode=Temp(moments,255);negCode[0]|=0x80000000u;negCode.insert(negCode.begin()+1,0x41u);
        Emit(out,52,{Dest(value,8),Temp(moments,255),negCode});
        Emit(out,26,{Dest(distance,2),Temp(value,255)});
        Words negMagnitude=Temp(distance,85);negMagnitude[0]|=0x80000000u;negMagnitude.insert(negMagnitude.begin()+1,0x41u);
        Emit(out,29,{Dest(weight,8),Temp(moments,255),Literal(0,0,0)});
        Emit(out,55,{Dest(distance,2),Temp(weight,255),Temp(distance,85),negMagnitude});
        Emit(out,65,{Dest(value,8),Temp(value,255)});
        Emit(out,24,{Dest(value),Temp(value,255),Literal(1,2,3)});
        Emit(out,55,{Dest(value),Temp(value),Temp(distance,85),Literal(0,0,0)});
        Words negOffset=Temp(value);negOffset[0]|=0x80000000u;negOffset.insert(negOffset.begin()+1,0x41u);
        Emit(out,0,{Dest(delta),Temp(delta),negOffset});
        Emit(out,16,{Dest(distance,1),Temp(delta),Temp(delta)});
        Emit(out,52,{Dest(distance,1),Temp(distance,0),Literal(.0001f,.0001f,.0001f)});
        Emit(out,68,{Dest(distance,2),Temp(distance,0)});Emit(out,56,{Dest(distance,1),Temp(distance,0),Temp(distance,85)});
        // Blend directional visibility continuously. Choosing a single dominant
        // band creates diagonal seams wherever abs(delta.x) crosses abs(delta.y).
        Emit(out,16,{Dest(distance,2),Temp(delta),Temp(delta)});
        Emit(out,29,{Dest(coords,8),Temp(distance,85),Literal(1e-8f,1e-8f,1e-8f,1e-8f)});
        Emit(out,52,{Dest(distance,2),Temp(distance,85),Literal(1e-8f,1e-8f,1e-8f)});
        Emit(out,56,{Dest(value),Temp(delta),Temp(delta)});
        Emit(out,14,{Dest(value),Temp(value),Temp(distance,85)});
        Emit(out,55,{Dest(value),Temp(coords,255),Temp(value),Literal(1.f/3,1.f/3,1.f/3)});
        Emit(out,29,{Dest(weight),Temp(delta),Literal(0,0,0)});
        Emit(out,54,{Dest(weight,8),Literal(0,0,0)});
        Emit(out,54,{Dest(color,8),Literal(0,0,0)}); // color.w: this probe has any valid band
        for(unsigned axis=0;axis<3;++axis){const unsigned scalar=axis*0x55;
            Emit(out,56,{Dest(coords),Temp(cage),Literal(1,1,1.f/6)});
            Emit(out,55,{Dest(coords,8),Temp(weight,scalar),Literal(float(axis)/6,float(axis)/6,float(axis)/6,float(axis)/6),Literal(float(axis)/6+.5f,float(axis)/6+.5f,float(axis)/6+.5f,float(axis)/6+.5f)});
            Emit(out,0,{Dest(coords,4),Temp(coords,170),Temp(coords,255)});
            SampleVolume(out,moments,coords,IrradianceDistanceSlot,15);
            Emit(out,52,{Dest(color,8),Temp(color,255),Temp(moments,170)});
            Emit(out,56,{Dest(color,1),Temp(moments,0),Temp(moments,0)});
            Words negSquare=Temp(color,0);negSquare[0]|=0x80000000u;negSquare.insert(negSquare.begin()+1,0x41u);
            Emit(out,0,{Dest(color,1),Temp(moments,85),negSquare});
            Emit(out,52,{Dest(color,1),Temp(color,0),Literal(.0001f,.0001f,.0001f)});
            Words negMean=Temp(moments,0);negMean[0]|=0x80000000u;negMean.insert(negMean.begin()+1,0x41u);
            Emit(out,0,{Dest(color,2),Temp(distance,0),negMean});
            Emit(out,52,{Dest(color,2),Temp(color,85),Literal(0,0,0)});
            Emit(out,50,{Dest(color,2),Temp(color,85),Temp(color,85),Temp(color,0)});
            Emit(out,14,{Dest(color,2),Temp(color,0),Temp(color,85)});
            Emit(out,56,{Dest(color,4),Temp(color,85),Temp(color,85)});
            Emit(out,56,{Dest(color,2),Temp(color,85),Temp(color,170)});
            Emit(out,56,{Dest(color,2),Temp(color,85),Temp(moments,170)});
            Emit(out,50,{Dest(weight,8),Temp(color,85),Temp(value,scalar),Temp(weight,255)});
        }
        // Wicked-style wrapped surface-facing weight. A back-facing probe must
        // not dominate a receiver merely because its broad distance band is open.
        Emit(out,16,{Dest(color,1),Temp(delta),Temp(n)});
        Emit(out,14,{Dest(color,1),Temp(color,0),Temp(distance,0)});
        Emit(out,50,{Dest(color,1),Temp(color,0),Literal(-.5f,-.5f,-.5f),Literal(.5f,.5f,.5f)});
        Emit(out,52,{Dest(color,1),Temp(color,0),Literal(.0001f,.0001f,.0001f)});
        Emit(out,50,{Dest(color,1),Temp(color,0),Temp(color,0),Literal(.2f,.2f,.2f)});
        Emit(out,56,{Dest(weight,1),Temp(weight,255),Temp(color,0)});
        // Match the separable quadratic kernel used by ScreenGiShader.
        offsets();
        Emit(out,50,{Dest(value),Temp(fraction),Temp(coords),Literal(.5f,.5f,.5f)});
        Emit(out,56,{Dest(value),Temp(value),Temp(value)});
        Emit(out,56,{Dest(value),Temp(value),Literal(.5f,.5f,.5f)});
        Emit(out,50,{Dest(moments),Temp(fraction),Temp(fraction),Literal(0,0,0)});
        Words negSquared=Temp(moments);negSquared[0]|=0x80000000u;negSquared.insert(negSquared.begin()+1,0x41u);
        Emit(out,0,{Dest(moments),negSquared,Literal(.75f,.75f,.75f)});
        Emit(out,24,{Dest(coords),Temp(coords),Literal(0,0,0)});
        Emit(out,55,{Dest(value),Temp(coords),Temp(moments),Temp(value)});
        Emit(out,56,{Dest(weight,1),Temp(weight,0),Temp(value,0)});Emit(out,56,{Dest(weight,1),Temp(weight,0),Temp(value,85)});Emit(out,56,{Dest(weight,1),Temp(weight,0),Temp(value,170)});
        // An invalid probe (no open origin, or a band that sees back faces) must hand its
        // kernel share to the other corners rather than darken the receiver: a receiver
        // standing at such a probe's position drew a black line fading into a wedge.
        Emit(out,56,{Dest(distance,8),Temp(value,0),Temp(value,85)});Emit(out,56,{Dest(distance,8),Temp(distance,255),Temp(value,170)});
        Emit(out,56,{Dest(distance,8),Temp(distance,255),Temp(color,255)});
        Emit(out,0,{Dest(total,2),Temp(total,85),Temp(distance,255)});
        Emit(out,54,{Dest(color),Literal(0,0,0)});
        for(unsigned axis=0;axis<3;++axis){const unsigned scalar=axis*0x55;
            Emit(out,56,{Dest(coords),Temp(cage),Literal(1,1,1.f/6)});
            Emit(out,55,{Dest(coords,8),Temp(sign,scalar),Literal(float(axis)/6,float(axis)/6,float(axis)/6,float(axis)/6),Literal(float(axis)/6+.5f,float(axis)/6+.5f,float(axis)/6+.5f,float(axis)/6+.5f)});
            Emit(out,0,{Dest(coords,4),Temp(coords,170),Temp(coords,255)});SampleVolume(out,value,coords,IrradianceWorldSlot);
            Emit(out,50,{Dest(color),Temp(value),Temp(weights,scalar),Temp(color)});
        }
        Emit(out,50,{Dest(sum),Temp(color),Temp(weight,0),Temp(sum)});Emit(out,0,{Dest(total,1),Temp(total,0),Temp(weight,0)});
        Emit(out,0,{Dest(total,4),Temp(total,170),Literal(1,1,1)});
    }
    Emit(out,22,{}); // endloop
    // Continuous confidence below the denominator floor: zero visibility
    // remains black, but crossing the floor never switches irradiance on/off.
    // Confidence is visibility relative to the valid probes' kernel weight, so
    // a cage made mostly of invalid probes is judged by the ones that remain.
    Emit(out,52,{Dest(total,2),Temp(total,85),Literal(.0001f,.0001f,.0001f)});
    Emit(out,14,{Dest(weight,1),Temp(total,0),Temp(total,85)});
    Emit(out,56,{Dest(weight,1),Temp(weight,0),Literal(10000,10000,10000)});
    Emit(out,51,{Dest(weight,1),Temp(weight,0),Literal(1,1,1)});
    Emit(out,52,{Dest(total,1),Temp(total,0),Literal(.0001f,.0001f,.0001f)});
    Emit(out,14,{Dest(sum),Temp(sum),Temp(total,0)});
    Emit(out,56,{Dest(sum),Temp(sum),Temp(weight,0)});
    Emit(out,54,{destination,Temp(sum,resultSwizzle)});
    Emit(out,18,{});SimpleQuery(out,first,position,normal,destination,resultSwizzle);Emit(out,21,{});
}
// Four depth-compatible half-resolution neighbours. Keep native normal maps by
// caching all six ambient-cube bands rather than one already-shaded RGB value.
inline void Query(Words& out,unsigned first,const Words& position,const Words& normal,const Words& destination,unsigned resultSwizzle){
    const unsigned p=first,n=first+1,angular=first+2,sign=first+3,sum=first+4,
        coords=first+5,base=first+6,fraction=first+7,test=first+8,color=first+9,
        clip=first+10,sample=first+11,weight=first+12,total=first+13,value=first+14,gradient=first+15;
    Emit(out,29,{Dest(test,1),Cb(13,3,255),Literal(2,2,2)});
    Emit(out,0x4001f,{Temp(test,0)});
    Emit(out,0,{Dest(p),position,Cb(4,35)});Emit(out,54,{Dest(p,8),Literal(1,1,1,1)});
    for(unsigned row=0;row<4;++row)Emit(out,17,{Dest(clip,1u<<row),Temp(p),Cb(13,4+row)});
    // Reciprocal view depth is affine across a projected triangle. Predict the
    // depth at each half-resolution sample on this receiver's plane instead of
    // rejecting its own sloped floor/wall and falling back to coarse probes.
    Emit(out,52,{Dest(gradient,4),Temp(clip,255),Literal(.0001f,.0001f,.0001f)});
    Emit(out,14,{Dest(gradient,4),Literal(1,1,1),Temp(gradient,170)});
    Emit(out,11,{Dest(gradient,1),Temp(gradient,170)});Emit(out,12,{Dest(gradient,2),Temp(gradient,170)});
    Emit(out,14,{Dest(base,3),Temp(clip),Temp(clip,255)});
    Emit(out,50,{Dest(base,3),Temp(base),Literal(.5f,-.5f,0),Literal(.5f,.5f,0)});
    Emit(out,29,{Dest(test,3),Temp(base),Literal(0,0,0)});
    Emit(out,29,{Dest(value,3),Literal(1,1,1),Temp(base)});
    Emit(out,1,{Dest(test,3),Temp(test),Temp(value)});
    Emit(out,1,{Dest(sign,8),Temp(test,0),Temp(test,85)});
    Emit(out,29,{Dest(test,1),Temp(clip,255),Literal(.0001f,.0001f,.0001f)});
    Emit(out,1,{Dest(sign,8),Temp(sign,255),Temp(test,0)});
    Emit(out,50,{Dest(base,3),Temp(base),Cb(13,9),Literal(-.5f,-.5f,0)});
    Emit(out,26,{Dest(fraction,3),Temp(base)});
    Words negFraction=Temp(fraction);negFraction[0]|=0x80000000u;negFraction.insert(negFraction.begin()+1,0x41u);
    Emit(out,0,{Dest(base,3),Temp(base),negFraction});
    Emit(out,0,{Dest(p,3),Temp(base),Temp(fraction)});
    Emit(out,0,{Dest(p,3),Temp(p),Literal(.5f,.5f,0)});Emit(out,56,{Dest(p,3),Temp(p),Cb(13,9,0xee)});
    Emit(out,16,{Dest(n,8),normal,normal});Emit(out,52,{Dest(n,8),Temp(n,255),Literal(.0001f,.0001f,.0001f,.0001f)});
    Emit(out,68,{Dest(n,8),Temp(n,255)});Emit(out,56,{Dest(n),normal,Temp(n,255)});
    Emit(out,56,{Dest(angular),Temp(n),Temp(n)});Emit(out,29,{Dest(sign),Temp(n),Literal(0,0,0)});
    Emit(out,54,{Dest(sum,15),Literal(0,0,0,1)});Emit(out,54,{Dest(total,1),Literal(0,0,0)});
    Emit(out,0,{Dest(value,15),Temp(base,0),Literal(.5f,1.5f,.5f,1.5f)});
    Emit(out,52,{Dest(value,15),Temp(value),Literal(.5f,.5f,.5f,.5f)});
    Emit(out,0,{Dest(color,15),Cb(13,9,0),Literal(-.5f,-.5f,-.5f,-.5f)});
    Emit(out,51,{Dest(value,15),Temp(value),Temp(color)});
    Emit(out,56,{Dest(value,15),Temp(value),Cb(13,9,170)});Emit(out,65,{Dest(value,15),Temp(value)});
    Emit(out,0,{Dest(value,15),Temp(value),Literal(.5f,.5f,.5f,.5f)});
    Words negX=Temp(p,0);negX[0]|=0x80000000u;negX.insert(negX.begin()+1,0x41u);
    Emit(out,0,{Dest(value,15),Temp(value),negX});
    Emit(out,50,{Dest(value,15),Temp(value),Temp(gradient,0),Temp(gradient,170)});
    Emit(out,0,{Dest(color,15),Temp(base,85),Literal(.5f,.5f,1.5f,1.5f)});
    Emit(out,52,{Dest(color,15),Temp(color),Literal(.5f,.5f,.5f,.5f)});
    Emit(out,0,{Dest(coords,15),Cb(13,9,85),Literal(-.5f,-.5f,-.5f,-.5f)});
    Emit(out,51,{Dest(color,15),Temp(color),Temp(coords)});
    Emit(out,56,{Dest(color,15),Temp(color),Cb(13,9,255)});Emit(out,65,{Dest(color,15),Temp(color)});
    Emit(out,0,{Dest(color,15),Temp(color),Literal(.5f,.5f,.5f,.5f)});
    Words negY=Temp(p,85);negY[0]|=0x80000000u;negY.insert(negY.begin()+1,0x41u);
    Emit(out,0,{Dest(color,15),Temp(color),negY});
    Emit(out,50,{Dest(gradient,15),Temp(color),Temp(gradient,85),Temp(value)});
    for(unsigned corner=0;corner<4;++corner){const float x=float(corner&1),y=float((corner>>1)&1);
        Emit(out,0,{Dest(coords,3),Temp(base),Literal(x+.5f,y+.5f,0)});
        Emit(out,29,{Dest(test,2),Temp(gradient,corner*0x55),Literal(1e-8f,1e-8f,1e-8f)});
        Emit(out,52,{Dest(weight,2),Temp(gradient,corner*0x55),Literal(1e-8f,1e-8f,1e-8f)});
        Emit(out,14,{Dest(weight,2),Literal(1,1,1),Temp(weight,85)});
        Emit(out,14,{Dest(coords,3),Temp(coords),Cb(13,9)});
        Emit(out,54,{Dest(color),Literal(0,0,0)});
        for(unsigned axis=0;axis<3;++axis){const unsigned scalar=axis*0x55;
            Emit(out,55,{Dest(coords,4),Temp(sign,scalar),Literal((axis+.5f)/6,(axis+.5f)/6,(axis+.5f)/6),Literal((axis+3.5f)/6,(axis+3.5f)/6,(axis+3.5f)/6)});
            SampleVolume(out,sample,coords,IrradianceTextureSlot,15);
            Emit(out,50,{Dest(color),Temp(sample),Temp(angular,scalar),Temp(color)});
        }
        Words negDepth=Temp(weight,85);negDepth[0]|=0x80000000u;negDepth.insert(negDepth.begin()+1,0x41u);
        Emit(out,0,{Dest(value,1),Temp(sample,255),negDepth});
        Words negDifference=Temp(value,0);negDifference[0]|=0x80000000u;negDifference.insert(negDifference.begin()+1,0x41u);
        Emit(out,52,{Dest(value,1),Temp(value,0),negDifference});
        Emit(out,50,{Dest(value,2),Temp(weight,85),Literal(.001f,.001f,.001f),Literal(2,2,2)});
        Emit(out,14,{Dest(value,1),Temp(value,0),Temp(value,85)});
        Words negRelative=Temp(value,0);negRelative[0]|=0x80000000u;negRelative.insert(negRelative.begin()+1,0x41u);
        Emit(out,0,{Dest(weight,1),Literal(1,1,1),negRelative});
        Emit(out,52,{Dest(weight,1),Temp(weight,0),Literal(0,0,0)});
        Emit(out,29,{Dest(value,1),Temp(sample,255),Literal(.0001f,.0001f,.0001f)});
        Emit(out,1,{Dest(value,1),Temp(value,0),Temp(sign,255)});
        Emit(out,1,{Dest(value,1),Temp(value,0),Temp(test,85)});
        Emit(out,55,{Dest(weight,1),Temp(value,0),Temp(weight,0),Literal(0,0,0)});
        Emit(out,50,{Dest(value,3),Temp(fraction),Literal(x?1.f:-1.f,y?1.f:-1.f,0),Literal(x?0.f:1.f,y?0.f:1.f,0)});
        Emit(out,56,{Dest(weight,1),Temp(weight,0),Temp(value,0)});Emit(out,56,{Dest(weight,1),Temp(weight,0),Temp(value,85)});
        Emit(out,50,{Dest(sum),Temp(color),Temp(weight,0),Temp(sum)});
        Emit(out,0,{Dest(total,1),Temp(total,0),Temp(weight,0)});
    }
    Emit(out,18,{});Emit(out,54,{Dest(total,1),Literal(0,0,0)});Emit(out,21,{});
    Emit(out,29,{Dest(test,1),Temp(total,0),Literal(.0001f,.0001f,.0001f)});
    Emit(out,0x4001f,{Temp(test,0)});
    Emit(out,14,{Dest(sum),Temp(sum),Temp(total,0)});Emit(out,54,{destination,Temp(sum,resultSwizzle)});
    Emit(out,18,{});WorldQuery(out,first,position,normal,destination,resultSwizzle);Emit(out,21,{});
}

} // namespace IrradianceDxbc

// Version-coupled transformation, validated against the shipped programs and D3D11.
// Unknown signatures, resource collisions and unfamiliar query forms fail closed.
inline bool DynamicIrradianceProgram(const void* data,size_t size,std::vector<uint8_t>& output,
                                     IrradianceProgramInfo* details=nullptr,unsigned probeSlot=0){
    using namespace IrradianceDxbc;
    output.clear();if(details)*details={};
    if(!data||size<32||size>16*1024*1024)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t n){uint32_t u;std::memcpy(&u,p+n,4);return u;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=size)return false;
    const unsigned chunks=word(28);if(chunks>(size-32)/4)return false;
    unsigned pos=~0u,normal=~0u;size_t program=0,programSize=0;
    for(unsigned i=0;i<chunks;++i){
        const size_t start=word(32+i*4);if(start<32+chunks*4||start>size-8)return false;
        const size_t bytes=word(start+4);if(bytes>size-start-8)return false;
        if(!std::memcmp(p+start,"SHEX",4)||!std::memcmp(p+start,"SHDR",4)){
            if(program)return false;program=start;programSize=bytes;
        }
        if(!std::memcmp(p+start,"ISGN",4)){
            if(bytes<8)return false;const unsigned parameters=word(start+8);
            if(parameters>(bytes-8)/24)return false;
            for(unsigned j=0;j<parameters;++j){const size_t record=start+16+j*24;
                const unsigned name=word(record),semantic=word(record+4),reg=word(record+16);
                if(name>=bytes||!std::memchr(p+start+8+name,0,bytes-name))return false;
                if(word(record+8)!=0||word(record+12)!=3||(p[record+20]&7)!=7)continue;
                if(!std::strcmp(reinterpret_cast<const char*>(p+start+8+name),"TEXCOORD")){
                    if(semantic==0)pos=reg;if(semantic==1)normal=reg;
                }
            }
        }
    }
    if(!program||programSize<8||programSize%4||word(program+8)!=0x50||word(program+12)!=programSize/4)return false;
    Words code(programSize/4);std::memcpy(code.data(),p+program+8,programSize);
    struct Instruction {size_t start,length;};std::vector<Instruction> instructions;
    unsigned temps=0;bool cb4=false,positionDeclared=false,normalDeclared=false;
    size_t lastConstant=0,lastSampler=0,lastResource=0;
    for(size_t i=2;i<code.size();){
        const auto token=code[i],op=token&2047;size_t length=(token>>24)&127;
        if(op==53){if(i+1>=code.size())return false;length=code[i+1];}
        if(!length||length>code.size()-i)return false;
        instructions.push_back({i,length});
        if(op==89)lastConstant=i;
        if(op==90)lastSampler=i;
        if(op==88||op==161||op==162)lastResource=i;
        if(op==104){if(length!=2||temps)return false;temps=code[i+1];}
        if(op==89&&length==4){if(code[i+2]==13)return false;if(code[i+2]==4&&code[i+3]>=36)cb4=true;}
        if(op==88&&length>=4&&(code[i+2]==IrradianceTextureSlot||code[i+2]==IrradianceDistanceSlot||code[i+2]==IrradianceWorldSlot))return false;
        if(op==90&&length>=3&&code[i+2]==15)return false;
        if((op==98||op==99||op==100)&&length>=3){if(code[i+2]==pos)positionDeclared=true;if(code[i+2]==normal)normalDeclared=true;}
        i+=length;
    }
    // Projected decals reconstruct their surface from depth rather than interpolating
    // position/normal. Derive those operands from the camera rebase and native sun DP3.
    Words reconstructedNormal;size_t normalUse=0,positionCapture=0;Words reconstructedPosition;
    if(!positionDeclared||!normalDeclared){
        for(const auto& ins:instructions){const unsigned op=code[ins.start]&2047;
            if(op!=0&&op!=16)continue;size_t cursor=ins.start+1;std::vector<Operand> operands;
            while(cursor<ins.start+ins.length){Operand o;if(!ReadOperand(code,cursor,ins.start+ins.length,o))return false;operands.push_back(o);}
            if(operands.size()!=3)continue;
            for(unsigned source=1;source<3;++source){const auto& cb=operands[source];
                if(cb.type!=8||cb.index==~0u)continue;
                const bool extended=(code[cb.start]&0x80000000u)!=0;
                const size_t row=cb.start+(extended?3:2);if(row>=cb.end)continue;
                if(op==16&&cb.index==3&&code[row]==1945&&operands[3-source].type==0&&!normalUse){
                    const auto& n=operands[3-source];reconstructedNormal.assign(code.begin()+n.start,code.begin()+n.end);normalUse=ins.start;
                }
                if(op==0&&cb.index==4&&code[row]==35&&extended&&code[cb.start+1]==0x41&&operands[0].type==0&&
                   (code[operands[0].start]&0xff)==0x72){
                    positionCapture=ins.start;reconstructedPosition=Temp(operands[0].index);
                }
            }
        }
        if(!normalUse||!positionCapture||reconstructedNormal.size()!=2)return false;
    }
    if(!temps||temps>4078||!cb4||!lastConstant||!lastSampler||!lastResource)return false;
    size_t declarationsEnd=2,firstQuery=0,lastQuery=0;unsigned nativeQueries=0;
    for(const auto& ins:instructions){const unsigned op=code[ins.start]&2047;
        if(op==53||(op>=88&&op<=106)||(op>=143&&op<=162))declarationsEnd=ins.start+ins.length;
        if(op!=69&&op!=72&&op!=73&&op!=74)continue;
        size_t cursor=ins.start+1;uint32_t ext=code[ins.start];
        while(ext&0x80000000u){if(cursor>=ins.start+ins.length)return false;ext=code[cursor++];}
        Operand dest,coords,resource;
        if(!ReadOperand(code,cursor,ins.start+ins.length,dest)||
           !ReadOperand(code,cursor,ins.start+ins.length,coords)||
           !ReadOperand(code,cursor,ins.start+ins.length,resource))return false;
        if(resource.type==7&&(resource.index==2||(probeSlot&&resource.index==probeSlot))){
            if(!nativeQueries)firstQuery=ins.start;lastQuery=ins.start;++nativeQueries;
        }
    }
    // Interpolated position/normal and the GI bindings are invariant throughout a
    // pixel invocation. Evaluate once before native control flow so a skipped
    // first sample, a later branch or a loop cannot read an uninitialised cache.
    const bool sharedQuery=reconstructedNormal.empty()&&nativeQueries>1;
    // Reconstructed programs read one position snapshot and one proven normal at
    // every site, so those sites agree too. Their inputs only exist from the first
    // site onwards: evaluate there, and reuse the result at the later sites. Loops
    // and subroutines could change the inputs between sites, so they keep one
    // query per site. Branches between sites guard the cache with a flag instead.
    auto controlFlow=[&](size_t from,size_t to,std::initializer_list<unsigned> ops){
        for(const auto& ins:instructions)if(ins.start>from&&ins.start<to)
            for(unsigned op:ops)if((code[ins.start]&2047)==op)return true;
        return false;
    };
    const bool anchoredQuery=!reconstructedNormal.empty()&&nativeQueries>1&&temps<=4077&&
        !controlFlow(declarationsEnd-1,normalUse,{2,3,4,5,7,8,22,44,48});
    const bool guardedQuery=anchoredQuery&&controlFlow(firstQuery,lastQuery,{2,3,4,5,6,7,8,10,18,21,22,23,31,44,48,62,63,76});
    const unsigned cached=temps+17,valid=temps+18;
    Words patched{code[0],0};unsigned queries=0;
    for(const auto& ins:instructions){const auto token=code[ins.start],op=token&2047;
        if(sharedQuery&&ins.start==declarationsEnd)
            Query(patched,temps,Input(pos),Input(normal),Dest(cached,15),0xe4);
        if(guardedQuery&&ins.start==declarationsEnd)Emit(patched,54,{Dest(valid,1),Literal(0,0,0,0)});
        if(op==104){
            patched.insert(patched.end(),{0x02000068u,temps+(guardedQuery?19u:sharedQuery||anchoredQuery?18u:17u)});continue;
        }
        // SAMPLE/SAMPLE_L/SAMPLE_D/SAMPLE_B: preserve native result masks and
        // reject unexpected swizzles rather than silently changing material math.
        if(op==69||op==72||op==73||op==74){
            size_t cursor=ins.start+1;uint32_t ext=token;
            while(ext&0x80000000u){if(cursor>=ins.start+ins.length)return false;ext=code[cursor++];}
            std::vector<Operand> operands;
            while(cursor<ins.start+ins.length){Operand o;if(!ReadOperand(code,cursor,ins.start+ins.length,o))return false;operands.push_back(o);}
            if(operands.size()<4)return false;
            const auto& resource=operands[2];const auto& dest=operands[0];
            if(resource.type==7&&(resource.index==2||resource.index==3||(probeSlot&&resource.index==probeSlot))){
                if(dest.type!=0||dest.end-dest.start!=2||dest.index>=temps||
                   (code[resource.start]&15)!=6)return false;
                const Words destination{code[dest.start],code[dest.start+1]};
                if(resource.index==2||(probeSlot&&resource.index==probeSlot)){
                    if(!reconstructedNormal.empty()){
                        if(ins.start<=positionCapture||ins.start>=normalUse)return false;
                        // A later overwrite would mean that DP3 describes a different
                        // normal than the one live at the irradiance query.
                        for(const auto& later:instructions)if(later.start>ins.start&&later.start<normalUse){
                            size_t c=later.start+1;uint32_t ext=code[later.start];while(ext&0x80000000u)ext=code[c++];
                            if(c+1<later.start+later.length&&(code[c]&0xfffff00f)==0x00100002u&&
                               (code[c]&0x70)!=0&&code[c+1]==reconstructedNormal[1])return false;
                        }
                    }
                    if(anchoredQuery&&(guardedQuery||ins.start==firstQuery)){
                        if(guardedQuery)Emit(patched,0x1f,{Temp(valid,0)});
                        Query(patched,temps,Temp(temps+16),reconstructedNormal,Dest(cached,15),0xe4);
                        if(guardedQuery){Emit(patched,54,{Dest(valid,1),Literal(1,1,1,1)});Emit(patched,21,{});}
                    }
                    if(sharedQuery||anchoredQuery)Emit(patched,54,{destination,Temp(cached,(code[resource.start]>>4)&255)});
                    else Query(patched,temps,reconstructedNormal.empty()?Input(pos):Temp(temps+16),
                        reconstructedNormal.empty()?Input(normal):reconstructedNormal,destination,(code[resource.start]>>4)&255);
                    ++queries;
                }else{
                    const float neutral[4]{.5f,.5f,1,1};const unsigned swizzle=(code[resource.start]>>4)&255;
                    Emit(patched,54,{destination,Literal(neutral[swizzle&3],neutral[(swizzle>>2)&3],neutral[(swizzle>>4)&3],neutral[(swizzle>>6)&3])});
                }
                continue;
            }
        }
        patched.insert(patched.end(),code.begin()+ins.start,code.begin()+ins.start+ins.length);
        // Keep declaration groups in the order emitted by the SM5 compiler.
        if(ins.start==lastConstant)patched.insert(patched.end(),{0x04000059u,0x00208e46u,IrradianceConstantSlot,10});
        if(ins.start==lastSampler)patched.insert(patched.end(),{0x0300005au,0x00106000u,IrradianceSamplerSlot});
        if(ins.start==lastResource)patched.insert(patched.end(),{0x04002858u,0x00107000u,IrradianceTextureSlot,0x00005555u,0x04002858u,0x00107000u,IrradianceDistanceSlot,0x00005555u,0x04002858u,0x00107000u,IrradianceWorldSlot,0x00005555u});
        if(ins.start==positionCapture)Emit(patched,54,{Dest(temps+16),reconstructedPosition});
    }
    if(!queries)return false;patched[1]=unsigned(patched.size());
    output.assign(p,p+32+chunks*4);
    auto put=[&](size_t n,uint32_t u){std::memcpy(output.data()+n,&u,4);};
    for(unsigned i=0;i<chunks;++i){const size_t start=word(32+i*4),bytes=word(start+4),destination=output.size();
        put(32+i*4,unsigned(destination));
        if(start==program){output.insert(output.end(),p+start,p+start+8);output.resize(output.size()+patched.size()*4);
            std::memcpy(output.data()+destination+8,patched.data(),patched.size()*4);put(destination+4,unsigned(patched.size()*4));}
        else output.insert(output.end(),p+start,p+start+8+bytes);
    }
    put(24,unsigned(output.size()));
    Environment::DxbcHash::ComputeHashRetail(output.data()+20,unsigned(output.size()-20),output.data()+4);
    if(details)*details={queries,pos,normal,sharedQuery||anchoredQuery?1u:queries};return true;
}
} // namespace Filmmaker::Relight
