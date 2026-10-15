#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace Filmmaker::Environment {
// Shipped post_process D_COLORBUFFER_TONEMAPSPACE decode: rgb /= 1-max(rgb).
// Match the complete scalar/vector data flow; never infer encoding from texture format.
inline bool NativePostUsesCompressedHdr(const void* code,size_t bytes) {
    if(!code||bytes<32||bytes>16*1024*1024)return false;
    const auto* p=static_cast<const unsigned char*>(code);
    auto w=[&](size_t n){uint32_t v;std::memcpy(&v,p+n*4,4);return v;};
    if(std::memcmp(p,"DXBC",4)||w(6)!=bytes||w(7)>(bytes-32)/4)return false;
    for(unsigned chunk=0;chunk<w(7);++chunk){
        const size_t at=w(8+chunk);
        if(at<32+w(7)*4||at>bytes-8||at%4)return false;
        const size_t size=w(at/4+1);
        if(size>bytes-at-8)return false;
        if(std::memcmp(p+at,"SHEX",4)&&std::memcmp(p+at,"SHDR",4))continue;
        if(size<8||size%4||w(at/4+2)!=0x50||w(at/4+3)!=size/4)return false;
        const size_t begin=at/4+2,end=begin+size/4;
        for(size_t i=begin+2;i<end;){
            const auto token=w(i),op=token&0x7ff;size_t n=(token>>24)&127;
            if(op==53){if(i+1>=end)return false;n=w(i+1);if(n<2)return false;}
            if(!n||n>end-i)return false;
            if(i+34<=end&&token==0x07000034&&w(i+7)==0x07000034&&
               w(i+14)==0x08000000&&w(i+22)==0x05000081&&w(i+27)==0x07000038){
                const auto dst=w(i+1),reg=w(i+2),rgb=w(i+4);
                const auto scalar=w(i+10);
                const auto tempScalar=[](uint32_t t){return (t&~0x30u)==0x0010000a;};
                const unsigned mask=(dst>>4)&15;
                unsigned component=0;while(component<4&&mask!=(1u<<component))++component;
                const uint32_t select=0x0010000a|(component<<4);
                const unsigned channels=(1u<<((w(i+3)>>4)&3))|(1u<<((w(i+5)>>4)&3))|(1u<<((scalar>>4)&3));
                if(channels==7&&component<4&&dst==(0x00100002|(mask<<4))&&
                   tempScalar(w(i+3))&&tempScalar(w(i+5))&&w(i+6)==rgb&&
                   w(i+8)==dst&&w(i+9)==reg&&tempScalar(scalar)&&w(i+11)==rgb&&
                   w(i+12)==select&&w(i+13)==reg&&
                   w(i+15)==dst&&w(i+16)==reg&&w(i+17)==(select|0x80000000)&&
                   w(i+18)==0x41&&w(i+19)==reg&&w(i+20)==0x00004001&&w(i+21)==0x3f800000&&
                   w(i+23)==dst&&w(i+24)==reg&&w(i+25)==select&&w(i+26)==reg&&
                   (w(i+28)==0x00100072||w(i+28)==0x00102072)&&w(i+30)==0x00100246&&w(i+31)==rgb&&
                   w(i+32)==(0x00100006|((component*0x55)<<4))&&w(i+33)==reg)
                    return true;
            }
            i+=n;
        }
    }
    return false;
}
}
