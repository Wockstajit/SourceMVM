// Included inside kShader. This is HLSL, kept separate from the scene/mesh plumbing.
#include "EnvironmentStormShaders.h"
R"HLSL(
float Hash31(float3 p) { p=frac(p*.1031); p+=dot(p,p.yzx+33.33); return frac((p.x+p.y)*p.z); }
float Noise3(float3 p) {
    float3 a=floor(p),f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(lerp(Hash31(a),Hash31(a+float3(1,0,0)),f.x),lerp(Hash31(a+float3(0,1,0)),Hash31(a+float3(1,1,0)),f.x),f.y),
        lerp(lerp(Hash31(a+float3(0,0,1)),Hash31(a+float3(1,0,1)),f.x),lerp(Hash31(a+float3(0,1,1)),Hash31(a+1),f.x),f.y),f.z);
}
float Fbm(float3 p) { return Noise3(p)*.57+Noise3(p*2.03+17)*.29+Noise3(p*4.11-9)*.14; }
float3 SunDirection() {
    float a=radians(CL_sunAzimuth),e=radians(CL_sunElevation);
    return float3(cos(e)*cos(a),cos(e)*sin(a),sin(e));
}
// The elevation ramp runs warm at the horizon and white overhead; the authored colour
// multiplies it, so 1,1,1 keeps the shipped look and anything else tints the surfaces, the
// sun disk and the sky glow together.
float3 SunTint() { return float3(CL_sunColorR,CL_sunColorG,CL_sunColorB); }
// The ambient fill is skylight plus one grey bounce, and it is the ONLY light most of the frame
// receives once the engine's own lighting is taken away. Leaving it colourless meant the sun
// colour reached the sky and the sun disk but never the light landing on the map. It carries the
// authored tint but NOT the elevation ramp or CL_sunPower, so it neither goes black at night nor
// changes a scene that has not been tinted.
float3 AmbientTint() { return SunTint(); }
float3 SunColor() { return lerp(float3(1,.36,.12),float3(1,.94,.83),saturate((CL_sunElevation+4)/30))*SunTint()*CL_sunPower*smoothstep(0,1,CL_sunElevation); }
)HLSL"
#include "EnvironmentSkyRadianceShaders.h"
R"HLSL(
float3 SkyColor(float3 ray,float sunVisibility=1) {
    float3 color=SkyRadiance(ray,SunDirection(),SunColor(),CL_sunElevation,CL_haze,CL_stars,sunVisibility);
    float3 flashDirection=normalize(stormSource.xyz-camera.xyz+float3(0,0,1e-4));
    color+=float3(.55,.7,1)*stormSource.w*.16*pow(saturate(dot(ray,flashDirection)*.5+.5),6);
    return max(color,0);
}
)HLSL"
#include "EnvironmentRainSurfaceShaders.h"
R"HLSL(
// Lighting uses persistent light-space shadow maps, including blockers outside the camera view.
// DOES THIS PIXEL BELONG TO A PLAYER? Players get no world coating (no snow, no wet film) and no
// rain impacts: the world's masks are keyed to map positions and would smear across a moving body.
// Reference implementation and tests: CoatingActorClaims in EnvironmentCoatingMath.h
// (environment-math-tests). Change both together.
//
// A point is body only if it is inside a body-sized capsule around a pawn and does not look like
// the ground (upward-facing below knee height) or like a wall or crate the player is touching
// (a vertical face turned back toward the body axis). The floor under the player sits inside a
// plain column around the pawn, which is why the test is not just a radius.
// EXACT PLAYER MASK. The native skinned player and world-weapon triangles (the same ones that cast
// shadows) are drawn from the camera into playerMask. A point belongs to a player when a texel of
// that mask (its own or a direct neighbour, so the anti-aliased silhouette is covered) holds a surface
// at the point's own position. This is what keeps snow, wet film and rain impacts off bodies, heads,
// gear, weapons and corpses whatever pose they are in; the capsule below stays as the fallback for
// the frame before the first capture. coatingActorCount.y is 1 when the mask holds this frame's players.
bool PlayerMaskClaims(float3 p) {
    if(coatingActorCount.y<.5) return false;
    float4 h=mul(viewProjection,float4(p,1));
    if(h.w<=1e-4) return false;
    float3 ndc=h.xyz/h.w;
    if(abs(ndc.x)>1||abs(ndc.y)>1) return false;
    float2 px=(ndc.xy*float2(.5,-.5)+.5)*dimensions.xy;
    float tolerance=2.5+.004*length(p-camera.xyz);
    static const int2 taps[5]={int2(0,0),int2(1,0),int2(-1,0),int2(0,1),int2(0,-1)};
    [unroll] for(int k=0;k<5;++k) {
        int2 q=clamp(int2(px)+taps[k],int2(0,0),int2(dimensions.xy)-1);
        float d=playerMask.Load(int3(q,0));
        bool empty=camera.w>.5?d<=1e-7:d>=.9999999;
        if(!empty&&length(World(float2(q)+.5,d)-p)<tolerance) return true;
    }
    return false;
}
bool CoatingActor(float3 p,float3 geometric) {
    if(PlayerMaskClaims(p)) return true;
    int count=(int)coatingActorCount.x;
    for(int i=0;i<count;++i) {
        float3 origin=coatingActors[i].xyz; float radius=coatingActors[i].w;
        if(radius<=0) continue;
        float2 offset=p.xy-origin.xy; float height=p.z-origin.z;
        float d2=dot(offset,offset);
        if(d2>radius*radius||height<-8||height>80) continue;
        float2 radial=d2>1e-4?offset*rsqrt(d2):float2(0,0);
        bool upward=geometric.z>.6;
        if(upward&&height<20) continue;
        if(!upward&&abs(geometric.z)<.35&&d2>1e-4&&dot(geometric.xy,radial)<-.15) continue;
        return true;
    }
    return false;
}
// SETTLED SNOW UNDER COVER. Snow and ash are sheltered straight overhead, and a plain comparison
// ends the cover on a hard line exactly under the edge of an arch or eave. Real snow blows in under
// cover and thins out with distance from the opening. Open sky is always fully exposed (filtering
// open ground beside tall walls turned whole courtyards grey). Under cover the exposure is a
// distance-weighted average of how much of each ring around the point is open sky: continuous in
// position, so it fades smoothly. (Taking the NEAREST open tap along 8 fixed directions quantised
// the distance and cut straight bare bands under arcades.) The fringe grows with the cover's height
// but stays short: a building above an arcade is not an opening.
static const float kSnowFringeMax=140;
float SettledSnowExposure(float3 world){
    if(shelterInfo.x<=0)return 1;
    float4 clip=mul(shelterMatrix,float4(world+shelterDirection.xyz*shelterInfo.z,1));
    float3 ndc=clip.xyz/clip.w;float2 uv=ndc.xy*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>1)||ndc.z<0)return 1;
    int resolution=(int)shelterInfo.y;
    float2 texel=uv*resolution-.5;
    float here=ShelterCompare(texel,ndc.z,resolution);
    if(here>=1)return 1;
    float texelWorld=2/max(length(shelterMatrix[0].xyz)*resolution,1e-6);
    float depthWorld=1/max(length(shelterMatrix[2].xyz),1e-9);
    float cover=shelterTexture.Load(int3(clamp((int2)floor(texel+.5),0,resolution-1),0));
    float height=max(0,ndc.z-cover)*depthWorld;
    // Uneven reach: drifts push further in at some spots than others.
    float reach=clamp(height*.7,56,kSnowFringeMax)*(.6+.8*Noise3(world*float3(.035,.035,.01)));
    // Fine per-spot jitter of the ring radii (half-unit grain, like snow): fixed radii drew the
    // rings' contours as stair steps and faint arcs.
    float jitter=.25+.5*Hash31(floor(world*2)+17.3);
    float open=0,weight=0;
    [unroll]for(int ring=1;ring<=6;++ring){
        float r=reach*(ring-jitter)/6,w=1-(ring-.5)/6;
        float ringOpen=0;
        [unroll]for(int k=0;k<12;++k){
            float a=k*.5235988+ring*.2618;
            float d=shelterTexture.Load(int3(clamp((int2)floor(texel+float2(cos(a),sin(a))*(r/texelWorld)+.5),0,resolution-1),0));
            ringOpen+=ndc.z<=d+.000001?1:0;
        }
        open+=w*w*ringOpen/12;weight+=w*w;
    }
    // Half the directions open at the edge reads as full cover there; deep inside it falls to 0.
    return max(here,saturate(1.25*open/weight));
}
float3 ApplyCoatings(float3 color,float3 p,float3 normal,float3 coatingNormal,float mapLuminance,bool viewmodel=false) {
    // The Wetness control decides how wet surfaces get. While it rains, only ground the rain
    // actually reaches is wet; picking Rain sets a starting wetness in the UI.
    bool raining=weatherParams.z==1&&weatherColor.w>0;
    [branch] if(CL_snow<=0 && CL_wet<=0 && CL_puddles<=0) return color;
    // Players get no world coating at all. Native CS2 shading already owns player, glove and weapon
    // base wetness, and a second film there both flattened skins and emphasized capsule boundaries;
    // world snow keyed to map positions would only smear across a moving body.
    if(CoatingActor(p,normal)) return color;
    float3 sun=SunDirection(),view=normalize(camera.xyz-p);
    // GLOBAL UNTIL YOU PLACE SOMETHING. Radius 0 (the default) means the coating covers the whole
    // map, so turning Snow or Wetness up does something immediately -- that is what was broken
    // before, when an unplaced sphere meant an invisible effect and nothing said so.
    //
    // Placing a surface confines it, which is the point: wetness over an entire map looks wrong in
    // the places that should be dry, and one puddle-strewn courtyard is a real shot. The sphere
    // stays put in the world -- it does NOT follow the camera, so walking away leaves it behind.
    float radius=CL_surfaceRadius;
    float local=radius>0?1-smoothstep(radius*.8,radius,length(p.xy-float2(CL_surfaceX,CL_surfaceY))):1;
    float footprint=2*length(p-camera.xyz)/max(1,dimensions.y*length(viewProjection[1].xyz));
    footprint/=max(.2,abs(dot(coatingNormal,view)));
    float noise=lerp(.5,Fbm(p*CL_maskScale),CoatingDetailFade(footprint,CL_maskScale));
    float up=smoothstep(.35,.85,coatingNormal.z);
    // OPEN SKY, looked up once and shared by snow and rain (the shelter lookup is the heavy part):
    // the same roof and gust shelter maps that stop falling flakes and drops. 1 on open ground, a
    // fraction for a short way inside a big window or wide doorway, 0 in hallways and interiors.
    const bool settling=weatherParams.z==2||weatherParams.z==3;
    float open=shelterInfo.x>0&&(raining||CL_snow>0)?
        (settling?SettledSnowExposure(p+float3(0,0,2)):PrecipitationExposure(p+float3(0,0,2))):1;
    float snow=0,drift=.5;
    // First-person hands and weapon get no snow either: the coating is keyed to map positions.
    if(!viewmodel){
        // DRIFTS. Broad patches collect more and others are swept thin, so the cover is not one
        // even sheet; where it collects it reads as thicker (relief below).
        drift=Fbm(p*CL_maskScale*.3+31);
        // Open ground is covered; drifts only decide where it lies thin or deep.
        float amount=saturate(CL_snow*lerp(1,1.7,drift));
        // Without a shelter map (nothing falling, or an unprepared map) the coating stays global.
        // Under cover the fringe fades gradually and unevenly (drift noise), never on one line.
        float shelterFade=shelterInfo.x>0?smoothstep(.02,.8,open+(drift-.5)*.3*(1-open)):1;
        snow=local*up*CoatingCoverage(amount,noise,.18)*shelterFade;
    }
    if(snow>0) {
        // A coating rounds small upward-facing facets; preserve the map's broad lighting instead
        // of painting an unshadowed, triangle-lit white surface over it.
        float3 snowNormal=normalize(lerp(coatingNormal,float3(0,0,1),snow*.55));
        {
            // Thickness: a soft height field of piled snow tilts the lighting normal, so drifts
            // show lumps and wind-swept ridges instead of a flat white decal. Fades out where the
            // pattern would be sub-pixel.
            float3 h=p*.045;
            float h0=Noise3(h),hx=Noise3(h+float3(.35,0,0)),hy=Noise3(h+float3(0,.35,0));
            float bump=(.35+.65*saturate(CL_snow))*snow*CoatingDetailFade(footprint,.045)*up;
            snowNormal=normalize(snowNormal+float3(h0-hx,h0-hy,0)*3.5*bump);
        }
        float snowLit=saturate(dot(snowNormal,sun));
        float visibility=CL_contactShadows>0?WorldSunShadowQuick(p+sun*shadowInfo.y,snowNormal,viewmodel):1;
        float3 snowAlbedo=float3(.94,.97,1);
        float inheritedShade=.03+.97*pow(saturate(mapLuminance),.28);
        float3 mappedSnow=snowAlbedo*inheritedShade;
        mappedSnow*=1+SunColor()*snowLit*visibility*CL_sunRelight;
        float3 relitSnow=snowAlbedo*(CL_ambient+SunColor()*snowLit*visibility*CL_sunRelight);
        float3 snowColor=lerp(mappedSnow,relitSnow,CL_relight);
        snowColor+=snowAlbedo*(FillLights(p,snowNormal,viewmodel)+WorldBounce(p,snowNormal));
        float grain=CoatingDetailFade(footprint,.5);
        float relief=lerp(1,.96+.04*Noise3(p*.5),grain);
        // The thin edge of a patch is packed and a little grey; the deep middle is clean white.
        relief*=lerp(.86,1.03,smoothstep(.15,.9,snow)*(.5+.5*drift));
        float sparkle=pow(Hash31(floor(p*2)),80)*.035*snowLit*CoatingDetailFade(footprint,2);
        color=lerp(color,snowColor*relief+sparkle,snow);
    }
    // Only the first-person model sits this close; the film fades in just beyond it. This used to
    // be a hard 72-unit cut, so a wall or floor right in front of the camera dried out.
    float nearFade=smoothstep(18,44,length(p-camera.xyz));
    if(nearFade<=0)return color;
    // RainExposure, from the shared lookup: an unprepared map cannot assert a surface is exposed.
    float exposure=raining&&shelterInfo.x>=0?smoothstep(0,.9,open)*saturate(weatherColor.w*2):0;
    // STANDING WATER. Puddle coverage is the shader film — native CS2 water meshes were removed
    // because rebuilding wet-ground geometry stalled every coverage edit.
    float puddle=0,damp=0;
    if(CL_puddles>0){
        float basin=Fbm(p*CL_maskScale)-(.8-.62*saturate(CL_puddles));
        // A wide shoreline: damp ground first, then the mirror film, never a cut edge.
        float heightBand=CL_surfaceRadius>0?saturate(1-abs(p.z-CL_puddleHeight)/max(CL_puddleDepth,1)):1;
        float flat=smoothstep(.95,.995,coatingNormal.z)*local*(1-snow)*heightBand;
        puddle=smoothstep(-.02,.13,basin)*flat;
        damp=smoothstep(-.16,.02,basin)*flat*.7;
        // In rain, standing water collects only where rain lands: open ground, and the wetted
        // fringe of doorways and arches -- never across a whole interior floor.
        if(raining){float collect=smoothstep(.1,.45,exposure);puddle*=collect;damp*=collect;}
    }
    float wet=max(CL_wet*local*(raining?exposure:1)*(1-snow),max(puddle,damp))*nearFade;
    puddle*=nearFade;
    if(wet<=0)return color;
    // A stable world-space response estimate for maps without authored F_WETNESS.
    // Screen derivatives of geometric normals are NOT material roughness: they outline
    // every triangle seam and stair edge as a different wet material, changing with resolution.
    // This pass cannot identify material names or rewrite CS2's underlying roughness.
    float porous=saturate(.3+noise*.35);
    float fresnel=.0204+.9796*pow(1-saturate(dot(coatingNormal,view)),5);
    porous*=1-puddle;
    color*=1-wet*porous*.16*(1-fresnel);
    color*=1-puddle*.22;
    float3 wetNormal=normalize(lerp(coatingNormal,float3(0,0,1),puddle));
    if(exposure>0&&up>0){
        float2 q=p.xy*.065;
        float2 slope=RainImpulse(q,max(length(ddx(q)),length(ddy(q))),climateClock.x);
        wetNormal=normalize(wetNormal+float3(slope*exposure*up*CL_ripple,0));
    }
    float3 reflected=reflect(-view,wetNormal);
    // One filtered sun lookup serves both the mirrored sky and the highlight: the ripple-bent normal
    // moves the receiver offset by a fraction of a unit, not across a shadow edge.
    float sunVisibility=CL_contactShadows>0?WorldSunShadowQuick(p+sun*shadowInfo.y,coatingNormal,viewmodel):1;
    float3 environment=CL_sky>0?lerp(color,SkyColor(reflected,sunVisibility),CL_sky):WetScreenReflection(p,wetNormal,reflected,color);
    environment=WorldReflectionRoughness(p,wetNormal,reflected,environment,.12+porous*.13);
    // A water film mirrors more than Fresnel alone on flat ground: pooled film on smooth stone.
    float film=max(wet*(fresnel*.8+(1-porous)*.22*up),puddle*(.3+.7*fresnel));
    color=lerp(color,environment,saturate(film));
    float3 halfDir=normalize(sun+view+float3(1e-7,0,0));
    float exponent=lerp(28,100,CoatingDetailFade(footprint,.12))*(1-porous*.4);
    color+=SunColor()*CL_sunRelight*sunVisibility*saturate(dot(wetNormal,sun))*pow(saturate(dot(wetNormal,halfDir)),exponent)*wet*.3;
    [loop]for(int l=0;l<(int)lighting.x;++l){
        float3 dir;float atten=StudioAttenuation(l,0,p,dir);
        if(atten<=0)continue;
        atten*=WorldLightShadowSample(l,p+wetNormal*shadowInfo.y,0,viewmodel);
        float3 h=normalize(view+dir);
        color+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*
            pow(saturate(dot(wetNormal,h)),exponent)*wet*.25;
    }
    return color;
}
)HLSL"
R"HLSL(
float3 Surface(float3 color,float3 p,float3 normal,float3 coatingNormal,bool viewmodel=false,float contactOcclusion=1,float3 nativeDirect=float3(-1,-1,-1),float3 nativeFixtures=0) {
    float mapLuminance=dot(max(color,0),float3(.2126,.7152,.0722));
    float3 receiverAlbedo=max(color,float3(.02,.02,.02));
    receiverAlbedo*=.55/max(.02,max(receiverAlbedo.r,max(receiverAlbedo.g,receiverAlbedo.b)));
    float3 sun=SunDirection(),view=normalize(camera.xyz-p);
    float lit=saturate(dot(normal,sun));
    // Bias toward the light, not sideways along a reconstructed/material normal.
    // The old two-unit normal offset bent shadow UVs across hands and rounded props.
    // SurfaceSunShadow supplies its own small, per-cascade texel-scale normal allowance.
    float visibility=1;
    if(CL_contactShadows>0&&CL_sunRelight>0)visibility=lerp(1,WorldSunShadowSoft(p+sun*(viewmodel?min(shadowInfo.y,.02):shadowInfo.y),normal,viewmodel),CL_contactShadows);
    lit*=visibility;
    float3 fill=FillLights(p,normal,viewmodel);
    float3 bounce=WorldBounce(p,normal);
    [branch]if(nativeDirect.x>=0){
        // Native roughness, metalness and normal maps already evaluated this
        // sunlight. Only its direct contribution receives sun visibility;
        // ambient detail survives. Do not reconstruct a BRDF from final color.
        color=max(color-nativeFixtures,0)*CL_ambient*AmbientTint()+nativeFixtures+nativeDirect*visibility+color*(fill+bounce);
    }else {
    // Keep the map's baked shading as AO instead of switching sc_disable_baked_lighting, which
    // blows every surface out to unlit white and wrecks lightmapped decals. Divide by a soft
    // luminance so painted/decal colour survives, rather than a full linear unbake.
    float luminance=max(dot(color,float3(.2126,.7152,.0722)),.02);
    // Native weapon colour is already material-shaded, not a lightmapped wall. Dividing it
    // by its own luminance erases dark metal, grip creases and native highlight contrast.
    // Keep a linear response for these pixels. The fixed gain normalizes the retained
    // native fill, without per-pixel "unbaking" or fabricating metallic/roughness values.
    float ao=viewmodel?contactOcclusion:saturate(pow(luminance,.55));
    float3 albedo=viewmodel?color*2:color/pow(luminance,.82);
    float3 hemi=CL_ambient*AmbientTint()*(.55+.45*saturate(normal.z*.5+.5));
    float3 direct=SunColor()*lit*CL_sunRelight;
    if(CL_relight>0) color=lerp(color*(1+fill),albedo*(hemi*ao+direct+fill),CL_relight);
    else {
        // Relight ambient has to reach this path too. The branch above scales the retained baked
        // contribution by CL_ambient; without the same scale here a surface the native replay does
        // not cover -- a tarp, an alpha-blended prop -- keeps its full baked brightness while
        // everything around it dims, which reads as "that object ignores the lighting". The tint
        // rides along for the same reason it does above. CL_ambient is 1 by default, so an
        // untouched scene is unchanged.
        color*=CL_ambient*AmbientTint();
        color*=1+fill;
        color+=color*direct;
    }
    color+=(viewmodel?albedo:receiverAlbedo)*bounce*(viewmodel?contactOcclusion:1);
    }
    return ApplyCoatings(color,p,normal,coatingNormal,mapLuminance,viewmodel);
}
float CloudDensity(float3 p) {
    float h=(p.z-CL_cloudBase)/CL_cloudDepth;
    if(h<=0||h>=1)return 0;
    float3 q=p*CL_cloudScale+float3(climateClock.y*CL_cloudScale,climateClock.y*CL_cloudScale*.28,0);
    float shape=smoothstep(0,.12,h)*(1-smoothstep(.65,1,h));
    return saturate((Fbm(q)-1+CL_coverage)*3)*shape*CL_density;
}
float Phase(float cosine) { float a=CL_scatter;return (1-a*a)/max(.08,pow(1+a*a-2*a*cosine,1.5)); }
// How far the medium reaches along a pixel's ray. The 3D skybox (distant town, hills) sits in a
// reserved depth band that does not unproject to a real distance; it stands beyond the map, so it
// takes the fog of a point a fixed distance out. It used to be skipped entirely, which left the
// background crisp inside fog that had swallowed the sky right behind it.
static const float kSkyboxFogDistance=9000;
float FogDistance(float2 pixel,float d){
    if(IsSky(d))return 24000;
    if(IsSkyboxGeometry(d))return kSkyboxFogDistance;
    return length(World(pixel,d)-camera.xyz);
}
// Fog variation: three octaves at map scale (features of several hundred units), contrast-stretched
// so Variation opens clear pockets and thick banks instead of a faint ripple that a long ray averages
// away. Drifts with the cloud wind. 0..1, mean about 0.5.
float FogVariation(float3 p){
    float3 q=p*.0014+float3(climateClock.y,climateClock.y*.28,0)*.0005+float3(0,0,climateClock.x*.012);
    return saturate((Fbm(q)-.5)*2.6+.5);
}
// Interleaved gradient noise: a per-texel offset whose error a small blur removes, unlike white noise.
float InterleavedGradient(float2 p){return frac(52.9829189*frac(dot(p,float2(.06711056,.00583715))));}
float4 VolumeAt(float2 pixel,float d) {
    bool sky=IsSky(d);
    float3 farPoint=World(pixel,camera.w>0?.00001:.99999);
    float3 ray=normalize(farPoint-camera.xyz);
    float distance=FogDistance(pixel,d);
    float3 color=0;float trans=1;int steps=(int)volumeDimensions.z;
    float jitter=InterleavedGradient(floor(pixel*volumeDimensions.xy/dimensions.xy));
    if(CL_fogDensity>0) {
        float enter=min(distance,CL_fogStart),extent=min(distance,CL_fogStart+CL_fogRange);
        float stepLength=max(0,extent-enter)/steps;
        [loop] for(int j=0;j<steps;++j) {
            float3 p=camera.xyz+ray*(enter+(j+jitter)*stepLength);
            // Variation thins and thickens the fog and raises and lowers its top, so banks roll.
            float vary=CL_fogNoise>0?FogVariation(p):.5;
            float height=exp(-max(0,p.z-CL_fogBase)/(CL_fogHeight*lerp(1,.4+1.2*vary,CL_fogNoise)));
            // Global until placed, same contract as the coatings: radius 0 fogs the map, a placed
            // radius confines it to that volume and leaves it there when the camera moves on.
            float local=CL_localFogRadius>0?1-smoothstep(CL_localFogRadius*.65,CL_localFogRadius,length(p-float3(CL_fogX,CL_fogY,CL_fogZ))):1;
            float density=CL_fogDensity*.001*height*local*lerp(1,vary*2,CL_fogNoise);
            float visibility=CL_lightShafts>0?WorldSunShadow(p+SunDirection()*shadowInfo.y):1;
            float3 illumination=float3(.45,.5,.6)+SunColor()*Phase(dot(ray,SunDirection()))*.25*visibility;
            [loop] for(int l=0;l<min((int)lighting.x,4);++l) {
                [loop]for(int sample=0;sample<StudioSamples(l);++sample){
                    float3 dir;float atten=StudioAttenuation(l,sample,p,dir);
                    if(CL_lightShafts>0&&atten>.001)atten*=WorldLightShadowSample(l,p,sample);
                    illumination+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*Phase(dot(ray,dir));
                }
            }
            float a=1-exp(-density*stepLength);color+=trans*a*illumination*fogColorDensity.rgb;trans*=1-a;
            if(trans<.01)break;
        }
    }
    // Continue the same medium beyond the detailed, lit interval. Disjoint intervals mean
    // near fog is never applied twice. Height and color still come from the same controls.
    if(CL_fogDensity>0 && CL_localFogRadius<=0 && distance>CL_fogStart+CL_fogRange) {
        float enter=CL_fogStart+CL_fogRange,dt=(distance-enter)/8,optical=0;
        [unroll]for(int f=0;f<8;++f) {
            float3 p=camera.xyz+ray*(enter+(f+.5)*dt);
            optical+=CL_fogDensity*.001*exp(-max(0,p.z-CL_fogBase)/CL_fogHeight)*dt;
        }
        float a=1-exp(-optical);
        color+=trans*a*fogColorDensity.rgb;trans*=1-a;
    }
    if(CL_clouds>0&&!IsSkyboxGeometry(d)&&abs(ray.z)>.0001) {
        float t0=(CL_cloudBase-camera.z)/ray.z,t1=(CL_cloudBase+CL_cloudDepth-camera.z)/ray.z;
        float enter=max(0,min(t0,t1)),leave=min(distance,max(t0,t1));leave=min(leave,24000);
        if(leave>enter) {
            float dt=(leave-enter)/steps,cloudTrans=1;float3 cloudColor=0;
            [loop] for(int k=0;k<steps;++k) {
                float3 p=camera.xyz+ray*(enter+(k+jitter)*dt);
                float density=CloudDensity(p);
                if(density>.001) {
                    float shadow=0;
                    [unroll]for(int sh=1;sh<=4;++sh)shadow+=CloudDensity(p+SunDirection()*CL_cloudDepth*(sh*.16));
                    float3 light=lerp(float3(.10,.14,.22),float3(.32,.39,.5),saturate(CL_sunElevation/30));
                    light+=SunColor()*WorldSunShadow(p)*exp(-shadow*1.8)*(.45+.15*Phase(dot(ray,SunDirection())));
                    light+=StormCloudLight(p);
                    float a=1-exp(-density*dt*.004*CL_clouds);
                    cloudColor+=cloudTrans*a*light;cloudTrans*=1-a;
                    if(cloudTrans<.01)break;
                }
            }
            color+=trans*cloudColor;trans*=cloudTrans;
        }
    }
    return float4(color,trans);
}
struct VolumeOutput {float4 scene:SV_Target0;float4 sky:SV_Target1;};
struct VolumeMarchOutput {float4 scene:SV_Target0;float2 geometry:SV_Target1;};
VolumeMarchOutput VolumePS(Screen i) {
    float2 p=i.position.xy*dimensions.xy/volumeDimensions.xy;float d=ReadDepth(p);
    VolumeMarchOutput o;o.scene=VolumeAt(p,d);o.geometry=float2(FogDistance(p,d),IsSky(d));return o;
}
float2 VolumeGeometry(int2 q) {return volumeGeometryTexture.Load(int3(q,0));}
float4 SkyVolumePS(Screen i):SV_Target {
    float2 p=i.position.xy*dimensions.xy/volumeDimensions.xy;float d=ReadDepth(p);
    [branch]if(IsSky(d))return volumeTexture.Load(int3(int2(i.position.xy),0));
    bool sky=IsSky(d);
#if COLOR_MS
    uint w,h,n;sourceColor.GetDimensions(w,h,n);
    [loop]for(uint sample=0;sample<n&&!sky;++sample)sky=IsSky(ReadCombinedSample(p,sample));
#endif
    // Keep minority sky coverage in the same filtered cache as open sky. A closest-depth
    // cache alone forces those samples through an unfiltered, differently jittered march.
    [branch]if(sky)return VolumeAt(p,camera.w>0?0:1);
    return float4(0,0,0,-1);
}
// 5x5 depth-aware Gaussian over the marched volume, at volume resolution. Taps across a depth edge
// are rejected, so shafts stay sharp against silhouettes while the jitter grain averages out.
VolumeOutput VolumeBlurPS(Screen i) {
    int2 q=int2(i.position.xy),size=int2(volumeDimensions.xy);
    float2 geometry=VolumeGeometry(q);bool sky=geometry.y>0;float centre=geometry.x;
    float4 sum=0,skySum=0;float weights=0,skyWeights=0;
    [unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x){
        int2 t=clamp(q+int2(x,y),int2(0,0),size-1);
        float2 neighbour=VolumeGeometry(t);
        float weight=exp(-(x*x+y*y)/4.5);
        float4 skyValue=skyVolumeTexture.Load(int3(t,0));
        if(skyValue.a>=0){skySum+=skyValue*weight;skyWeights+=weight;}
        weight*=(neighbour.y>0)==sky?exp(-abs(neighbour.x-centre)/max(8,centre*.03)):0;
        sum+=volumeTexture.Load(int3(t,0))*weight;weights+=weight;
    }
    VolumeOutput o;o.scene=sum/max(weights,1e-6);
    o.sky=skyWeights>0?skySum/skyWeights:float4(0,0,0,-1);return o;
}
float4 CompositePS(Screen i,uint sample:SV_SampleIndex):SV_Target {
    float2 p=SamplePixel(i.position.xy,sample),uv=p/dimensions.xy;
    float d=ReadCombinedSample(p,sample);bool sky=IsSky(d);
    float distance=FogDistance(p,d);
    float2 v=uv*volumeDimensions.xy-.5;int2 lo=int2(floor(v));float2 f=frac(v);
    float4 sum=0;float weights=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
        int2 q=clamp(lo+int2(x,y),int2(0,0),int2(volumeDimensions.xy)-1);
        float2 geometry=VolumeGeometry(q);bool ss=geometry.y>0;float dist=geometry.x;
        float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        float4 value;
        if(sky){value=skyVolumeTexture.Load(int3(q,0));weight*=value.a>=0?1:0;}
        else {value=volumeTexture.Load(int3(q,0));
            weight*=!ss?exp(-abs(dist-distance)/max(8,distance*.02)):0;}
        sum+=value*weight;weights+=weight;
    }
    // A minority MSAA sample can have no representative in the coarse depth image.
    // Integrate that sample directly; never leave unclouded sky holes around a dark silhouette.
    // HLSL's conditional operator evaluates both operands. Keep the full raymarch
    // behind a real branch: it is needed only for unmatched depth/MSAA edges,
    // not for every sample of every full-resolution pixel.
    float4 volume;
    [branch] if(weights>.01) volume=sum/weights;
    else volume=VolumeAt(p,d);
    // FOG BEHIND A SMOKE. Where a CS2 smoke covers the pixel, only the fog BETWEEN the camera and
    // the smoke may show; the fog integrated past it (a deep doorway behind the cloud) used to be
    // painted over the smoke. That share is re-integrated up to where the smoke starts; with no
    // placed smokes to measure against, the covered share gets no added fog at all.
    float smoke=SmokeCover(p);
    [branch] if(smoke>0) {
        float3 farPoint=World(p,sky?(camera.w>0?.00001:.99999):d);
        float3 dir=normalize(farPoint-camera.xyz);
        float entry=smokeVolumeCount.x>.5?SmokeCoveredEntry(dir,distance):0;
        float4 front=float4(0,0,0,1);
        [branch] if(entry>=distance) front=volume;   // the smoke starts beyond the surface
        else if(entry>1) {
            float4 clip=mul(viewProjection,float4(camera.xyz+dir*entry,1));
            front=VolumeAt(p,clip.z/clip.w);
        }
        volume=lerp(volume,front,smoke);
    }
    float3 color=ReadColor(p,sample);
    // Distant 3D-skybox buildings live in the leftover depth range; VolumeAt gives them the medium
    // out to kSkyboxFogDistance, so they sink into the fog with the sky behind them.
    return float4(color*volume.a+volume.rgb,1);
}
float4 BlitPS(Screen i,uint sampleIndex:SV_SampleIndex):SV_Target {
    float2 p=i.position.xy,uv=p/dimensions.xy;float3 color=ReadColor(p,sampleIndex);
    if(CL_chromatic>0){float2 shift=(uv-.5)*CL_chromatic;color.r=ReadColor(p+shift,sampleIndex).r;color.b=ReadColor(p-shift,sampleIndex).b;}
    if(CL_bloom>0){float3 glow=0;
        [unroll]for(int j=0;j<8;++j){float a=j*.785398;float3 sample=ReadColor(p+float2(cos(a),sin(a))*8,sampleIndex);glow+=max(0,sample-CL_bloomThreshold);}
        color+=glow*(CL_bloom/8);
    }
    float luminance=dot(color,float3(.2126,.7152,.0722));color=lerp(luminance.xxx,color,CL_saturation);
    color=(color-.5)*CL_contrast+.5;
    color*=1-CL_vignette*smoothstep(.15,.7,length(uv-.5));
    color+=(Hash31(float3(p,floor(climateClock.x*24)))-.5)*CL_grain;
    return float4(color,1);
}
)HLSL"
