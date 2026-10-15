// Original implementation of water-film Fresnel and timed normal impulses; see the research
// references in the environment contract. No external shader source or Valve assets are copied.
R"HLSL(
float RainExposure(float3 p) {
    if(weatherParams.z!=1||weatherColor.w<=0)return 0;
    // An unprepared roof map may draw falling rain, but cannot assert a surface is exposed.
    if(shelterInfo.x<0)return 0;
    // Graded, not a cut: the share of mean and gust directions that reach the point, so a doorway
    // or arch facing the rain is damp for a short way in and then dry.
    return smoothstep(0,.9,PrecipitationExposure(p+float3(0,0,2)))*saturate(weatherColor.w*2);
}
// Scene radiance is already lit by CS2, including colored map lights and camera exposure.
// Broad taps avoid coloring each drop with one sharp background texel. This is a local
// illumination estimate, not access to the engine's material buffers or reflection probes.
float3 RainSceneRadiance(float2 pixel,uint sample) {
    float3 sum=ReadSource(pixel,sample)*2;
    sum+=ReadSource(pixel+float2(-24,-18),sample)+ReadSource(pixel+float2(24,-18),sample);
    sum+=ReadSource(pixel+float2(-24,18),sample)+ReadSource(pixel+float2(24,18),sample);
    return max(sum/6,0);
}
float3 RainLighting(float3 p,float2 pixel,uint sample) {
    float3 scene=RainSceneRadiance(pixel,sample);
    // Keep native fixture illumination and hue when the sky changes to night.
    // Replacing it with the sky/flat ambient erased lamps from rain lighting.
    float3 ambient=scene*CL_ambient;
    float visibility=WorldSunShadowQuick(p,float3(0,0,1));
    float3 illumination=ambient+SunColor()*visibility*CL_sunRelight*.3;
    float3 view=normalize(camera.xyz-p);
    // Isotropic plus forward-scattered light, with the same light-space shadows as props.
    [loop]for(int l=0;l<(int)lighting.x;++l){
        float3 dir;float atten=StudioAttenuation(l,0,p,dir);
        if(atten>0)atten*=WorldLightShadowSample(l,p,0);
        float phase=.25+.75*pow(saturate(dot(-view,dir)),8);
        illumination+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*phase*CL_particleLight;
    }
    return illumination;
}
// WHAT A WET SURFACE MIRRORS when neither the world trace nor the procedural sky is on. Without
// this the wet film reflected the surface's own colour, so wetness only ever read as darkening.
//
// A SCREEN-SPACE MARCH, done properly. The first version stepped through WORLD space with steps that
// grew with distance and accepted any hit up to 16+ units behind a surface. That produced the two
// misalignments that were visible on wet ground: (1) the reflected image landed a whole step away
// from where a mirror puts it, because a hit was never refined; (2) any silhouette the ray slid
// behind was smeared into a streak, because "up to 16 units behind" is not a thin surface.
//
// This one walks the reflected ray in SCREEN space (perspective-correct: 1/w is linear on screen),
// one stride of a few pixels at a time, compares the ray's view depth against the scene's view depth
// at that pixel, accepts only a hit within about one stride of thickness, then bisects between the
// last miss and the hit so the reflection sits on the surface it came from. Edges and grazing hits
// fade out instead of cutting.
float SceneViewDepth(float2 pixel,float d) {
    float2 ndc=pixel/dimensions.xy*float2(2,-2)+float2(-1,1);
    float4 h=mul(inverseViewProjection,float4(ndc,d,1));
    return abs(1/max(abs(h.w),1e-9));
}
float2 ClipPixel(float4 c) { return (c.xy/c.w*float2(.5,-.5)+.5)*dimensions.xy; }
// One depth sample per march step. ReadScene takes the nearest of every MSAA sample (four loads at
// 4x), and this pass itself runs per sample, so the full-quality read made each wet pixel issue
// several hundred loads; a reflection lookup does not need sub-pixel depth.
float ReadSceneMarch(float2 pixel) {
    int2 p=clamp(int2(pixel*dimensions.zw/dimensions.xy),int2(0,0),int2(dimensions.zw)-1);
#if DEPTH_MS
    return NormalizeDepth(sceneDepth.Load(p,0));
#else
    return NormalizeDepth(sceneDepth.Load(int3(p,0)));
#endif
}
float3 WetScreenReflection(float3 p,float3 normal,float3 reflected,float3 fallback) {
    float3 origin=p+normal*1.5;
    float4 c0=mul(viewProjection,float4(origin,1));
    if(c0.w<=1)return fallback;
    float dw=mul(viewProjection,float4(reflected,0)).w;
    // A ray heading back at the camera runs out where it reaches the near plane.
    float maxT=2600;
    if(dw<0)maxT=min(maxT,(c0.w-2)/-dw);
    if(maxT<8)return fallback;
    float4 c1=mul(viewProjection,float4(origin+reflected*maxT,1));
    float2 p0=ClipPixel(c0),p1=ClipPixel(c1);
    float k0=1/c0.w,k1=1/c1.w;
    float2 delta=p1-p0;
    float lenPix=length(delta);
    // Straight along the view axis: the mirrored point is directly behind the surface point, so
    // there is nothing on screen to find.
    if(lenPix<2)return fallback;
    // Only march the part of the segment that is on screen.
    float tClip=1;
    if(delta.x>0)tClip=min(tClip,(dimensions.x-1-p0.x)/delta.x);else if(delta.x<0)tClip=min(tClip,-p0.x/delta.x);
    if(delta.y>0)tClip=min(tClip,(dimensions.y-1-p0.y)/delta.y);else if(delta.y<0)tClip=min(tClip,-p0.y/delta.y);
    tClip=saturate(tClip);
    // Strides grow along the ray (fine near the surface, where a reflection's contact is seen,
    // coarse far out), and bisection restores the precision a long stride gives up.
    const int steps=24;
    float clippedPix=lenPix*tClip;
    float jitter=Hash31(float3(floor(p0),weatherParams.w))*.9+.1;
    float pixelWorldScale=2/max(1,dimensions.y*length(viewProjection[1].xyz));
    float lastT=0;
    float hitT=-1;
    [loop]for(int k=0;k<steps;++k){
        float u=(k+jitter)/steps;
        float t=tClip*u*u;
        if(max(t-lastT,0)*lenPix<1&&k<steps-1){continue;} // sub-pixel step: nothing new to read
        float stridePix=max((t-lastT)*lenPix,1.5);
        float2 pix=p0+delta*t;
        float d=ReadSceneMarch(pix);
        float rayW=1/lerp(k0,k1,t);
        if(!IsSky(d)){
            float diff=rayW-SceneViewDepth(pix,d);
            float thick=rayW*pixelWorldScale*stridePix*1.4+2;
            if(diff>.02&&diff<thick){
                // Refine: bisect between the last miss and this hit.
                float lo=lastT,hi=t;
                [unroll]for(int r=0;r<5;++r){
                    float mid=(lo+hi)*.5;
                    float2 mp=p0+delta*mid;
                    float md=ReadSceneMarch(mp);
                    bool behind=!IsSky(md)&&(1/lerp(k0,k1,mid)-SceneViewDepth(mp,md))>.02;
                    if(behind)hi=mid;else lo=mid;
                }
                hitT=hi;
                break;
            }
        }
        lastT=t;
    }
    if(hitT>=0){
        float2 pix=p0+delta*hitT;
        float2 ndc=pix/dimensions.xy*2-1;
        // Fade toward the frame edge (the truthful answer there is "unknown") and for hits that took
        // most of the march to find.
        float edge=saturate((1-max(abs(ndc.x),abs(ndc.y)))*6);
        float travel=1-saturate(hitT/max(tClip,1e-4)*.5);
        return lerp(fallback,ReadSource(pix,0),edge*(.35+.65*travel));
    }
    // Nothing on screen: if the ray leaves through open sky and the sky is visible where it points,
    // take the frame's own sky there.
    float4 far=mul(viewProjection,float4(origin+reflected*20000,1));
    if(far.w>1){
        float2 ndc=far.xy/far.w;
        if(all(abs(ndc)<1)){
            float2 pixel=(ndc*float2(.5,-.5)+.5)*dimensions.xy;
            if(IsSky(ReadSceneMarch(pixel)))return lerp(fallback,ReadSource(pixel,0),saturate((1-max(abs(ndc.x),abs(ndc.y)))*5));
        }
    }
    return fallback;
}
// Finite-support droplets: an expanding capillary ring followed by a short gravity trail.
// The derivative is analytical (not a darkening pulse), and footprint filtering removes
// subpixel impulses before they become a glittering screen-space pattern.
float2 RainImpulse(float2 uv,float footprint,float time) {
    float2 base=floor(uv),gradient=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
        float2 cell=base+float2(x,y);
        float seed=Hash31(float3(cell,weatherParams.w));
        float cycle=time*1.35+seed*19;
        float age=frac(cycle),event=floor(cycle);
        float2 center=cell+.18+.64*float2(Hash31(float3(cell,event)),Hash31(float3(cell+47,event)));
        float2 q=uv-center;float r=length(q),wave=r-age*.65;
        float envelope=(1-smoothstep(.28,.7,age))*smoothstep(0,.035,age);
        float band=max(.045,footprint*.7);
        float ring=-wave/band*exp(-wave*wave/(band*band))*envelope;
        float bead=exp(-dot(q,q)/.009)*exp(-age*12);
        gradient+=q/max(r,.001)*(ring*.16+bead*.22);
    }
    return gradient*(1-smoothstep(.15,.6,footprint));
}
float3 ActiveRainImpacts(float3 color,float3 p,float3 normal,float2 pixel,uint sample) {
    float distance=length(camera.xyz-p);
    float exposure=RainExposure(p);
    if(exposure<=0)return color;
    float3 source=normalize(float3(-weatherVelocity.xy,max(1,-weatherVelocity.z)));
    float facing=smoothstep(-.12,.65,dot(normal,source));
    float3 axis=abs(normal.z)>.65?float3(1,0,0):normalize(cross(float3(0,0,1),normal));
    float3 bitangent=cross(normal,axis);
    // ONE impact field at every distance, anchored to the world. A near-camera variant built from the
    // camera's right/up axes read as a second, screen-space set of rings that stretched as it moved.
    float3 local=p;
    float2 uv=abs(normal.z)>.65?local.xy:float2(dot(local,axis),local.z);
    float scale=.13;
    float2 q=uv*scale;
    float footprint=max(length(ddx(q)),length(ddy(q)));
    float2 slope=RainImpulse(q,footprint,climateClock.x)*exposure*facing;
    // The bright crown that used to be painted here is now real splash geometry (SplashVS), which
    // sits on the ground from any angle and never lands on a player.
    float activity=saturate(length(slope)*6);
    if(activity<.001)return color;
    float3 n=normalize(normal+axis*slope.x+bitangent*slope.y);
    float3 view=normalize(camera.xyz-p),halfVector=normalize(view+source);
    float f=.0204+.9796*pow(1-saturate(dot(n,view)),5);
    float highlight=pow(saturate(dot(n,halfVector)),72);
    float3 light=RainLighting(p,pixel,sample);
    // Refraction taps are accepted only on this surface: never drag a bright silhouette
    // across a player edge, or sample a background through the weapon.
    float2 offset=float2(dot(axis*slope.x+bitangent*slope.y,cameraRight.xyz),-dot(axis*slope.x+bitangent*slope.y,cameraUp.xyz))*3;
    float2 tap=pixel+offset;
    float tapDepth=ReadSampleDepth(tap,sample);
    float3 refracted=color;
    if(!IsSky(tapDepth)&&length(World(tap,tapDepth)-p)<max(1,distance*.002))refracted=ReadSource(tap,sample);
    return lerp(color,refracted,activity*.18)+light*(f*.7+highlight*.28)*activity;
}
)HLSL"
