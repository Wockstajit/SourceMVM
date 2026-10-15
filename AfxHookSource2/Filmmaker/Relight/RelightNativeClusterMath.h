#pragma once
#include "RelightClusterMath.h"
#include <cstddef>
#include <cstring>

namespace Filmmaker::Relight {
inline bool NativeClusterDeclarations(const void* data,size_t size,unsigned kind){
    if(!data||size<32||kind<1||kind>2)return false;
    const auto* p=static_cast<const uint8_t*>(data);
    auto word=[&](size_t at){uint32_t value;memcpy(&value,p+at,4);return value;};
    if(word(0)!=0x43425844u||word(24)!=size)return false;
    const uint32_t chunks=word(28);if(chunks>(size-32)/4)return false;
    unsigned programs=0;bool items=false,planes=kind==1,output=false,constants=false;
    for(uint32_t chunk=0;chunk<chunks;++chunk){const size_t offset=word(32+chunk*4);
        if(offset<32+size_t(chunks)*4||offset>size-8||word(offset+4)>size-offset-8)return false;
        if(word(offset)!=0x52444853u&&word(offset)!=0x58454853u)continue;
        const size_t bytes=word(offset+4),start=offset+8;
        if(++programs!=1||bytes<8||bytes%4||word(start)!=0x50050u||word(start+4)!=bytes/4)return false;
        for(size_t at=2;at<bytes/4;){const uint32_t token=word(start+at*4),op=token&2047;size_t length=(token>>24)&127;
            if(op==53){if(at+1>=bytes/4)return false;length=word(start+(at+1)*4);}
            if(!length||length>bytes/4-at)return false;
            if(length==4){const auto operand=word(start+(at+1)*4),slot=word(start+(at+2)*4),value=word(start+(at+3)*4);
                if(op==162&&operand==0x00107000u){
                    if(slot==(kind==1?0u:1u)){if(value!=80||items)return false;items=true;}
                    else if(kind==2&&slot==0){if(value!=16||planes)return false;planes=true;}
                    else return false;
                }
                if(op==158){if(operand!=0x0011e000u||slot!=0||value!=4||output)return false;output=true;}
                if(op==89){if(operand!=0x00208e46u||slot!=0||value!=(kind==1?10u:7u)||constants)return false;constants=true;}
            }
            at+=length;
        }
    }
    return programs==1&&items&&planes&&output&&constants;
}
// CS2's CullParams_t: three light/probe/reflection batches, with independent
// tile and depth outputs. The native kernels were verified in an engine capture.
struct NativeClusterParams {
    uint32_t tiles=0,tilesX=0,tilesY=0;float tileEpsilon=0;
    std::array<float,4> tileToCenter{1,1,.5f,.5f};
    uint32_t depthBins=0;float binWidth=1,binEpsilon=0,nearPlane=0;
    ClusterUintRow firstMasks{};
    std::array<ClusterUintRow,3> tileBatches{},binBatches{};
};
static_assert(sizeof(NativeClusterParams)==160);
static_assert(offsetof(NativeClusterParams,firstMasks)==48);
static_assert(offsetof(NativeClusterParams,tileBatches)==64);
static_assert(offsetof(NativeClusterParams,binBatches)==112);
struct NativeCullItem {
    ClusterUintRow planes{}; // start, unused, first plane count, second plane count
    ClusterFloatRow rectangle{},interval{},conic0{},conic1{};
};
static_assert(sizeof(NativeCullItem)==80);
inline NativeClusterParams MakeNativeClusterParams(const ClusterGrid& grid) {
    NativeClusterParams p;p.tiles=grid.TileCount();p.tilesX=grid.tilesX;p.tilesY=grid.tilesY;p.depthBins=grid.sliceCount;
    p.firstMasks={0,grid.lights.words,grid.lights.words,grid.lights.words};
    p.tileBatches[0]={grid.lights.tiles,grid.lights.words,0,grid.lights.count};
    p.binBatches[0]={grid.lights.slices,grid.lights.words,0,grid.lights.count};
    return p;
}
inline std::array<uint32_t,4> NativeClusterDispatches(const ClusterGrid& grid) {
    return {(grid.sliceCount+31)/32,grid.lights.words,(grid.tilesX+7)/8,(grid.tilesY+3)/4};
}
inline NativeCullItem NativeItemFromExtent(ClusterExtent e,const ClusterGrid& grid,uint32_t index) {
    NativeCullItem item;item.planes={index*4,0,4,0};
    if(e.allTiles)e={0,grid.tilesX-1,0,grid.tilesY-1,e.z0,e.z1,1,0};
    // Integer tile coordinates avoid cancellation at viewport edges. Quarter
    // margins distinguish the included centres from their adjacent tile/bin.
    item.rectangle={float(e.x0)+.25f,float(e.y0)+.25f,float(e.x1)+.75f,float(e.y1)+.75f};
    item.interval={float(e.z0)+.25f,float(e.z1)+.75f,0,0};
    return item;
}
inline std::array<ClusterFloatRow,4> NativeItemPlanes(const NativeCullItem& item) {
    const auto& r=item.rectangle;
    return {{{1,0,-r[0],0},{-1,0,r[2],0},{0,1,-r[1],0},{0,-1,r[3],0}}};
}
}
