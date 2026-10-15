// Rain splashes on the ground. Included inside kShader after the weather pass.
//
// WHAT THIS IS. Every drop that reaches the floor breaks into a ring that spreads across the
// surface and a small crown of droplets thrown up and out on a ballistic arc. Both are real
// geometry drawn into the scene, depth-tested against it, so they sit ON the ground from any
// camera angle instead of being a texture painted over pixels (which is what the old crown-light
// was, and why it also appeared on players).
//
// WHERE. Impacts live on a world-anchored lattice around the camera, two nested bands (fine near,
// coarse far). Each cell fires on its own hashed schedule from ABSOLUTE demo time, so a pause
// freezes them and a seek reconstructs the same pattern. The height of the floor is found from the
// scene depth, not assumed: the cell's column is projected, the surface seen there is read back,
// and its height is fed back in, five times. That converges on a flat floor and fails on a wall, a
// stair edge or a silhouette -- those cells simply do not splash, which is exactly what should
// happen. Because it is the SCENE depth, a player's body is never a landing point; the ground
// beneath a player is (a point inside a player's capsule higher than the knee is refused).
//
// WHERE NOT. Rain exposure (the same precipitation shelter map the falling rain uses) is tested at
// the landing point per fragment, so a covered floor stays dry, and a missing shelter map fails
// open exactly as the falling rain does.
R"HLSL(
static const float kSplashGravity=800.0;
struct SplashOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float4 info:TEXCOORD1; float3 world:TEXCOORD2; float3 hit:TEXCOORD3; };
// The floor under column `xy`, or false when the column does not land on a flat visible surface.
bool FindGround(float2 xy,out float3 hit) {
    hit=float3(xy,camera.z-56);
    float pixelScale=2/max(1,dimensions.y*length(viewProjection[1].xyz));
    float previous=hit.z;
    [loop]for(int k=0;k<5;++k){
        float4 c=mul(viewProjection,float4(hit,1));
        if(c.w<=24)return false;
        float2 ndc=c.xy/c.w;
        if(any(abs(ndc)>.98))return false;
        float2 pix=(ndc*float2(.5,-.5)+.5)*dimensions.xy;
        float d=ReadScene(pix);
        if(IsSky(d)||IsSkyboxGeometry(d))return false;
        float3 w=World(pix,d);
        previous=hit.z;
        hit.z=w.z;
        if(k==4){
            // Converged only if the surface seen here is the one at this column, and it stopped moving.
            if(length(w.xy-xy)>max(5,c.w*pixelScale*3.5)||abs(w.z-previous)>3.5)return false;
        }
    }
    // Players: the capsule around a pawn above knee height is a body, not a landing point.
    int count=(int)coatingActorCount.x;
    [loop]for(int i=0;i<count;++i){
        float radius=coatingActors[i].w;
        if(radius<=0)continue;
        float2 offset=hit.xy-coatingActors[i].xy;
        if(dot(offset,offset)<radius*radius&&hit.z-coatingActors[i].z>20)return false;
    }
    return true;
}
SplashOutput SplashVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    SplashOutput o=(SplashOutput)0;
    o.position=float4(2,2,2,1);
    const uint cells=SPLASH_GRID*SPLASH_GRID;
    uint band=instance/(cells*SPLASH_PARTS),rem=instance%(cells*SPLASH_PARTS);
    uint cell=rem/SPLASH_PARTS,part=rem%SPLASH_PARTS;
    float range=band==0?SPLASH_NEAR:SPLASH_FAR,cs=2*range/SPLASH_GRID;
    int2 base=(int2)floor(weatherCenterRadius.xy/cs)-SPLASH_GRID/2;
    int2 id=base+int2(cell%SPLASH_GRID,cell/SPLASH_GRID);
    uint n=(uint)id.x*73856093U^(uint)id.y*19349663U^(uint)weatherParams.w*7919U^band*104729U;
    float period=lerp(.55,1.5,Random01(n));
    float phase=climateClock.x/period+Random01(n+1);
    float event=floor(phase),age=frac(phase)*period;
    uint en=n^((uint)event*2654435761U);
    const float life=.46;
    if(age>life)return o;
    // Heavier rain means more cells fire per cycle. At most about a third of them: 80% read as a
    // carpet of splashes (user, 2026-09-29).
    if(Random01(en+3)>saturate(weatherColor.w*1.25)*.3)return o;
    float2 xy=(float2(id)+float2(Random01(en+5),Random01(en+6)))*cs;
    float2 toCamera=xy-camera.xy;
    float dist2=length(toCamera);
    // Nested bands: the fine band hands over to the coarse band without doubling up.
    float outer=1-smoothstep(range*.7,range*.95,dist2);
    float inner=band==0?1:smoothstep(SPLASH_NEAR*.72,SPLASH_NEAR*.95,dist2);
    float fade=outer*inner;
    if(fade<=0)return o;
    float3 hit;
    if(!FindGround(xy,hit))return o;
    // The Ripple control also sizes the splashes: it is the strength of the rain's impact on water.
    float sizeScale=(.75+.5*Random01(en+7))*clamp(CL_ripple/.35,.4,2.5)*(band==0?1:1.6);
    float3 eye=camera.xyz-hit;
    float eyeDist=length(eye);
    float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    float2 uv=corners[vertex];
    float pixelWorld=2*eyeDist/max(1,dimensions.y*length(viewProjection[1].xyz));
    // Only right at the lens: fading out over the first 110 units left a low camera with no splashes.
    float nearFade=smoothstep(6,24,eyeDist);
    float3 world;float alpha;float kind;
    if(part==0){
        // The spreading ring, flat on the surface.
        float t=age/life;
        float r=(1.4+8.5*sqrt(t))*sizeScale;
        r=max(r,pixelWorld*1.5);
        world=hit+float3(uv*r,.3);
        alpha=pow(1-t,1.4)*.62;
        kind=0;
    }else{
        uint j=part-1;
        float theta=Random01(en+10+j)*6.2831853;
        float vh=lerp(20,80,Random01(en+20+j))*sizeScale;
        float vz=lerp(70,150,Random01(en+30+j))*sizeScale;
        float flight=2*vz/kSplashGravity;
        if(age>flight)return o;
        float2 dirXY=float2(cos(theta),sin(theta));
        float3 velocity=float3(dirXY*vh,vz-kSplashGravity*age);
        float3 center=hit+float3(dirXY*vh*age,vz*age-.5*kSplashGravity*age*age+.4);
        float3 dirV=normalize(velocity+float3(0,0,1e-4));
        float3 view=normalize(camera.xyz-center);
        float3 side=cross(dirV,view);
        float sl=length(side);
        side=sl>.02?side/sl:cameraRight.xyz;
        float physical=.42*sizeScale;
        float width=max(physical,pixelWorld*.7);
        float length_=max(width*1.4,length(velocity)*.012+width);
        world=center+side*uv.x*width+dirV*uv.y*length_;
        alpha=(1-pow(age/flight,3))*(physical/width);
        kind=1;
    }
    o.world=world;
    o.position=mul(viewProjection,float4(world,1));
    o.uv=uv;
    o.hit=hit;
    o.info=float4(alpha*fade*nearFade*saturate(CL_particleOpacity*1.6),kind,age/life,0);
    if(o.info.x<=0)o.position=float4(2,2,2,1);
    return o;
}
float4 SplashPS(SplashOutput i,uint sample:SV_SampleIndex):SV_Target {
    if(Hidden(i.position.z,ReadCombinedSample(i.position.xy,sample)))discard;
    // Partial exposure keeps that share of splashes (one fixed draw per impact point), so a doorway
    // facing the rain gets a few splashes just inside, thinning out, instead of none or all.
    if(shelterInfo.x>=0&&Hash31(floor(i.hit*.25)+float3(0,0,weatherParams.w))>=PrecipitationExposure(i.hit+float3(0,0,2)))discard;
    float smoke=SmokeHides(i.position.xy,i.world);
    if(smoke>=.999)discard;
    float a;
    if(i.info.y<.5){
        float r=length(i.uv);
        // A thin bright ring with a soft inner edge, brighter seen at a low angle (where water catches
        // the light) and nearly gone looking straight down.
        float ring=exp(-pow((r-.8)/.13,2));
        float3 view=normalize(camera.xyz-i.hit);
        float sheen=.35+.65*pow(1-saturate(view.z),1.3);
        a=ring*smoothstep(1,.9,r)*sheen;
    }else{
        a=pow(saturate(1-abs(i.uv.x)),2)*saturate(1-abs(i.uv.y));
    }
    float3 color=weatherColor.rgb*RainLighting(i.world,i.position.xy,sample)*RAIN_BRIGHTNESS*.8*tintExposure.rgb*tintExposure.w;
    float distance=length(i.world-camera.xyz);
    float optical=max(0,distance-CL_fogStart)*CL_fogDensity*.001*exp(-max(0,i.world.z-CL_fogBase)/CL_fogHeight);
    a*=exp(-optical);
    color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    // Capped like the falling drops: a splash is a brief glint, not a white ring painted on the ground.
    return float4(color,min(saturate(a*1.4)*i.info.x*(1-smoke),RAIN_MAX_OPACITY*1.25));
}
)HLSL"
