#pragma once
namespace Filmmaker::Relight {
inline constexpr char ClusterShader[]=R"HLSL(
struct Light {float4 rows[30];};
struct Extent {uint4 xy;uint4 z;};
StructuredBuffer<Light> lights:register(t0);
StructuredBuffer<Extent> extents:register(t1);
RWStructuredBuffer<Extent> outputExtent:register(u0);
RWStructuredBuffer<uint> masks:register(u1);
RWStructuredBuffer<uint4> constants:register(u2);
cbuffer Params:register(b0) {
    uint4 grid; // shift, tilesX, tilesY, slices
    uint4 list; // recordCount, words, tileBase, sliceBase
    float4 depth; // scale, maxSlice, viewport width, viewport height
    uint4 control; // mode, ones base, require world-position projection, occupancy mirror offset
    float4 nearby; // first-person eye (world), reach
    uint4 nearbyList; // first-person list base (0: use the full list)
    float4 reach; // x: radiance cutoff for visible reach (0: authored ranges)
}
cbuffer NativeViewport:register(b1) {float4 viewport[21];}
cbuffer NativeView:register(b4) {float4 view[36];}
float3 Project(float3 p) {
    return p.x*view[12].xyw+p.y*view[13].xyw+p.z*view[14].xyw+view[15].xyw;
}
// Mirrors RelightClusterMath.h LightVisibleReach: where the light, at its brightest, falls to
// the cutoff. Never beyond the authored radius.
float VisibleReach(Light light,float radius) {
    if(!(reach.x>0)||!(radius>0)||!isfinite(radius)||!(light.rows[8].w>0))return radius;
    float peak=max(light.rows[10].x,max(light.rows[10].y,light.rows[10].z));
    float soft=light.rows[8].w;
    if(!(peak>0)||!isfinite(peak*soft))return radius;
    float low=0,high=radius;
    [loop]for(uint i=0;i<24;++i){
        float d=(low+high)*.5;
        float t=saturate(light.rows[9].x+light.rows[9].y*d);
        float c=peak*soft/max(soft,d*d)*(t*t*(3-2*t));
        if(c>reach.x)low=d;else high=d;
    }
    return high;
}
// Clip the convex hull against positive W before projecting. All vertex pairs
// include every true edge, so additional diagonal intersections only enlarge it.
void TileBounds(float3 hull[8],uint count,inout Extent e) {
    float3 clips[8];float2 lo=depth.zw,hi=0;bool visible=false;
    const float nearW=1e-4;
    [loop]for(uint i=0;i<count;++i){
        clips[i]=Project(hull[i]);if(!all(isfinite(clips[i])))return;
        if(clips[i].z>=nearW){
            float2 p=clamp((float2(clips[i].x,-clips[i].y)/clips[i].z+1)*depth.zw*.5,0,depth.zw-1);
            lo=min(lo,p);hi=max(hi,p);visible=true;
        }
    }
    [unroll]for(uint pair=0;pair<8;++pair){
      [unroll]for(uint j=0;j<8;++j){
        if(pair>=count||j>=count||j<=pair)continue;
        float3 a=clips[pair],b=clips[j];if((a.z>=nearW)==(b.z>=nearW))continue;
        float t=(nearW-a.z)/(b.z-a.z);float2 q=a.xy+(b.xy-a.xy)*t;
        float2 p=clamp((float2(q.x,-q.y)/nearW+1)*depth.zw*.5,0,depth.zw-1);
        lo=min(lo,p);hi=max(hi,p);visible=true;
    }
    }
    if(!visible){e.xy=uint4(grid.y,0,0,0);e.z.z=0;return;}
    uint2 a=(uint2)lo>>grid.x,b=(uint2)hi>>grid.x;
    a=max(a,1)-1;b=min(b+1,grid.yz-1);e.xy=uint4(a.x,b.x,a.y,b.y);e.z.z=0;
}
bool GlassHull(Light light,out float3 hull[8]) {
    [unroll]for(uint i=0;i<8;++i)hull[i]=0;
    if(light.rows[8].w!=0||!(light.rows[10].w>0)||any(light.rows[3]!=float4(0,0,0,1)))return false;
    float3 x=light.rows[0].xyz,y=light.rows[1].xyz,z=light.rows[2].xyz;
    float xx=dot(x,x),yy=dot(y,y),zz=dot(z,z);
    float3 yz=cross(y,z),zx=cross(z,x),xy=cross(x,y);float determinant=dot(x,yz);
    if(!isfinite(xx+yy+zz)||min(xx,min(yy,zz))<1e-14||
        !isfinite(light.rows[0].w+light.rows[1].w+light.rows[2].w)||
        abs(determinant)<1e-4*sqrt(xx*yy*zz))return false;
    float3 ix=yz/determinant,iy=zx/determinant,iz=xy/determinant;
    [unroll]for(uint i=0;i<8;++i){
        float u=((i&1)?1:-1)*(1+sqrt(xx)),v=((i&2)?1:-1)*(1+sqrt(yy));
        float w=(i&4)?1+sqrt(zz):-sqrt(zz);
        hull[i]=ix*(u-light.rows[0].w)+iy*(v-light.rows[1].w)+iz*(w-light.rows[2].w);
        if(!all(isfinite(hull[i])))return false;
    }
    return true;
}
[numthreads(64,1,1)] void Bounds(uint3 id:SV_DispatchThreadID) {
    if(id.x>=list.x)return;
    Extent e;e.xy=uint4(0,grid.y-1,0,grid.z-1);e.z=uint4(0,grid.w-1,1,0);
    Light light=lights[id.x];
    // First-person raster coordinates use a different projection/depth interval.
    // Native direct lighting instead projects its world position with rows 12..15
    // when cb4[2].x is set. Only that branch can use these world-light bounds.
    // The separate shifted position used by native probes does not index lights.
    bool viewportSafe=all(viewport[20].xy==0)&&all(viewport[20].zw==depth.zw)&&
        (!control.z||asuint(view[2].x)!=0);
    if(id.x==0) {
        uint mode=viewportSafe?control.x:0;
        // First-person pixels that cannot use tiles read the lights near the eye (Fill).
        uint flat=control.z&&nearbyList.x?nearbyList.x:control.y;
        constants[0]=uint4(mode?list.w:flat,mode==1?list.z:flat,list.y,0);
        constants[1]=constants[2]=0;
        constants[3]=mode==1?uint4(grid.xy,0,0):uint4(31,0,0,0);
        constants[4]=mode?asuint(float4(depth.xy,0,0)):uint4(0,0,0,0);
        constants[5]=asuint(float4(view[12].w,view[13].w,view[14].w,view[15].w));
    }
    float3 beamHull[8];
    if(viewportSafe&&light.rows[20].x>0&&light.rows[8].w>0){
        float3 x=light.rows[0].xyz,y=light.rows[1].xyz,f=light.rows[3].xyz,c=light.rows[8].xyz;
        float near=sqrt(light.rows[8].w),far=near+light.rows[20].y;
        float xx=dot(x,x),yy=dot(y,y);float2 interval=float2(3.402823e38,-3.402823e38);
        [unroll]for(uint i=0;i<8;++i){float d=(i&4)?far:near;
            beamHull[i]=c+f*d+x/xx*((i&1)?d:-d)+y/yy*((i&2)?d:-d);
            float w=Project(beamHull[i]).z;interval.x=min(interval.x,w);interval.y=max(interval.y,w);}
        e.z.xy=(uint2)clamp(floor(interval*depth.x)+float2(-1,1),0,depth.y);
        TileBounds(beamHull,8,e);outputExtent[id.x]=e;return;
    }
    if(viewportSafe&&GlassHull(light,beamHull)){
        float2 interval=float2(3.402823e38,-3.402823e38);bool finite=true;
        [unroll]for(uint i=0;i<8;++i){
            float3 clip=Project(beamHull[i]);finite=finite&&all(isfinite(clip));
            interval.x=min(interval.x,clip.z);interval.y=max(interval.y,clip.z);
        }
        if(finite)e.z.xy=(uint2)clamp(floor(interval*depth.x)+float2(-1,1),0,depth.y);
        TileBounds(beamHull,8,e);
        outputExtent[id.x]=e;return;
    }
    if(viewportSafe&&light.rows[9].y<0&&all(isfinite(light.rows[9].xy))) {
        float r=-light.rows[9].x/light.rows[9].y;
        float3 c=light.rows[8].xyz;
        if(r>=0&&isfinite(r)&&all(isfinite(c))) {
            r=VisibleReach(light,r);
            float3 x=light.rows[0].xyz,y=light.rows[1].xyz,f=light.rows[3].xyz;
            float xx=dot(x,x),yy=dot(y,y),ff=dot(f,f);
            bool cone=light.rows[8].w>0&&isfinite(xx+yy+ff)&&xx>=1e-4&&yy>=1e-4&&abs(ff-1)<=1e-4&&
                abs(dot(x,y))<=1e-4*sqrt(xx*yy)&&abs(dot(x,f))<=1e-4*sqrt(xx)&&abs(dot(y,f))<=1e-4*sqrt(yy)&&
                abs(dot(x,c)+light.rows[0].w)<=.25&&abs(dot(y,c)+light.rows[1].w)<=.25&&abs(dot(f,c)+light.rows[3].w)<=.25;
            float pad=1+r*.001;uint count=cone?5:8;
            float3 hull[8];
            [unroll]for(uint vertex=0;vertex<8;++vertex){
                if(cone){uint corner=vertex-1;
                    hull[vertex]=vertex==0?c-f*pad:c+f*(r+pad)+x/xx*((corner&1)?r+3*pad:-r-3*pad)+y/yy*((corner&2)?r+3*pad:-r-3*pad);
                }else hull[vertex]=c+r*float3((vertex&1)?1:-1,(vertex&2)?1:-1,(vertex&4)?1:-1);
            }
            float w=Project(c).z;
            float reach=r*length(float3(view[12].w,view[13].w,view[14].w));
            float2 interval=float2(w-reach,w+reach);
            if(cone){interval=float2(3.402823e38,-3.402823e38);
                [unroll]for(uint vertex=0;vertex<5;++vertex){float d=Project(hull[vertex]).z;interval.x=min(interval.x,d);interval.y=max(interval.y,d);}}
            if(all(isfinite(interval))) {
                e.z.xy=(uint2)clamp(floor(interval*depth.x)+float2(-1,1),0,depth.y);
            }
            TileBounds(hull,count,e);
        }
    }
    outputExtent[id.x]=e;
}
// Mirrors RelightClusterMath.h FirstPersonLight.
bool FirstPersonLight(Light light) {
    float3 cookie[8];
    if(light.rows[20].x>0||GlassHull(light,cookie))return true;
    if(!(light.rows[9].y<0)||!all(isfinite(light.rows[9].xy)))return true;
    float radius=-light.rows[9].x/light.rows[9].y;
    if(!(radius>=0)||!isfinite(radius))return true;
    radius=VisibleReach(light,radius);
    float3 delta=light.rows[8].xyz-nearby.xyz;float distance=dot(delta,delta);
    if(!isfinite(distance))return true;
    return distance<=(radius+nearby.w)*(radius+nearby.w);
}
[numthreads(64,1,1)] void Fill(uint3 id:SV_DispatchThreadID) {
    uint tiles=grid.y*grid.z;
    if(id.x==tiles+grid.w&&nearbyList.x) {
        uint listed=0;
        for(uint word=0;word<list.y;++word) {
            uint bits=0;
            [loop]for(uint bit=0;bit<32;++bit) {
                uint record=word*32+bit;if(record>=list.x)break;
                if(FirstPersonLight(lights[record]))bits|=1u<<bit;
            }
            masks[nearbyList.x+word]=bits;
            if(bits&&word<32)listed|=1u<<word;
        }
        if(control.w)for(uint word=0;word<list.y;++word)masks[control.w+nearbyList.x+word]=listed;
        return;
    }
    if(id.x>=tiles+grid.w)return;
    bool tile=id.x<tiles;
    uint x=id.x%grid.y,y=id.x/grid.y,z=id.x-tiles;
    uint base=tile?list.z+id.x*list.y:list.w+z*list.y;
    uint occupied=0;
    for(uint word=0;word<list.y;++word) {
        uint bits=0;
        [loop]for(uint bit=0;bit<32;++bit) {
            uint record=word*32+bit;if(record>=list.x)break;
            Extent e=extents[record];
            bool inside=tile?(e.z.z!=0||(x>=e.xy.x&&x<=e.xy.y&&y>=e.xy.z&&y<=e.xy.w)):(z>=e.z.x&&z<=e.z.y);
            if(inside)bits|=1u<<bit;
        }
        masks[base+word]=bits;
        if(bits&&word<32)occupied|=1u<<word;
    }
    if(control.w)for(uint word=0;word<list.y;++word)masks[control.w+base+word]=occupied;
}
)HLSL";
}
