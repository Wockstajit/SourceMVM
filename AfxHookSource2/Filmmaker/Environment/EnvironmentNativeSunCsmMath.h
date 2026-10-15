#pragma once
#include "EnvironmentShadowMath.h"
#include "../Relight/RelightMath.h"
#include <array>
namespace Filmmaker::Environment {
inline constexpr unsigned OwnedSunBufferBytes=1992*16;
inline constexpr std::array<unsigned,4> OwnedSunTiles{
    Relight::SunCloseTile,Relight::SunDetailTile,Relight::SunMiddleTile,Relight::SunTile};
// The native shader multiplies position components by consecutive CB rows
// (matrix columns). Translation must be rebased with the actual native view,
// not the application's estimate of the eye position.
inline std::array<float,16> NativeSunColumns(const Matrix& matrix,Vec3 view) {
    std::array<float,16> columns{};
    for(unsigned axis=0;axis<4;++axis)for(unsigned row=0;row<4;++row)
        columns[axis*4+row]=matrix[row*4+axis];
    for(unsigned row=0;row<4;++row)
        columns[12+row]+=matrix[row*4]*view.x+matrix[row*4+1]*view.y+matrix[row*4+2]*view.z;
    return columns;
}
// The native comparison shares a normalized bias with local lights. Cancel its
// excess in the sun-only sampling transform, keeping the depth atlas and lamps
// unchanged. The caller selects the tolerated fraction of a shadow texel.
inline Matrix NativeSunReceiverMatrix(Matrix m,float bias,float resolution,float toleranceTexels=.05f) {
    const float lateral=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
    const float depth=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
    if(!(lateral>0)||!(depth>0)||!(resolution>0))return m;
    const float bounded=(std::max)(1e-7f,(std::min)(bias,2*depth/(lateral*resolution)*toleranceTexels));
    m[11]+=bias-bounded;
    return m;
}
// Moves the receiver `units` world units toward the sun for one sampling transform: casters closer
// than that along the sun ray no longer shade it. Used for player bodies so they never self-shadow.
inline Matrix NativeSunTowardSun(Matrix m,float units) {
    const float depth=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
    if(units>0&&depth>0)m[11]-=units*depth;
    return m;
}
// The three camera-following crops of the native sun (the fourth tile is the whole map).
// Camera-centred squares spend half their texels behind the camera, and the old 64/183/524
// ladder dropped everything past ~500 units onto the whole-map tile (several units per texel),
// which is the visible quality cliff and pop down a long street. The largest crop now spans
// the user's shadowRange; the finest keeps first-person detail; detail is their geometric mean.
struct NativeSunCrops {float close,detail,middle;};
inline constexpr float NativeSunCloseHalfExtent=128.f;
// Fraction of each crop's half extent it is pushed ahead of the camera along the view.
inline constexpr float NativeSunLead=.5f;
inline NativeSunCrops NativeSunCropExtents(float shadowRange,float mapRadius) {
    const float close=NativeSunCloseHalfExtent;
    float middle=shadowRange>0?shadowRange*.5f:1500.f;
    if(mapRadius>0)middle=(std::min)(middle,mapRadius);
    middle=(std::max)(middle,close*4);
    return {close,std::sqrt(close*middle),middle};
}
// Engine view angles (pitch down positive, yaw) to the unit view direction.
inline Vec3 ViewForward(Vec3 angles) {
    const float pitch=Radians(angles.x),yaw=Radians(angles.y);
    return {std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)};
}
// Crop centred ahead of the camera: the camera stays inside with margin (at NativeSunLead of
// the half extent from centre, well before the blend band) while the region the camera looks
// into gets the rest. The lead point is snapped by NearSunShadowMatrix in whole texels.
// Depth interval of a crop: only the receivers inside its footprint (the map's height range
// under the crop square), padded. Everything nearer the sun is pancaked onto the near plane
// by the sun raster state (depth clip off) and still shadows. The whole-map interval was
// several thousand units along the sun, so the native depth bias came to over a world unit:
// the floor next to every sun-facing-away wall base read as lit, a thin bright line at the
// contact. A crop's receivers span a few hundred units, so the same bias shrinks with it.
inline constexpr float NativeSunDepthPadding=32;
inline Matrix TightenSunCropDepth(Matrix m,float zLow,float zHigh,float padding=NativeSunDepthPadding) {
    const Vec3 u{m[0],m[1],m[2]},v{m[4],m[5],m[6]},wRow{m[8],m[9],m[10]};
    const float lu=std::sqrt(Dot(u,u)),lv=std::sqrt(Dot(v,v)),lw=std::sqrt(Dot(wRow,wRow));
    if(!(lu>0)||!(lv>0)||!(lw>0)||!(zHigh>=zLow))return m;
    const Vec3 U=u*(1/lu),V=v*(1/lv),W=wRow*(1/lw);
    // A sun at the horizon: every height maps across the whole depth range; keep the map's.
    if(std::abs(W.z)<.05f)return m;
    // Crop square in light space: clip x = lu*dot(U,p) + m[3] spans -1..1.
    const float cu=-m[3]/lu,cv=-m[7]/lv,hu=1/lu,hv=1/lv;
    float dLow=1e30f,dHigh=-1e30f;
    for(int corner=0;corner<8;++corner){
        const float a=cu+(corner&1?hu:-hu),b=cv+(corner&2?hv:-hv),z=corner&4?zHigh:zLow;
        // World point a*U + b*V + d*W at height z.
        const float d=(z-a*U.z-b*V.z)/W.z;
        dLow=(std::min)(dLow,d);dHigh=(std::max)(dHigh,d);
    }
    dLow-=padding;dHigh+=padding;
    // Never wider than the whole-map interval this crop was cut from.
    const float wholeLow=-m[11]/lw,wholeHigh=(1-m[11])/lw;
    dLow=(std::max)(dLow,wholeLow);dHigh=(std::min)(dHigh,wholeHigh);
    if(!(dHigh>dLow))return m;
    const float scale=1/(dHigh-dLow);
    m[8]=W.x*scale;m[9]=W.y*scale;m[10]=W.z*scale;m[11]=-dLow*scale;
    return m;
}
inline Matrix NativeSunCropMatrix(const Matrix& wholeMap,Vec3 camera,Vec3 forward,float halfExtent,float resolution,
                                  float zLow=1,float zHigh=0) {
    auto m=NearSunShadowMatrix(wholeMap,camera+forward*(halfExtent*NativeSunLead),halfExtent,resolution);
    return zHigh>=zLow?TightenSunCropDepth(m,zLow,zHigh):m;
}
// Distant sky billboards must not reduce village trees to subpixel casters.
// Retain the full sky's upstream depth before tightening only receiver heights.
inline Matrix NativeSkyboxSunMatrix(const Matrix& wholeSky,Vec3 camera,Vec3 origin,float scale,
                                   float mapLow,float mapHigh,float resolution) {
    if(!(scale>=1)||!std::isfinite(scale))return wholeSky;
    auto crop=NearSunShadowMatrix(wholeSky,origin+camera*(1/scale),512.f,resolution);
    return TightenSunCropDepth(crop,origin.z+mapLow/scale-64,origin.z+mapHigh/scale+128,2);
}
inline constexpr std::array<float,4> NativeSkyboxSunRect{.5f,.5f,.5f,-.5f};
inline std::array<float,16> NativeSunRects() {
    std::array<float,16> rects{};
    for(unsigned i=0;i<4;++i)Relight::TileRect(OwnedSunTiles[i],rects.data()+i*4);
    return rects;
}
}
