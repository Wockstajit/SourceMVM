#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
namespace Filmmaker::Environment {
// Source 2 assigns separate depth intervals to the world, viewmodel and 3D
// skybox. The sky backdrop itself is 1..1 and has no material illumination.
inline bool NativeLightingViewport(float width,float height,float minimum,float maximum) {
    if(!std::isfinite(width)||!std::isfinite(height)||width<256||height<256)return false;
    auto equal=[](float a,float b){return std::abs(a-b)<1e-6f;};
    return (equal(minimum,0)&&(equal(maximum,.95f)||equal(maximum,.1f)))||
           (equal(minimum,.95f)&&equal(maximum,1));
}
// Observed CS2 forward-material ABI, verified from the shipped weapon/cable PS.
// These offsets are accepted only with the matching declaration and native sun
// operand signatures. Unknown layouts fail closed, never patch an arbitrary b3.
inline constexpr unsigned NativeSunBufferBytes=1990*16;
inline constexpr unsigned NativeSunDirectionOffset=1945*16;
inline constexpr unsigned NativeSunRadianceOffset=1946*16;
inline constexpr unsigned NativeSunCascadesOffset=1953*16;
// Shipped clustered-light loop bounds its mask words with cb3[19].z.
inline constexpr unsigned NativeFixtureWordsOffset=19*16+8;
// Derived from the shipped bytecode, not from a table of known program hashes:
// lightSlot is the register of the single 480-byte structured light buffer,
// comparisonSampler the single mode_comparison sampler the shadow tap uses, and
// lightmapArrayMask a bit per baked Texture2DArray input among t1..t3, so a
// partial declaration never binds a replacement over a differently typed slot.
// A program declaring more than one of either is ambiguous and leaves them zero,
// so callers fail closed exactly as an unrecognised program used to.
struct NativeSunAbiInfo {
    uint32_t flags=0,cb3Count=0,lightSlot=0,comparisonSampler=0,lightmapArrayMask=0;
    uint32_t probeSlot=0,probeOcclusionSlot=0;
    // A bit per declared reflection slot, t0-t31. Baked cubemaps are a third carrier of the
    // map's compiled lighting: each is a 360 capture taken under the original lights, so a
    // glossy surface keeps reflecting the map as it was lit long after the lightmap is gone.
    // Cube and cube-array are tracked apart because a neutral replacement has to match the
    // declared dimension or the bind is invalid.
    uint32_t cubeSlotMask=0,cubeArraySlotMask=0;
    // The projected-cookie volume the light loop samples for a record with radiance.w > 0: the one
    // texture3d besides the probe pair. hasCookie is false when that is ambiguous.
    uint32_t cookieSlot=0;bool hasCookie=false;
    uint64_t programHash=0;
    bool ownedCascades=false;
    // Texture slot of the engine's screen-space shadow sample, taken over by the owned sun
    // visibility pass (NativeAtmosphereProgram); 0 when the program keeps the native filter.
    uint32_t screenShadowSlot=0;
};
// Which baked inputs an unbaked replay replaces, from the arrays a program declares. Bit n of
// lightmapArrayMask is slot t(n+1). t1 carries the baked direct term and goes black; t2 carries
// the baked irradiance and takes the flat fill; t3 is geometric bent-normal data, not radiance,
// and keeps whatever the engine bound. A slot the program types differently is never touched.
struct LightmapNeutralization {
    uint32_t blackSlots=0,fillSlots=0; // bit s = texture slot s
    bool Any()const{return blackSlots||fillSlots;}
};
inline LightmapNeutralization NeutralizeLightmapArrays(uint32_t lightmapArrayMask){
    LightmapNeutralization n;
    if(lightmapArrayMask&1)n.blackSlots|=1u<<1;
    if(lightmapArrayMask&2)n.fillSlots|=1u<<2;
    return n;
}
inline bool HasNativeSunMaterialAbi(const void* code,size_t bytes,NativeSunAbiInfo* info=nullptr){
    if(info)*info={};
    if(!code||bytes<32||bytes>16*1024*1024)return false;
    auto p=static_cast<const unsigned char*>(code);
    auto word=[&](size_t n){uint32_t w;std::memcpy(&w,p+n,4);return w;};
    if(std::memcmp(p,"DXBC",4)||word(24)!=bytes)return false;
    auto chunks=word(28);if(chunks>(bytes-32)/4)return false;
    bool program=false,declaration=false,direction=false,radiance=false,fixtureBuffer=false,fixtureLoop=false;
    unsigned lightBuffers=0,comparisonSamplers=0;uint32_t lightmapArrayMask=0;
    uint32_t lightSlot=0,comparisonSampler=0;
    uint32_t cubeSlotMask=0,cubeArraySlotMask=0;
    bool probeBuffer=false,texture3d[128]{};
    for(uint32_t j=0;j<chunks;++j){size_t start=word(32+j*4);
        if(start<32+size_t(chunks)*4||start>bytes-8)return false;
        const size_t size=word(start+4);if(size>bytes-start-8)return false;
        if(std::memcmp(p+start,"SHEX",4)&&std::memcmp(p+start,"SHDR",4))continue;
        if(program||size<8||size%4)return false;program=true;start+=8;
        if(word(start)!=0x50||word(start+4)!=size/4)return false;
        if(info){info->flags|=1;info->programHash=14695981039346656037ull;
            for(size_t b=0;b<size;++b){info->programHash^=p[start+b];info->programHash*=1099511628211ull;}}
        for(size_t i=2;i<size/4;){
            const uint32_t token=word(start+i*4),op=token&0x7ff;size_t count=(token>>24)&127;
            if(op==53){if(i+1>=size/4)return false;count=word(start+(i+1)*4);if(count<2)return false;}
            if(!count||count>size/4-i)return false;
            // Record discard for diagnostics; coverage instrumentation retains the native test.
            if(op==13&&info)info->flags|=16;
            auto t=[&](size_t n){return word(start+(i+n)*4);};
            if(op==89&&count==4&&t(1)==0x00208e46&&t(2)==3){
                if(info)info->cb3Count=t(3);
                if(token==0x04000859&&t(3)==1990)declaration=true;}
            if(op==89&&count==4&&t(1)==0x00208e46&&t(2)==2&&token==0x04000859&&t(3)==3600)
                probeBuffer=true;
            if(op==88&&count>=4&&t(1)==0x00107000&&((token>>11)&0x1f)==5&&t(2)<128)
                texture3d[t(2)]=true;
            // dcl_resource_texturecube / texturecubearray. Same opcode and dimension field as
            // the arrays above; 6 is TEXTURECUBE and 10 is TEXTURECUBEARRAY.
            if(op==88&&count>=4&&t(1)==0x00107000&&t(2)<32){
                const auto dimension=(token>>11)&0x1f;
                if(dimension==6)cubeSlotMask|=1u<<t(2);
                else if(dimension==10)cubeArraySlotMask|=1u<<t(2);
            }
            // DP3 rN.component, cb3[1945].xyz, rM.<any three>, either operand order. The temp
            // swizzle follows register allocation (observed .xyz and .yzw); the constant row
            // and its .xyz components are what identify the sun direction.
            if(token==0x08000010&&(t(1)&~0xf0u)==0x00100002){
                const auto temp=[](uint32_t operand){return (operand&~0xff0u)==0x00100006u;};
                direction|=(t(3)==0x00208246&&t(4)==3&&t(5)==1945&&temp(t(6)))||
                    (temp(t(3))&&t(5)==0x00208246&&t(6)==3&&t(7)==1945);}
            // MUL rN.<three lanes>, rM.scalar, cb3[1946].xyz (native sun visibility * radiance).
            // The destination mask follows register allocation (.xyz, .xzw, ...); the constant
            // swizzle must then feed those lanes x, y, z in order.
            const auto threeLanes=[](uint32_t operand,uint32_t constant){
                const unsigned mask=(operand>>4)&15;
                if((operand&~0xf0u)!=0x00100002u||(constant&~0xff0u)!=0x00208006u)return false;
                unsigned next=0;
                for(unsigned lane=0;lane<4;++lane)if(mask&(1u<<lane)){
                    if(((constant>>(4+lane*2))&3)!=next)return false;++next;}
                return next==3;};
            if(token==0x08000038&&threeLanes(t(1),t(5))&&t(6)==3&&t(7)==1946){
                const auto operand=t(3);radiance|=operand==0x00100006||operand==0x00100556||operand==0x00100aa6||operand==0x00100ff6;}
            // Require both the native 480-byte fixture records and the exact
            // loop / unsigned bound / conditional break sequence. VRF identifies this
            // as clustered direct lighting; actual DXBC establishes the binding/offset.
            if(token==0x040000a2&&t(1)==0x00107000&&t(2)<128&&t(3)==480){
                fixtureBuffer=true;lightSlot=t(2);++lightBuffers;}
            // dcl_sampler sN, mode_comparison. Sampler mode is bits 11-13 of the
            // opcode token; 1 is comparison. The shadow tap uses the only one.
            if(op==90&&count>=3&&t(1)==0x00106000&&((token>>11)&7)==1&&t(2)<128){
                comparisonSampler=t(2);++comparisonSamplers;}
            // dcl_resource_texture2darray tN. Resource dimension is bits 11-15 of
            // the opcode token; 8 is TEXTURE2DARRAY. Baked lighting arrives at t1-t3.
            if(op==88&&count>=4&&t(1)==0x00107000&&((token>>11)&0x1f)==8&&t(2)>=1&&t(2)<=3)
                lightmapArrayMask|=1u<<(t(2)-1);
            if(token==0x08000050&&t(5)==0x0020802a&&t(6)==3&&t(7)==19&&i>2&&
               word(start+(i-1)*4)==0x01000030&&i+count+3<=size/4&&
               word(start+(i+count)*4)==0x03040003&&
               (t(1)&~0xf0u)==0x00100002&&word(start+(i+count+2)*4)==t(2))fixtureLoop=true;
            i+=count;
        }
    }
    if(info){
        info->flags|=(declaration?2:0)|(direction?4:0)|(radiance?8:0)|((fixtureBuffer&&fixtureLoop)?32:0)|(probeBuffer?64:0); // 64: declares the CB2 probe volume table
        info->lightmapArrayMask=lightmapArrayMask;
        info->cubeSlotMask=cubeSlotMask;info->cubeArraySlotMask=cubeArraySlotMask;
        if(lightBuffers==1&&comparisonSamplers==1){
            info->lightSlot=lightSlot;info->comparisonSampler=comparisonSampler;
            if(declaration&&probeBuffer&&lightSlot>=2&&texture3d[lightSlot-2]&&texture3d[lightSlot-1]) {
                info->probeSlot=lightSlot-2;info->probeOcclusionSlot=lightSlot-1;
            }else if(declaration&&probeBuffer&&lightSlot>=1&&texture3d[lightSlot-1]){
                // First-person weapon/glove programs (d47bf4fa23a33fe2): one probe volume directly
                // below the light records and no sun-occlusion volume. Missing this left the
                // viewmodel without any ambient or bounce: black wherever direct light missed it.
                info->probeSlot=lightSlot-1;info->probeOcclusionSlot=0;
            }
            unsigned volumes=0;uint32_t slot=0;
            for(uint32_t n=0;n<128;++n)
                if(texture3d[n]&&!(info->probeSlot&&(n==info->probeSlot||n==info->probeOcclusionSlot))){++volumes;slot=n;}
            if(volumes==1){info->cookieSlot=slot;info->hasCookie=true;}
        }
    }
    return program&&declaration&&direction&&radiance;
}
}
