// Rain splashes on the ground. Included inside kShader after the weather pass.
//
// WHAT THIS IS. Every drop that reaches the floor throws up a small cup of water, a few centimetres
// across and gone in an eighth of a second, and a few fine droplets on ballistic arcs. Both are
// real geometry drawn into the scene, depth-tested against it, so they stand ON the ground from any
// camera angle. They are small, dim and lit by the scene: a splash is seen as a brief grey smudge
// against what is behind it, not as a white mark. The rings a drop leaves belong to the water: they
// bend what a puddle mirrors (RainImpulse in ApplyCoatings) and nothing is drawn for them here.
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
    const float life=.3;
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
    float3 world;float alpha;float kind;float stage;
    if(part==0){
        // The cup: an upright smudge facing the camera that opens and rises as it thins.
        const float cup=.13;
        if(age>cup)return o;
        stage=age/cup;
        float w=max((1.4+2.0*sqrt(stage))*sizeScale,pixelWorld*1.5),h=max((.9+2.1*sqrt(stage))*sizeScale,pixelWorld*1.5);
        float3 right=cross(float3(0,0,1),eye);
        float rl=length(right);
        right=rl>.02*eyeDist?right/rl:cameraRight.xyz;
        world=hit+right*uv.x*w+float3(0,0,(uv.y+1)*h+.1);
        alpha=(1-stage*stage)*.8;
        kind=0;
    }else{
        uint j=part-1;
        float theta=Random01(en+10+j)*6.2831853;
        float vh=lerp(10,45,Random01(en+20+j))*sizeScale;
        float vz=lerp(40,110,Random01(en+30+j))*sizeScale;
        float flight=min(2*vz/kSplashGravity,life);
        if(age>flight)return o;
        stage=age/flight;
        float2 dirXY=float2(cos(theta),sin(theta));
        float3 velocity=float3(dirXY*vh,vz-kSplashGravity*age);
        float3 center=hit+float3(dirXY*vh*age,vz*age-.5*kSplashGravity*age*age+.4);
        float3 dirV=normalize(velocity+float3(0,0,1e-4));
        float3 view=normalize(camera.xyz-center);
        float3 side=cross(dirV,view);
        float sl=length(side);
        side=sl>.02?side/sl:cameraRight.xyz;
        float physical=lerp(.1,.22,Random01(en+40+j))*sizeScale;
        float width=max(physical,pixelWorld*.7);
        float length_=max(width*1.4,length(velocity)*.012+width);
        world=center+side*uv.x*width+dirV*uv.y*length_;
        alpha=(1-pow(stage,3))*(physical/width)*.8;
        kind=1;
    }
    o.world=world;
    o.position=mul(viewProjection,float4(world,1));
    o.uv=uv;
    o.hit=hit;
    o.info=float4(alpha*fade*nearFade*saturate(CL_particleOpacity*1.6),kind,stage,0);
    // Behind a ground bank or a gust's drift, the splash shows through it as much as it lets through.
    [branch]if(o.info.x>0)o.info.x*=FloorVolumeThrough(hit,80);
    if(o.info.x<=0)o.position=float4(2,2,2,1);
    return o;
}
float4 SplashPS(SplashOutput i,uint sample:SV_SampleIndex):SV_Target {
    if(i.info.w>1.5){
        // The foot view: a cross (green when the foot is down, red when lifted, white in the air)
        // or a yellow ring where a footfall fired, fading.
        if(i.info.y<3.5){
            if(min(abs(i.uv.x),abs(i.uv.y))>.18)discard;
            return float4(i.info.z>1e8?float3(1,1,1):i.info.z<FOOT_DOWN?float3(0,1,0):float3(1,0,0),1);
        }
        float r=length(i.uv);
        if(r>1||r<.82)discard;
        return float4(1,.9,0,1-i.info.z);
    }
    if(Hidden(i.position.z,ReadCombinedSample(i.position.xy,sample)))discard;
    bool foot=i.info.w>.5;
    // A foot splashes only where it came down in standing water. Where snow or sand lies instead,
    // it kicks up a small puff of powder or dry sand: the same sheets, soft, with no froth.
    bool sandFoot=foot&&SandLies()&&CL_sand>=.25;
    bool powder=foot&&((!SandLies()&&CL_snow>=.25)||sandFoot);
    if(foot&&!powder&&PuddleAt(i.hit)<.3)discard;
    if(powder&&(i.info.y<.5||WeatherCover(i.hit,float3(0,0,1)).x<.5))discard;
    // Partial exposure keeps that share of splashes (one fixed draw per impact point), so the wetted fringe
    // inside a doorway gets a few splashes, thinning out, instead of none or all.
    if(!foot&&shelterInfo.x>=0&&Hash31(floor(i.hit*.25)+float3(0,0,weatherParams.w))>=WetReach(WeatherCover(i.hit,float3(0,0,1))))discard;
    float smoke=SmokeHides(i.position.xy,i.world);
    if(smoke>=.999)discard;
    float a;
    if(foot&&i.info.y<.5){
        // Where the boot went in the water is churned for a moment: a ragged patch of froth, only
        // on the water itself.
        float r=length(i.uv);
        a=pow(saturate(1-r),.7)*smoothstep(.38,.62,Noise3(float3(i.world.xy*.9,i.hit.x*.37)))*smoothstep(.25,.6,PuddleAt(i.world));
    }else if(foot&&i.info.y>1.5){
        // A sheet of thrown water: whole at first, then it thins, opens into holes and tears into
        // shreds (the noise it is cut from is fixed to the world, so it does not swim).
        float body=saturate(1-length(i.uv*float2(1,.9)));
        float n=Noise3(float3(i.world.xy*.7,i.world.z*.4)+i.hit.yxz*.21)*.5+Noise3(i.world*2.3+i.hit.zxy*.4)*.32+Noise3(i.world*5.1)*.18;
        float cut=.1+.5*i.info.z,m=n*pow(body,.5);
        a=smoothstep(cut,cut+.1,m)*(.45+.55*(1-smoothstep(cut+.1,cut+.3,m)))*pow(body,.4);
    }else if(foot){
        // A drop in flight: a bead drawn out along the way it moves.
        float across=saturate(1-abs(i.uv.x)),lengthwise=saturate(1-abs(i.uv.y));
        a=pow(across,1.3)*pow(lengthwise,.8)*(.55+.45*pow(across,4));
    }else if(i.info.y<.5){
        // The cup a raindrop throws up, seen from the side: its wall shows at the two edges, it
        // flares upward and its rim is torn. Thin in the middle, where you look through it.
        float x=abs(i.uv.x),y=i.uv.y*.5+.5;
        float flare=lerp(.3,.8,pow(y,.7));
        float rag=Noise3(float3(i.uv.x*3.5+i.hit.x,i.hit.y*.7+y*2,i.info.z*2+i.hit.x*.3));
        float top=.5+.5*rag;
        a=(exp(-pow((x-flare)/.2,2))+.4*smoothstep(flare,flare-.3,x))*smoothstep(top,top-.4,y)*smoothstep(0,.1,y)*(.55+.45*rag);
    }else{
        a=pow(saturate(1-abs(i.uv.x)),2)*saturate(1-abs(i.uv.y));
    }
    // Lit by the scene and no brighter than it: thrown water is clear, it shows as a grey smudge
    // against what is behind it and catches lamps from behind (the forward scatter in RainLighting).
    float3 powderTone=sandFoot?float3(CL_sandColorR,CL_sandColorG,CL_sandColorB)*.82:float3(.95,.97,1)*.58;
    if(powder)powderTone*=.78+.44*Hash31(floor(i.world*4));
    float3 tone=powder?powderTone:foot?float3(.8,.86,.93)*(i.info.y>1.5?.5:i.info.y>.5?.75:.6):weatherColor.rgb*.75;
    float3 color=tone*RainLighting(i.world,i.position.xy,sample)*RAIN_BRIGHTNESS*.8*tintExposure.rgb*tintExposure.w;
    float distance=length(i.world-camera.xyz);
    float optical=max(0,distance-CL_fogStart)*CL_fogDensity*.001*exp(-max(0,i.world.z-CL_fogBase)/CL_fogHeight);
    a*=exp(-optical);
    color=lerp(color,fogColorDensity.rgb,1-exp(-optical));
    return float4(color,min(saturate(a*1.2)*i.info.x*(1-smoke),powder?.38:foot?.45:RAIN_MAX_OPACITY));
}
// A FOOT IN A PUDDLE. Shallow water hit by a boot does not stand up in a crown: it is thrown out
// low and fast as a sheet that tears into drops, most of them small, with a puff of fine spray over
// the spot, and the water left behind carries ripples. So each recent step (footSteps) draws nine
// sheets of thrown water that tear apart as they fly, many small drops on ballistic arcs and a
// patch of froth where it went in. Drawn for every step; the pixel stage keeps only those that
// landed in standing water.
SplashOutput FootSplashVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    SplashOutput o=(SplashOutput)0;
    o.position=float4(2,2,2,1);
    float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    float2 uv=corners[vertex];
    [branch]if(instance>=FOOT_STEPS*FOOT_SPLASH_PARTS){
        // THE FOOT VIEW (`environment footview`): a cross on every foot as it is read this frame,
        // facing the camera, and a ring flat on the ground for a third of a second where a footfall
        // was taken to happen. info.w = 2 marks them for the pixel stage, which draws them over
        // everything.
        uint d=instance-FOOT_STEPS*FOOT_SPLASH_PARTS;
        if(footInfo.y<.5)return o;
        if(d<FOOT_LIVE){
            float4 foot=footLive[d];
            if(foot.w<-1e8)return o;
            o.world=foot.xyz+(cameraRight.xyz*uv.x+normalize(cross(cameraRight.xyz,camera.xyz-foot.xyz))*uv.y)*2.2;
            o.info=float4(1,3,foot.w,2);
        }else{
            float4 fell=footSteps[d-FOOT_LIVE];
            float since=climateClock.x-fell.w;
            if(since<0||since>.33)return o;
            o.world=fell.xyz+float3(uv*7,.3);
            o.info=float4(1,4,since/.33,2);
        }
        o.position=mul(viewProjection,float4(o.world,1));
        o.uv=uv;
        return o;
    }
    uint s=instance/FOOT_SPLASH_PARTS,part=instance%FOOT_SPLASH_PARTS;
    // Powder snow or dry sand kicks up sheets plus a dense spray of granular clumps and grains.
    bool powderStep=(CL_snow>=.25||CL_sand>=.25);
    bool sandStep=CL_sand>=.25&&CL_sand>CL_snow;
    if(powderStep&&part>=48)return o;
    float4 landed=footSteps[s];
    float age=climateClock.x-landed.w;
    // HOW HARD THE FOOT CAME DOWN (FootImpact): about .55 a walk, .85 a run, 1.5 and up a landing
    // from a jump or a drop. It sets how much water is thrown, how fast (so how high: with the square
    // of it) and how wide the sheets and the froth are. Every step threw a run's splash before.
    float impact=footImpact[s/4][s%4];
    impact=impact>0?impact:.75;
    float throwScale=.6+.4*impact,sizeScale=.55+.45*impact;
    float life=.9*max(1,throwScale);
    if(age<0||age>life)return o;
    float3 hit=landed.xyz;
    uint en=(uint)(abs(landed.x)*13+abs(landed.y)*7+abs(landed.w)*31)*2654435761U^s*40503U;
    float eyeDist=length(camera.xyz-hit);
    float pixelWorld=2*eyeDist/max(1,dimensions.y*length(viewProjection[1].xyz));
    float nearFade=smoothstep(6,24,eyeDist);
    float3 world;float alpha;float kind;float stage=age/life;
    if(part==0){
        const float churn=.38;
        if(age>churn)return o;
        float t=age/churn;
        float r=max((5+7*sqrt(t))*sizeScale,pixelWorld*1.5);
        world=hit+float3(uv*r,.4);
        alpha=(1-t)*.4;
        kind=0;
    }else if(part<10){
        // A sheet: it leaves low and outward, rises a hand or two, spreads and slows, and is torn
        // apart by the time it comes down.
        const float fly=.34;
        if(age>fly)return o;
        stage=age/fly;
        float theta=(part+Random01(en+60+part))*(6.2831853/9);
        float2 dirXY=float2(cos(theta),sin(theta));
        float vz=lerp(55,120,Random01(en+80+part))*throwScale;
        float up=max(vz*age-.5*kSplashGravity*age*age,0);
        float3 center=hit+float3(dirXY*(3+lerp(22,60,Random01(en+90+part))*throwScale*age*(1-.45*stage)),1.2+up*.6);
        float3 right=cross(float3(0,0,1),camera.xyz-center);
        float rl=length(right);
        right=rl>.02*eyeDist?right/rl:cameraRight.xyz;
        float w=(2.5+7*stage)*sizeScale,h=(1.6+2.5*stage)*sizeScale+up*.6;
        world=center+right*uv.x*w+float3(0,0,uv.y*h);
        alpha=(1-stage)*smoothstep(0,.08,stage)*.6;
        kind=2;
    }else{
        uint j=part-10;
        // A soft step throws fewer drops/grains than a run, a landing all of them.
        if(j>=(uint)(66*saturate(.2+.55*impact)))return o;
        float theta=Random01(en+10+j)*6.2831853;
        float vh=lerp(30,powderStep&&sandStep?105:130,Random01(en+20+j))*throwScale;
        float vz=lerp(30,powderStep&&sandStep?160:190,pow(Random01(en+30+j),1.5))*throwScale;
        float flight=2*vz/kSplashGravity;
        float a=age-Random01(en+50+j)*.06;
        if(a<0||a>flight)return o;
        float2 dirXY=float2(cos(theta),sin(theta));
        float3 velocity=float3(dirXY*vh,vz-kSplashGravity*a);
        float3 center=hit+float3(dirXY*(2+3*Random01(en+70+j)+vh*a),vz*a-.5*kSplashGravity*a*a+.4);
        float3 dirV=normalize(velocity+float3(0,0,1e-4));
        float3 view=normalize(camera.xyz-center);
        float3 side=cross(dirV,view);
        float sl=length(side);
        side=sl>.02?side/sl:cameraRight.xyz;
        float physical=lerp(powderStep?.18:.14,powderStep?(sandStep?.48:.65):.55,pow(Random01(en+40+j),2))*(.8+.2*impact);
        float width=max(physical,pixelWorld*(powderStep?.85:.7));
        float length_=powderStep?width*lerp(1.05,1.45,Random01(en+65+j)):max(width*1.3,length(velocity)*lerp(.016,.008,saturate(a/.1))+width);
        world=center+side*uv.x*width+dirV*uv.y*length_;
        alpha=(1-pow(a/flight,4))*(physical/width);
        kind=1;
    }
    o.world=world;
    o.position=mul(viewProjection,float4(world,1));
    o.uv=uv;
    o.hit=hit;
    o.info=float4(alpha*nearFade,kind,stage,1);
    [branch]if(o.info.x>0)o.info.x*=FloorVolumeThrough(hit,80);
    if(o.info.x<=0)o.position=float4(2,2,2,1);
    return o;
}
)HLSL"
