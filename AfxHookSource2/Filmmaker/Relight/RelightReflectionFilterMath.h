#pragma once
#include <array>
#include <cmath>
#include <algorithm>
namespace Filmmaker::Relight {
inline constexpr unsigned ReflectionFilterSamples=32;
using ReflectionFilterTap=std::array<float,4>;
// Native materials select mip = sqrt(alpha) * (mipCount - 1). A plain box mip chain
// has a much narrower angular footprint than this GGX lobe on rough materials.
inline std::array<ReflectionFilterTap,ReflectionFilterSamples> ReflectionFilterKernel(unsigned mip,unsigned mips,unsigned size){
    std::array<ReflectionFilterTap,ReflectionFilterSamples> taps{};
    const float roughness=float(mip)/float((std::max)(1u,mips-1));
    const float alpha=roughness*roughness,a2=alpha*alpha,pi=3.14159265358979323846f;
    const float texelAngle=4*pi/(6*float(size)*size);
    for(unsigned i=0;i<taps.size();++i){
        unsigned bits=i;bits=(bits<<16)|(bits>>16);
        bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
        bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
        bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
        bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
        const float u=(i+.5f)/float(taps.size()),v=float(bits)*2.3283064365386963e-10f;
        const float phi=2*pi*v,cosine=std::sqrt((1-u)/(1+(a2-1)*u));
        const float sine=std::sqrt((std::max)(0.f,1-cosine*cosine));
        const float z=2*cosine*cosine-1;
        const float d=cosine*cosine*(a2-1)+1,pdf=a2/(4*pi*d*d);
        const float lod=.5f*std::log2(1/(float(taps.size())*(std::max)(pdf,1e-8f)*texelAngle));
        taps[i]={2*cosine*sine*std::cos(phi),2*cosine*sine*std::sin(phi),z,
                 std::clamp(lod,0.f,float(mips-1))};
    }
    return taps;
}
}
