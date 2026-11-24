// Included inside kShader. This is HLSL, kept separate from the scene/mesh plumbing.
#include "EnvironmentStormShaders.h"
R"HLSL(
float Hash31(float3 p) { p=frac(p*.1031); p+=dot(p,p.yzx+33.33); return frac((p.x+p.y)*p.z); }
float Noise3(float3 p) {
    float3 a=floor(p),f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(lerp(Hash31(a),Hash31(a+float3(1,0,0)),f.x),lerp(Hash31(a+float3(0,1,0)),Hash31(a+float3(1,1,0)),f.x),f.y),
        lerp(lerp(Hash31(a+float3(0,0,1)),Hash31(a+float3(1,0,1)),f.x),lerp(Hash31(a+float3(0,1,1)),Hash31(a+1),f.x),f.y),f.z);
}
// The same noise with how it slopes across x and y, from the one set of eight corners. (Grain and
// bumps used to take three evaluations to difference it.)
float Noise3Slope(float3 p,out float2 slope) {
    float3 a=floor(p),f=frac(p),u=f*f*(3-2*f),du=6*f*(1-f);
    float h000=Hash31(a),h100=Hash31(a+float3(1,0,0)),h010=Hash31(a+float3(0,1,0)),h110=Hash31(a+float3(1,1,0));
    float h001=Hash31(a+float3(0,0,1)),h101=Hash31(a+float3(1,0,1)),h011=Hash31(a+float3(0,1,1)),h111=Hash31(a+1);
    float x00=lerp(h000,h100,u.x),x10=lerp(h010,h110,u.x),x01=lerp(h001,h101,u.x),x11=lerp(h011,h111,u.x);
    slope=float2(lerp(lerp(h100-h000,h110-h010,u.y),lerp(h101-h001,h111-h011,u.y),u.z)*du.x,lerp(x10-x00,x11-x01,u.z)*du.y);
    return lerp(lerp(x00,x10,u.y),lerp(x01,x11,u.y),u.z);
}
float Fbm(float3 p) { return Noise3(p)*.57+Noise3(p*2.03+17)*.29+Noise3(p*4.11-9)*.14; }
)HLSL"
R"HLSL(
// WIND GUSTS: THE AIR, AS EVERY PASS READS IT. The air is a fluid over the whole map
// (EnvironmentWindFluid.h, bound at t42 for the composite, the particles and the veil): gust fronts
// sweep downwind through it, it squeezes down streets and curls round buildings.
// IT IS SHOWN AS A PATTERN CARRIED BY THE AIR, at the air's own velocity (flow
// mapping), so what the wind carries streams down a street at the speed the air moves there, bends
// round a corner and curls in the lee of a house. In the air it is a sheen of streaks (white in snow,
// grey in rain, dark in ash); on the ground it is what the gust does to the surface: wavelets that
// rough up the wet film and the puddles, and snow scoured into streamers.
float2 GustAlong(){float2 w=weatherVelocity.xy;float s=length(w);return s>1?w/s:float2(1,0);}
// The air at a place: its velocity (xy, units a second), how much it carries (z), and 1 inside a
// building (w). The steady wind, carrying nothing, where the fluid does not reach.
float4 WindAt(float3 p){
    if(windFluidWindow.z<=0)return float4(weatherVelocity.xy,0,1);
    float2 uv=(p.xy-windFluidWindow.xy)*windFluidWindow.zw;
    if(any(uv<0)||any(uv>1))return float4(weatherVelocity.xy,0,1);
    float4 air=windFluid.SampleLevel(windSampler,uv,0);
    // AGAINST A WALL. The fluid's wall cells are a band a few cells deep (the cover volume blurs a
    // wall's foot over its own voxel), and read there the air was still and held nothing: the drift
    // and the streamers stopped in a ruled line a pace short of every wall, as if the wall were
    // thicker than it is (reported 2026-11-18). There the air is taken from the open side, the most
    // open of four reads 24 units round, so what the gust carries runs on to the wall and the wall
    // hides it, as the ground fog does.
    [branch]if(air.w>.01){
        float2 reach=24*windFluidWindow.zw;
        [unroll]for(int k=0;k<4;++k){
            float4 near=windFluid.SampleLevel(windSampler,uv+float2(k==0?1:k==1?-1:0,k==2?1:k==3?-1:0)*reach,0);
            if(near.w<air.w)air=near;
        }
    }
    return air;
}
// How much blown mist or snow that air holds, 0..1: none inside a wall, and thinning against one.
float GustCarried(float4 air){return saturate(air.z)*(1-saturate(air.w*2));}
// How gusty the air is at a place, 0..1: how far it surges over the steady wind (a front runs at
// about 1.7 x (140 + the wind) over it, the air between fronts a third of that), shaded by what the
// fluid carries there, drawn out along its flow. None inside a wall, thinning against one. The
// fronts decide where: the carried load is spread over most of the map, and taken as the amount on
// its own it made every street equally gusty, a flat grey.
float GustAmount(float4 air){
    float steady=length(weatherVelocity.xy);
    float surge=(length(air.xy)-steady)/((140+steady)*1.7);
    return smoothstep(.15,.85,surge)*(.6+.4*saturate(air.z*1.3))*(1-saturate(air.w*2));
}
// How strongly the Gusts control shows it: 0.67 and over is a full storm.
float GustShown(){return saturate(CL_gusts*1.5);}
// A PATTERN CARRIED BY THE AIR. All of it rides downwind together at the speed the gust fronts
// travel: a rigid slide, which never tears it. Where the air at a place moves otherwise (squeezed
// down a street, turned at a corner, curling in an eddy) the pattern is bent that way by flow
// mapping, two copies half a period apart cross-faded so the restart never shows, but only by a
// couple of the pattern's own widths. (Carried the whole way by the local velocity, the eddies wound
// the pattern into marbled stripes across the floor.) `scale`: its size along the steady wind,
// across it and in height, in units. Returns 0..1.
float GustFlow(float3 p,float2 velocity,float3 scale,float period){
    float2 d=GustAlong(),n=float2(-d.y,d.x);
    const float fronts=220+1.5*length(weatherVelocity.xy);
    float2 rest=velocity-d*fronts;
    float restSpeed=length(rest);
    float2 bend=restSpeed>1e-3?rest*(min(restSpeed*period,scale.y*2.5)/restSpeed):float2(0,0);
    float t=climateClock.x/period;
    float a=frac(t),b=frac(t+.5),wa=1-abs(2*a-1);
    float ca=floor(t),cb=floor(t+.5),slide=climateClock.x*fronts;
    float2 pa=p.xy-bend*a,pb=p.xy-bend*b;
    float na=Noise3(float3((dot(pa,d)-slide)/scale.x+ca*7.31,dot(pa,n)/scale.y+ca*3.17,p.z/scale.z));
    float nb=Noise3(float3((dot(pb,d)-slide)/scale.x+cb*7.31+13.7,dot(pb,n)/scale.y+cb*3.17+5.3,p.z/scale.z+9.1));
    // Two noises blended are flatter than one: the contrast is put back.
    return saturate(.5+(na*wa+nb*(1-wa)-.5)/sqrt(wa*wa+(1-wa)*(1-wa)));
}
// HOW FAR THE LOCAL AIR MAY DRAG A FLOW-MAPPED PATTERN: its speed off the steady wind, held to what
// moves the pattern seven tenths of its own size in a cycle. Unheld, a gust's air (hundreds of units a
// second, turning every frame) dragged the pattern hundreds of units and back as it flickered, and it
// shook instead of flowing.
float2 GustDragRest(float2 rest,float period,float cycle){
    float m=length(rest),most=period*.7/cycle;
    return m>most?rest*(most/m):rest;
}
// LOOSE SNOW OVER THE CRUST (ApplyCoatings): the billow noise the drift is torn by (GustNoiseVolume),
// read at the ground at a third of the drift's size, sliding with the steady wind and dragged by the
// air at that place (flow mapping: two copies half a cycle apart, cross-faded), so it turns down a
// street with the air and has no grain of its own. 0..1.
float GustPowder(float3 p,float2 air){
    const float period=160,cycle=2.4;
    float2 steady=weatherVelocity.xy;
    float3 at=float3(p.xy-steady*climateClock.x,p.z*.25)/period;
    float t=climateClock.x/cycle;
    float a=frac(t)-.5,b=frac(t+.5)-.5,w=abs(a*2);
    float3 drag=float3(GustDragRest(air-steady,period,cycle)*cycle,0)/period;
    return lerp(gustNoise.SampleLevel(gustNoiseSampler,at-drag*a,0),gustNoise.SampleLevel(gustNoiseSampler,at-drag*b,0),w);
}
// The wavelets a gust raises on water: cat's paws. Three trains of crests, fanned round the STEADY
// wind (straight on and about 28 and 31 degrees either side), each running at its own speed and
// strong only in its own drifting patches, so the water is broken several ways at once and the
// pattern changes as the gust moves over it. (One train straight across the wind read as flat ruled
// lines.) The directions are fixed: the crests were first turned with the local air, and since a
// crest's place is measured from the map's origin thousands of units off, the gust's every flicker
// of direction swept them across the street each frame and the water shook (reported 2026-11-18).
// Crests some 14 to 22 units apart: at 4 to 10 they read as a fine fizz. They are the water's; the
// gust only raises them where it runs (gustRough). Returns how the surface slopes across x and y.
float2 GustRipples(float3 p){
    float2 base=GustAlong();
    static const float3 kTrains[3]={float3(0,16,.55),float3(-.49,22,.4),float3(.54,13,.35)}; // turn (rad), size, share
    float2 total=0;
    [unroll]for(int k=0;k<3;++k){
        float c=cos(kTrains[k].x),s=sin(kTrains[k].x);
        float2 d=float2(base.x*c-base.y*s,base.x*s+base.y*c),n=float2(-d.y,d.x);
        float2 q=p.xy-d*(climateClock.x*(26+10*k));
        float2 slope;Noise3Slope(float3(dot(q,d)/kTrains[k].y,dot(q,n)/(kTrains[k].y*2.2),climateClock.x*(.45+.15*k)+k*5.3),slope);
        float patch=smoothstep(.3,.75,Noise3(float3((p.xy-base*climateClock.x*40)/(220+80*k),k*3.7+climateClock.x*.08)));
        total+=(d*slope.x+n*(slope.y*.45))*kTrains[k].z*patch;
    }
    return total*1.3;
}

float3 TrueSunDirection() {
    float a=radians(CL_sunAzimuth),e=radians(CL_sunElevation);
    return float3(cos(e)*cos(a),cos(e)*sin(a),sin(e));
}
float3 MoonDirection() {
    float a=radians(CL_moonAzimuth),e=radians(CL_moonElevation);
    return float3(cos(e)*cos(a),cos(e)*sin(a),sin(e));
}
// THE KEY LIGHT. The map is lit and shadowed by the sun while it is up and by the moon once it has
// set; at the hand-over both are dark, so the change of direction never shows. SunDirection() and
// SunColor() are the key light everywhere; the sky draws the sun itself (TrueSun*) and the moon.
// C++ twin: ClimateMoonKey / ClimateMoonPower in EnvironmentClimateMath.h.
bool MoonKey() { return CL_sunElevation<0&&CL_moon>0; }
float MoonPower() {
    return max(CL_moon,0)*(.5-.5*cos(CL_moonPhase*6.2831853))*smoothstep(0,1,-CL_sunElevation/6)*
           smoothstep(0,1,CL_moonElevation)*.1;
}
float3 SunDirection() { return MoonKey()?MoonDirection():TrueSunDirection(); }
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
float3 TrueSunColor() { return lerp(float3(1,.36,.12),float3(1,.94,.83),saturate((CL_sunElevation+4)/30))*SunTint()*CL_sunPower*smoothstep(0,1,CL_sunElevation); }
float3 SunColor() { return MoonKey()?float3(CL_moonColorR,CL_moonColorG,CL_moonColorB)*MoonPower():TrueSunColor(); }
)HLSL"
#include "EnvironmentSkyRadianceShaders.h"
R"HLSL(
SkyLights ClimateSkyLights(float sunVisibility) {
    SkyLights l;
    l.sun=TrueSunDirection();l.sunTint=SunTint();l.sunlight=TrueSunColor();
    l.sunElevation=CL_sunElevation;l.haze=CL_haze;l.stars=CL_stars;l.visibility=sunVisibility;
    l.moon=MoonDirection();l.moonRadius=radians(CL_moonSize*.5);
    l.moonTint=float3(CL_moonColorR,CL_moonColorG,CL_moonColorB);l.moonBrightness=CL_moon;l.moonPhase=CL_moonPhase;
    return l;
}
// The Environment sky toward a direction. Reflections and the light the fogs and clouds take from
// the sky use the half-step march; the backdrop itself passes fast=false.
float3 SkyColor(float3 ray,float sunVisibility=1,bool fast=true) {
    float3 color=SkyRadiance(ray,ClimateSkyLights(sunVisibility),fast);
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
// The body's parts between its bones (actorBones), as kBodyParts in EnvironmentFootprintMath.h: from
// bone, to bone, how thick. Part 3 is the head, a ball a little above its bone (BodyPartEnds).
static const float3 kBodyPartsHlsl[16]={
    float3(0,1,6.5),float3(1,2,7),float3(2,3,5.5),float3(3,3,5),
    float3(2,4,5),float3(4,5,3.2),float3(5,6,3),float3(2,7,5),
    float3(7,8,3.2),float3(8,9,3),float3(0,10,5.5),float3(10,11,4.2),
    float3(11,12,3.4),float3(0,13,5.5),float3(13,14,4.2),float3(14,15,3.4)};
// THE BODY AS DRAWN THIS FRAME, from its bones, living or dead. Part of the test below: a point within a part's
// thickness plus a margin for clothes and gear (a little more far away, where a pixel is wider) is
// the player. It follows the pose exactly, whatever the player mask did; with the mask alone, bots
// came out snowed and wet over their shoulders and arms (reported 2026-11-17). The ground round the
// boots is ground: level surface at the height of the ankles is left to the coatings.
bool BonesClaim(float3 p,float3 geometric){
    if(coatingActorCount.w<.5)return false;
    float margin=4+.004*length(p-camera.xyz);
    // Every body with bones: the living (as coatingActors) and then the dead.
    int count=(int)coatingActorCount.w;
    [loop]for(int i=0;i<count;++i){
        int base=i*16;
        if(actorBones[base].w<.5)continue;
        float3 fromPelvis=p-actorBones[base].xyz;
        if(dot(fromPelvis,fromPelvis)>4900)continue;
        float feet=min(actorBones[base+12].w>.5?actorBones[base+12].z:1e9,actorBones[base+15].w>.5?actorBones[base+15].z:1e9);
        if(geometric.z>.6&&p.z<feet+3)continue;
        [loop]for(int k=0;k<16;++k){
            float3 part=kBodyPartsHlsl[k];
            float4 a=actorBones[base+(int)part.x],b=actorBones[base+(int)part.y];
            if(a.w<.5||b.w<.5)continue;
            float3 from=a.xyz,to=b.xyz;
            if(k==3){float3 up=from-actorBones[base+2].xyz;from+=(dot(up,up)>1?normalize(up):float3(0,0,1))*3.5;to=from;}
            float3 ab=to-from;float t=saturate(dot(p-from,ab)/max(dot(ab,ab),1e-4));
            if(length(p-from-ab*t)<part.z+margin)return true;
        }
    }
    return false;
}
bool CoatingActor(float3 p,float3 geometric) {
    if(PlayerMaskClaims(p)||BonesClaim(p,geometric)) return true;
    // THE BODY THE CAMERA IS IN. Looked down at in first person, the legs the game draws are not
    // posed as the body in the mask is (they are set back so they can be seen), so parts of them
    // fell outside it and were painted as snowed ground: white patches on the thighs and boots.
    // Nothing but the player can be inside the player's own hull above the feet, so for the pawn the
    // camera sits in, everything there is the player.
    {
        int own=(int)coatingActorCount.x;
        [loop]for(int j=0;j<own;++j){
            float3 o=coatingActors[j].xyz;
            float2 eye=abs(camera.xy-o.xy),at=abs(p.xy-o.xy);
            // But not the floor the player stands on: the drawn floor is often a unit or two above the
            // collision floor the feet are on (Mirage's Palace balcony: two), and it was taken for the
            // body, a bare 40-unit square of floor round the feet with snow all round it. Level ground
            // within a step of the feet is ground, as for any other player (kCoatingFloorBand). Except
            // what stands 3 to 8 units over the feet: that is the boots' toe caps, which are level too
            // and came out with snow on them (2026-11-18). The drawn floor is at most a couple of
            // units over the collision floor and a step up is higher than a boot.
            float over=p.z-o.z;
            bool floor=geometric.z>.9&&(over<3||(over>8&&over<18));
            if(coatingActors[j].w>0&&!floor&&max(eye.x,eye.y)<14&&camera.z>o.z+24&&camera.z<o.z+80&&max(at.x,at.y)<20&&p.z>o.z+1.5&&p.z<camera.z+8)return true;
        }
    }
    // With the mask of where players really are on screen, that is the whole answer. The columns
    // below are only for a frame without one: a column takes in whatever stands inside it, so the
    // wall, ledge or truck a player stood against lost its snow and its wet in a circle round them.
    if(coatingActorCount.y>.5) return false;
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
)HLSL"
R"HLSL(
// CLUMPS. One pattern, fixed to the map, that scattered snow and tracked-in water are both cut
// from: blobs a foot or two across with smaller ones between. 0..1, even enough that "more than
// 1 - c" covers about the share c of a floor.
float WeatherClumps(float3 world){
    float broad=saturate((Fbm(world*float3(.12,.12,.04))-.24)/.52);
    return saturate(broad*.8+Noise3(world*float3(.37,.37,.12)+7)*.2);
}
// HOW MUCH SNOW LIES HERE, 0..1, and how trodden it is.
//  - Under open sky, away from any door: all of it.
//  - Under open sky by a way indoors: trodden. People walk through it, so it is broken into packed
//    patches with bare ground between, more broken the nearer the door.
//  - Under cover: scattered. Snow is blown and carried in, so it lies in clumps, many by the opening
//    and fewer with every step in, and none where no open air reaches.
// The same clumps are used on both sides of a threshold, so the trodden snow outside runs into the
// scattered snow inside without a line.
float SnowLies(float3 cover,float3 world,out float trodden){
    float clumps=WeatherClumps(world);
    // Under cover, snow is full where the volume holds 45 % of the open air and gone under 10 % (the
    // documented contract: about 24 to 90 units in through a wide doorway). It used to start at 0.2 %
    // with a square root on top, which the spread reaches some 380 units in, so a room with an opening
    // on each side (Mirage's palace) had snow lying in patches across its whole floor.
    float inside=smoothstep(.1,.45,cover.y),walked=smoothstep(.06,.5,cover.z);
    // SNOW DRIFTS UP AGAINST A WALL rather than stopping short of it. At a wall's foot the sky map is
    // half shut off by the wall itself, and on its lee side the wind thins it further; taken as it
    // was, that left a bare strip along every wall with a cut edge (reported 2026-11-18). Half open
    // counts as open, and a doorstep is worn, not cleared.
    float open=saturate(cover.x*1.7);
    float share=lerp(inside*.85,1-.2*walked,open);
    trodden=saturate(lerp(1-inside*.6,walked,open));
    // A ragged edge in small torn flecks, with a soft rim about a hand wide rather than a cut.
    clumps+=(Noise3(world*float3(1.1,1.1,.4)+3)-.5)*.09+(Noise3(world*float3(3.3,3.3,1.1)+11)-.5)*.05;
    return share>=.995?1:smoothstep(1-share-.06,1-share+.06,clumps)*step(.004,share);
}
// Snow follows the luminance of the ground it covers, at most SNOW_MAX_LIFT times it, so night ground
// stays dark under snow (issue 135). Mirror of SnowCoverShade in EnvironmentCoatingMath.h.
float SnowCoverShade(float mapLuminance){
    return min(.03+.97*pow(saturate(mapLuminance),.28),SNOW_MAX_LIFT*max(mapLuminance,0));
}
// A lamp seen in water. Mirror of WaterLightGlint and WaterGlintExponent in EnvironmentCoatingMath.h.
float WaterFresnel(float cosine){float m=1-saturate(cosine);return .0204+.9796*m*m*m*m*m;}
float WaterLightGlint(float nDotH,float nDotL,float vDotH,float exponent){
    return nDotL>0&&nDotH>0?WaterFresnel(vDotH)*(exponent+2)/6.2831853*pow(nDotH,exponent)*nDotL*.25:0;
}
float WaterGlintExponent(float puddle,float porous,float detail){
    return lerp((28+72*detail)*(1-porous*.4),100+800*detail,saturate(puddle));
}
)HLSL"
R"HLSL(
// FOOTPRINTS: x = how deep a print is here (thin snow), y unused, z = how far down the players' feet
// have pressed deep snow (1 is to its bed), all 0..1, from the image of marks around the camera
// (EnvironmentFootprintMath.h). A mark made on another floor (its height layer is far from this
// surface's) is not this floor's.
#define FOOT_SAME_LEVEL 7. // see FootMarksNear
float3 FootMarksAt(float2 xy,float z){
    float2 g=xy/FOOT_TEXEL-.5;int2 a=(int2)floor(g);float2 f=g-a;float3 sum=0;
    [unroll]for(int k=0;k<4;++k){
        int2 o=int2(k&1,k>>1);
        float4 t=footTexture.Load(int3((a+o)&(FOOT_SIZE-1),0));
        float apart=abs(frac((z/FOOT_LAYER-t.z*255)/256+.5)-.5)*256*FOOT_LAYER;
        sum+=float3(t.x,0,t.w)*step(apart,FOOT_SAME_LEVEL)*(o.x?f.x:1-f.x)*(o.y?f.y:1-f.y);
    }
    return sum;
}
float3 FootMarks(float3 p){
    if(footInfo.x<.5)return 0;
    float2 d=abs(p.xy-camera.xy);
    float fade=1-smoothstep(.8,.95,max(d.x,d.y)/(FOOT_SIZE*.5*FOOT_TEXEL));
    return fade>0?FootMarksAt(p.xy,p.z)*fade:0;
}
// THE LIGHT ON THE SNOW. Snow hides the ground, so it cannot take its shade from the ground's own
// pixel: cobble joints, tile seams and painted lines showed through the cover as if it were a thin
// frost. It takes the scene's light from a ring of taps around the pixel instead, which keeps the
// broad picture (sun and shade, a lamp's pool, the colour of the light) and loses the texture.
// Taps on another surface (a wall behind, the sky, a different height) are left out.
// The ring is tens of pixels wide, and averaged as it stands across a sun shadow's edge it smeared
// every shadow on the snow into a broad, grainy blur; see below for how the shadow is kept sharp.
float SnowSunSide(float3 w){return WorldSunShadowQuick(w+SunDirection()*shadowInfo.y,float3(0,0,1));}
float3 SnowLight(float3 p,float3 color,float sunSeen){
    float4 h=mul(viewProjection,float4(p,1));
    if(h.w<=1e-4)return color;
    float2 pixel=(h.xy/h.w*float2(.5,-.5)+.5)*dimensions.xy;
    // The centre is the lit colour this pass worked out; the taps are the frame as it came in. Their
    // ratio carries this pass's own changes (ambient, tint) over to the taps.
    float3 here=ReadSource(pixel,0);
    float gain=clamp((dot(color,float3(.2126,.7152,.0722))+.002)/(dot(here,float3(.2126,.7152,.0722))+.002),.25,4);
    float spread=dimensions.y*.026,reach=24+.03*length(p-camera.xyz);
    // The ring of taps is turned and sized differently at every spot on the ground (a pattern fixed
    // to the map, half a unit fine). With one fixed ring every tap carried a shifted copy of the
    // ground's seams, and eight shifted copies of a tiled floor drew a plaid of stripes on the snow.
    // The centre pixel counts for little: it is the one that holds the seam itself.
    // It turns smoothly from place to place (noise, not a hash per half-unit cell): with a ring that
    // jumped at every cell edge, each cell measured the frame's shadow from a different set of taps
    // and a shadow's edge on the snow came out as a staircase of half-unit blocks.
    float turn=Noise3(p*1.6)*12.566371,size=.55+.9*Noise3(p*1.6+19.7);
    float2 turnX=float2(cos(turn),sin(turn))*size,turnY=float2(-turnX.y,turnX.x);
    // THE FRAME'S OWN SHADOW IS TAKEN OUT OF EVERY TAP AND PUT BACK ONCE, AT THIS POINT. Each tap
    // knows whether the sun reaches it (SnowSunSide); the shadowed ones are lifted to what they would
    // be in the sun, all twelve are averaged, and the result is darkened by this point's own
    // filtered sun visibility (`sunSeen`; -1 with no sun or moon maps, which leaves the plain
    // average). A shadow's edge on the snow is then as sharp as the lit world's, and every tap still
    // counts. (Leaving out the taps across the edge did sharpen it, but under a palm most taps are
    // across some edge: each half-unit patch averaged a different two or three and the shade came
    // out speckled.) `shadow` is how dark the frame's shadow is here, shadowed over lit, measured
    // from the taps when both kinds are among them.
    const float3 kLuma=float3(.2126,.7152,.0722);
    float3 tap[12];float seen[12],kept[12];
    float lit=0,litWeight=0,shade=0,shadeWeight=0;
    [unroll]for(int i=0;i<12;++i){
        tap[i]=0;seen[i]=1;kept[i]=0;
        float2 q=pixel+(turnX*kSoftTaps[i].x+turnY*kSoftTaps[i].y)*spread;
        if(any(q<0)||any(q>=dimensions.xy))continue;
        float d=ReadSampleDepth(q,0);
        if(IsSky(d))continue;
        float3 w=World(q,d);
        float keep=saturate(2-abs(w.z-p.z)/reach)*saturate(3-length(w-p)/(reach*4));
        [branch]if(keep<=0)continue;
        tap[i]=ReadSource(q,0)*gain;kept[i]=keep;
        [branch]if(sunSeen>=0)seen[i]=SnowSunSide(w);
        float l=dot(tap[i],kLuma),inSun=keep*saturate(seen[i]*5-4),inShade=keep*saturate(1-seen[i]*5);
        lit+=l*inSun;litWeight+=inSun;shade+=l*inShade;shadeWeight+=inShade;
    }
    // Snow in shade is never near black: the sky and the sunlit snow round it light it. Measured from
    // the frame, a corner's own occlusion counted as shadow and the shade under a plank went almost
    // black, down to an eighth of the sunlit snow; a third is the least it takes.
    float shadow=litWeight>.5&&shadeWeight>.5?clamp((shade/shadeWeight)/max(lit/litWeight,1e-4),.33,1):.45;
    float own=sunSeen>=0?lerp(shadow,1,sunSeen):1;
    float3 sum=color/own*.25;float weight=.25;
    [unroll]for(int j=0;j<12;++j){sum+=tap[j]/lerp(shadow,1,seen[j])*kept[j];weight+=kept[j];}
    return sum/weight*own;
}
)HLSL"
R"HLSL(
// DEEP SNOW TAKES EVERY FOOT. The ground the game drew is the BED of the snow; its surface lies
// `deep` units above it, and this is the height of that surface over the bed. Where nobody has
// walked it is level. Where a foot has been it is no higher than the foot's sole went (the image
// keeps how far down each texel was pressed), over a packed layer a fifth as deep where it was
// trodden right down. So a foot on the ground stands in a hole, a foot that swung low ploughed a
// groove, and holes close together are one hole.
float SnowHeight(float3 marks,float deep){
    return max(min(deep,(1-marks.z)*SNOW_TOP+.2*deep*marks.z)-min(deep,1.4)*marks.x,0);
}
// THE SNOW DOES NOT LIE AS ONE FLAT SHEET. It lies in mounds and hollows: broad swells several
// paces across, lumps a pace or two across on them and small ones on those, fixed to the map. This
// is how much of its depth lies at a spot, about 0.3 to SNOW_RELIEF_MOST. The surface really is that shape: the view ray is walked
// against it, so a mound hides what is behind its crest, a foot sinks deeper in a mound than in a
// hollow, and its slopes take the light.
// GLISTEN. Snow glitters because its top is a litter of ice crystals, each a tiny mirror: the few
// that happen to lie halfway between the eye and the light flash, and a different few flash as
// either moves. Drawn the way games do it (a stationary jittered grid of sparkles, after Shopf and
// Wang & Bowles 2016, "A Robust and Flexible Real-Time Sparkle Effect"): the ground is divided
// into cells fixed to the map, each holding one crystal at a random spot with a random tilt. A
// crystal is a ROUND point about a pixel across, never a whole cell (whole cells flashing showed
// as squares), and it flashes by how nearly its tilt lines up between eye and light, plus a
// fainter flash straight back at the eye so overcast and lamplit snow still twinkles. The cells
// are sized from how much ground a pixel covers, in doubling steps blended into each other, so
// none flickers smaller than a pixel.
// GLINTS ARE SMALL AND NEAR. Sized from the pixel alone, there were as many on screen across a far
// yard as at the feet, and a glint one pixel wide a thousand units off is a crystal the size of a
// plate: the far snow was strewn with big bright dots. A crystal's cell is never more than
// SNOW_GLINT_CELL across however far away it is, and the glints fade out with distance
// (SNOW_GLINT_NEAR to SNOW_GLINT_FAR from the eye), before a cell that size is too small to draw.
#define SNOW_GLINT_CELL 2.
#define SNOW_GLINT_NEAR 220.
#define SNOW_GLINT_FAR 700.
float SnowGlints(float3 p,float3 hv,float3 view,float footprint,float size){
    float2 u=float2(p.x*.83+p.y*.56,p.y*.83-p.x*.56)/size;
    float2 cell=floor(u);
    float3 key=float3(cell,size*37.1);
    float2 spot=.22+.56*float2(Hash31(key),Hash31(key+11.3));
    float3 tilt=normalize(float3(Hash31(key+23.7)-.5,Hash31(key+37.9)-.5,.25+.45*Hash31(key+51.1)));
    float wide=clamp(max(footprint*1.3,.05)/size,.02,.22);
    float point_=1-smoothstep(wide*.35,wide,length(u-cell-spot));
    return point_*(pow(saturate(dot(tilt,hv)),90)+.3*pow(saturate(dot(tilt,view)),220));
}
float SnowGlisten(float3 p,float3 hv,float3 view,float footprint){
    float near=1-smoothstep(SNOW_GLINT_NEAR,SNOW_GLINT_FAR,length(p-camera.xyz));
    [branch]if(near<=0)return 0;
    float level=clamp(log2(max(footprint,1e-3)*9/.25),0,log2(SNOW_GLINT_CELL/.25));
    float size=.25*exp2(floor(level));
    return lerp(SnowGlints(p,hv,view,footprint,size),SnowGlints(p,hv,view,footprint,min(size*2,SNOW_GLINT_CELL)),frac(level))*5*near;
}
// Value noise over the ground plane: four corners a look-up, where Noise3 reads eight.
float Noise2(float2 p){
    float2 a=floor(p),f=frac(p);f=f*f*(3-2*f);
    float4 h=frac(sin(float4(dot(a,float2(127.1,311.7)),dot(a+float2(1,0),float2(127.1,311.7)),dot(a+float2(0,1),float2(127.1,311.7)),dot(a+1,float2(127.1,311.7))))*43758.5453);
    return lerp(lerp(h.x,h.y,f.x),lerp(h.z,h.w,f.x),f.y);
}
// fBm over the ground plane with each octave rotated (iquilezles.org/articles/fbm). 0..1, mean .5.
float SnowFbm(float2 p,int octaves){
    float f=0,a=.5,total=0;
    [loop]for(int i=0;i<octaves;++i){f+=a*Noise2(p);total+=a;p=mul(float2x2(.8,.6,-.6,.8),p)*2.03+17.1;a*=.5;}
    return f/total;
}
// Summed value noise bunches round the middle; stretched to -1..1 with about the spread a product
// of two sines has, so the mounds keep their old height.
float SnowSigned(float n){return clamp((n-.5)*3.2,-1,1);}
float SnowRelief(float2 xy){
    // The mounds are waves, and waves repeat: the same ripple ran in rows across a yard. So the
    // ground they are laid on is first pushed about by slow noise, up to a few mounds' width and a
    // different way everywhere, and the same noise makes the lumps strong in one place and faint
    // in the next. No two stretches of a yard line up.
    // (The push is gentle for its size: one that moves the ground further than about half the
    // distance it changes over folds the waves onto themselves, which drew fine rings everywhere.)
    float2 push=float2(Noise2(xy/610),Noise2(xy.yx/530+31));
    float2 fine=float2(Noise2(xy/170+7),Noise2(xy.yx/190+19));
    xy+=(push-.5)*260+(fine-.5)*60;
    // NOISE, NOT WAVES. The mounds were products of sines, and however the ground was pushed about
    // under them a sine still repeats: from above, the small ripples ran in parallel rows every
    // twenty-odd units, and the whole yard read as one tile laid over and over. They are fBm now
    // (value noise summed over octaves, each octave turned against the last so no grid lines up;
    // after Inigo Quilez, "fBM", iquilezles.org/articles/fbm), at the same three sizes and with the
    // same spread as the waves had.
    float broad=SnowSigned(SnowFbm(xy/150,2));
    float lumps=SnowSigned(SnowFbm(xy/38+float2(5.2,1.3)+broad*.6,3));
    float small=SnowSigned(Noise2(mul(float2x2(.8,.6,-.6,.8),xy)/11+13.7+lumps));
    return min(.85+.25*broad+.2*lumps*(.35+1.3*fine.x)+.08*small*(.4+1.2*fine.y),SNOW_RELIEF_MOST);
}
// The marks at a point: one smoothed read (the sampler wraps and filters), with the height layer
// taken from the nearest texel, which must not be blended. No early exit: a lookup that returns
// early inside a branch sends the shader compiler into a stack overflow.
// A mark belongs to the level it was made on. One made in the street is not drawn on the kerb beside
// it, nor the other way: with marks shared across 28 units of height, a trampled patch ran up over a
// kerb as a slab hanging in the air, and its hollow was traced from the pavement as if the street's
// holes lay under it. Seven units takes in a slope under one boot and leaves out a kerb.
float3 FootMarksNear(float3 p){
    // The four texels round the point, each counted only if it was marked on this level, and blended.
    // (With the level taken from the nearest texel alone and the marks blended across all four, a
    // stair's treads, whose texels belong to one step or the next by turns, were drawn in blocks.)
    float2 uv=p.xy/(FOOT_TEXEL*FOOT_SIZE);
    float4 layers=footTexture.GatherBlue(footSampler,uv);
    float4 same=step(abs(frac((p.z/FOOT_LAYER-layers*255)/256+.5)-.5)*256*FOOT_LAYER,FOOT_SAME_LEVEL);
    float2 f=frac(uv*FOOT_SIZE-.5);
    float4 share=float4((1-f.x)*f.y,f.x*f.y,f.x*(1-f.y),(1-f.x)*(1-f.y))*same;
    float3 m=float3(dot(share,footTexture.GatherRed(footSampler,uv)),0,dot(share,footTexture.GatherAlpha(footSampler,uv)));
    float2 d=abs(p.xy-camera.xy);
    // The image of marks reaches 768 units from the camera. They fade out over the outer third of
    // that, a long way, where they used to go in the last fifty units, which read as a line.
    return m*(1-smoothstep(.66,.97,max(d.x,d.y)/(FOOT_SIZE*.5*FOOT_TEXEL)));
}
// THE MARKS, ROUND. A texel of the image is three quarters of a unit, so a pace from the eye it is
// drawn ten pixels wide, and the four-texel blend above, cut sharp by a print's edge, its rim and the
// melt line, drew every print and every bare patch as a stack of little squares. This reads the
// sixteen texels round the point with cubic B-spline weights (smooth across texels, so an edge cut
// from it is a curve; after Sigg and Hadwiger, "Fast Third-Order Texture Filtering", GPU Gems 2,
// ch. 20), each counted only if it was marked on this level as above. x/z as FootMarksNear; y is the
// melt, kept from marks within three times the level gap (as SnowMelted always took it).
// Four times the reads of FootMarksNear: the march into a hole keeps the cheap one, and only what is
// drawn from the point it found reads this.
float3 FootMarksRound(float3 p,out float melt){
    float2 g=p.xy/FOOT_TEXEL-.5,i=floor(g),t=g-i,s=1-t;
    float2 w0=s*s*s/6,w3=t*t*t/6,w1=(3*t*t*t-6*t*t+4)/6,w2=1-w0-w1-w3;
    float3 m=0;melt=0;
    [unroll]for(int k=0;k<4;++k){
        int qx=k&1,qy=k>>1;
        float2 uv=(i+float2(qx,qy)*2)/FOOT_SIZE;
        float u0=qx?w2.x:w0.x,u1=qx?w3.x:w1.x,v0=qy?w2.y:w0.y,v1=qy?w3.y:w1.y;
        float4 weight=float4(u0*v1,u1*v1,u1*v0,u0*v0);
        float4 apart=abs(frac((p.z/FOOT_LAYER-footTexture.GatherBlue(footSampler,uv)*255)/256+.5)-.5)*256*FOOT_LAYER;
        float4 share=weight*step(apart,FOOT_SAME_LEVEL);
        m+=float3(dot(share,footTexture.GatherRed(footSampler,uv)),0,dot(share,footTexture.GatherAlpha(footSampler,uv)));
        melt+=dot(weight*step(apart,FOOT_SAME_LEVEL*3),footTexture.GatherGreen(footSampler,uv));
    }
    float2 d=abs(p.xy-camera.xy);
    float fade=1-smoothstep(.66,.97,max(d.x,d.y)/(FOOT_SIZE*.5*FOOT_TEXEL));
    melt*=fade;
    return m*fade;
}
// MELTED: how far a fire or a blast has cleared the snow at a ground point, 0..1 (the image's second
// channel; EnvironmentFootprints.cpp), read round like the marks.
float SnowMelted(float3 p){float melt;FootMarksRound(p,melt);return melt;}
float3 FootMarksSmooth(float3 p){float melt;return FootMarksRound(p,melt);}
)HLSL"
R"HLSL(
// What the eye meets looking at ground point p: the view ray is walked down through the snow from
// its level top until it goes under the surface, so the near wall of a hole hides its floor and the
// far wall stands up behind it. Everything is placed where it is in the world: a hole's floor is
// on the ground where the foot stood and its mouth is `deep` above that, exactly over it. `met` is
// the height at which the ray met the snow (ApplyCoatings hides a player's leg behind the snow
// with it); `marks` and `height` are the snow there, `slope` how its surface leans, and `shadow`
// how far the hole's own wall cuts the sun off. The snow pushed out of a hole is heaped round its
// mouth (the rim): pressed snow nearby, none here.
void SnowTrench(float3 p,float3 view,float3 sun,float deep,float2 tilt,out float3 marks,out float height,out float met,out float level,out float2 slope,out float shadow,out float2 where){
    // THE BED IS THE GROUND AS IT LIES, NOT A LEVEL FLOOR. `tilt` is how the ground rises a unit
    // east and north. Taken as level, the bed under a point of the ray a few paces up a slope was
    // put at the wrong height: the marks there were counted as another floor's and no hole was
    // drawn, and what was drawn stood off the ground.
    float down=max(view.z-dot(tilt,view.xy),.12),top=deep*SNOW_RELIEF_MOST;
    shadow=0;met=0;
    {
        // WHERE THE RAY MEETS THE SNOW AS IT LAY, EXACTLY. The untrodden surface is a formula (its
        // mounds), so the height at which this ray enters it is found to a twentieth of a unit:
        // six even heights, then five halvings. Above that there is only air (marks can only lower
        // the surface), and if the snow at that very point has not been pressed, that is what the
        // eye meets. This is what draws the near lip of a furrow, and it must be exact: a ray that
        // clips the top corner of the lip is inside the snow for less than any sample spacing, so
        // a walk in steps skipped it by an amount that crept and snapped back along the furrow,
        // and the lip was drawn as a staircase (first with sixteen even heights, three units apart
        // along the ground at a low camera; then with samples 1.1 apart, which only made the
        // stairs smaller).
        // (These short searches are kept as loops, [loop], not copied out step by step: copied out,
        // the mound formula was compiled some thirty times over in every program that draws snow,
        // and that was a good part of the minutes the shader bundle takes to build.)
        float air=top,snowed=0,entered=0;
        [loop]for(int e=1;e<=6;++e){
            float h=top*(1-e/6.);
            float inside=step(h,deep*SnowRelief(p.xy+view.xy*(h/down)))*(1-entered);
            snowed=lerp(snowed,h,inside);
            entered=max(entered,inside);
            air=lerp(h,air,entered);
        }
        [loop]for(int b=0;b<5;++b){
            float mid=.5*(air+snowed);
            float inside=step(mid,deep*SnowRelief(p.xy+view.xy*(mid/down)));
            snowed=lerp(snowed,mid,inside);air=lerp(mid,air,inside);
        }
        float entry=.5*(air+snowed);
        // NOTHING MARKED NEAR HERE: one read of a coarse copy of the image (a texel of it is twelve
        // units, the average of 256) says whether any foot has been within reach of this point. If
        // none has, the untrodden surface at the entry is what the eye meets and there is nothing
        // else to work out: no walk, no rim, no wall's shadow. That is nearly all the snow on
        // screen, and it used to pay for twenty-odd reads of the image for nothing.
        float2 landing=p.xy+view.xy*(entry/down);
        float4 nearby=footTexture.SampleLevel(footSampler,landing/(FOOT_TEXEL*FOOT_SIZE),4);
        [branch]if(nearby.x+nearby.w<=0){
            // UNTRODDEN SNOW IS DRAWN ON THE GROUND WHERE IT IS SEEN. Its grain and lumps used to be
            // looked up where the ray met the raised surface, some way nearer the eye, while its edges,
            // the shade on it and everything standing in it stayed at the ground: two layers that slid
            // over each other as the eye moved, like a pattern under a sheet of glass. Only a hole
            // needs its depth.
            met=entry;level=deep*SnowRelief(p.xy);marks=0;height=level;where=p.xy;
            slope=deep*float2(SnowRelief(p.xy-float2(5,0))-SnowRelief(p.xy+float2(5,0)),SnowRelief(p.xy-float2(0,5))-SnowRelief(p.xy+float2(0,5)))*.1;
        }else{
        // Only a ray that enters over pressed snow goes on, down into the hole: samples at most
        // 1.1 units apart along the ground, whatever the camera's angle, until it is under the
        // surface. The first sample is the entry itself, taken as met if the snow there is within
        // a seventh of a unit of where it lay.
        float along=length(view.xy)/down;
        float drop=1.1/max(along,.05);
        int count=(int)min(ceil(entry/drop)+1,48);
        drop=max(drop,entry/47.);
        float above=entry,below=0,h=entry,last=entry,slack=.15;
        [loop]for(int i=0;i<count;++i){
            float2 at=p.xy+view.xy*(h/down);
            if(SnowHeight(FootMarksNear(float3(at,p.z+dot(tilt,at-p.xy))),deep*SnowRelief(at))>=h-slack){above=last;below=h;break;}
            above=h;last=h;h=max(h-drop,0);slack=0;
        }
        // Three halvings between the last sample in the air and the first in the snow.
        [loop]for(int k=0;k<3;++k){
            float mid=.5*(above+below);
            float2 at=p.xy+view.xy*(mid/down);
            float under=step(0,SnowHeight(FootMarksNear(float3(at,p.z+dot(tilt,at-p.xy))),deep*SnowRelief(at))-mid);
            below=lerp(below,mid,under);above=lerp(mid,above,under);
        }
        met=.5*(above+below);
    float3 q=float3(p.xy+view.xy*(met/down),p.z);q.z+=dot(tilt,q.xy-p.xy);
    where=q.xy;
    level=deep*SnowRelief(q.xy);
    marks=FootMarksSmooth(q);height=SnowHeight(marks,level);
    float3 east=FootMarksSmooth(q+float3(1.6,0,0)),west=FootMarksSmooth(q-float3(1.6,0,0)),north=FootMarksSmooth(q+float3(0,1.6,0)),south=FootMarksSmooth(q-float3(0,1.6,0));
    slope=clamp(float2(SnowHeight(west,level)-SnowHeight(east,level),SnowHeight(south,level)-SnowHeight(north,level))/3.2,-3,3);
    // The lean of the mound itself, where nobody has trodden it down.
    // (At the ground point, as the untrodden case above takes it: taken where the ray met the snow,
    // the two cases shaded the same mound differently, and the square patches of ground round every
    // print that take this path showed as squares.)
    slope+=deep*float2(SnowRelief(p.xy-float2(5,0))-SnowRelief(p.xy+float2(5,0)),SnowRelief(p.xy-float2(0,5))-SnowRelief(p.xy+float2(0,5)))*.1*(1-marks.z);
    float around=.25*(FootMarksSmooth(q+float3(3.4,3.4,0)).z+FootMarksSmooth(q+float3(-3.4,3.4,0)).z+FootMarksSmooth(q+float3(3.4,-3.4,0)).z+FootMarksSmooth(q-float3(3.4,3.4,0)).z);
    float rim=smoothstep(.02,.45,max(around,.25*(east.z+west.z+north.z+south.z))-marks.z)*(1-marks.z);
    height+=.1*deep*rim;
    slope+=float2(east.z-west.z,north.z-south.z)*rim*deep*.05;
    {
        float flat=max(length(sun.xy),.05);
        float2 toSun=sun.xy/flat;float rise=max(sun.z,.05)/flat;
        [unroll]for(int n=1;n<=4;++n){
            float reach=(min(deep*.4,2.2)+.3)*n;
            float wall=SnowHeight(FootMarksSmooth(q+float3(toSun*reach,0)),level);
            shadow=max(shadow,saturate((wall-height-reach*rise)/(deep*.3+.2)));
        }
    }
    // UNTOUCHED SNOW IN A MARKED TILE IS UNTOUCHED SNOW. This branch runs over the whole coarse tile
    // (twelve units) round any print, and away from the marks it still found the snow as it lay by
    // another route than the branch above: looked up where the ray met it, its mounds cut off at
    // SNOW_TOP, and the bed check on. Each of those shaded level snow a little differently, so every
    // print sat in a lighter, flatter square. Where nothing has touched the snow (no print, no
    // pressing, no heaped rim, no hole's shade) the result is now exactly the untrodden one.
    float touched=saturate(max(max(marks.x,marks.z),rim)*12+shadow*6);
    float flatLevel=deep*SnowRelief(p.xy);
    float2 flatSlope=deep*float2(SnowRelief(p.xy-float2(5,0))-SnowRelief(p.xy+float2(5,0)),SnowRelief(p.xy-float2(0,5))-SnowRelief(p.xy+float2(0,5)))*.1;
    where=lerp(p.xy,where,touched);level=lerp(flatLevel,level,touched);height=lerp(flatLevel,height,touched);
    slope=lerp(flatSlope,slope,touched);met=lerp(entry,met,touched);
        }
    }
}
// The ground a player stands on, for a point on that player: the height of the nearest pawn's feet.
// The point must be on that pawn's own legs, from its feet up: a pawn overhead (one standing on
// another's head, or jumping over) is not the one a chest under its feet belongs to, and taking its
// feet for the ground painted that chest as snow. A pawn in the air (coatingActorCount.z, a bit a
// pawn) stands on nothing: its boots are not in the snow.
bool ActorGround(float3 p,out float ground){
    ground=0;float best=1e9;int count=(int)coatingActorCount.x;uint airborne=(uint)coatingActorCount.z;
    [loop]for(int i=0;i<count;++i){
        float2 o=p.xy-coatingActors[i].xy;float d=dot(o,o),above=p.z-coatingActors[i].z;
        if(coatingActors[i].w>0&&d<best&&above>-6&&above<40&&((airborne>>(uint)i)&1u)==0){best=d;ground=coatingActors[i].z;}
    }
    return best<1600;
}
)HLSL"
R"HLSL(
// IS THE GROUND REALLY THERE? A hole is drawn at the bed under the point where the ray met the snow,
// which is not this pixel's ground point; it is taken to be the same floor carried on. At a step, a
// kerb or a drop it is not: the hole was drawn hanging in the air past the edge, or seen through
// the step. So the bed point is looked up on screen. If what the game drew there is another height
// (and is not something far in front, the gun, or a player standing in the hole) the depth is not
// drawn and the snow there is level. 1 = trust, 0 = do not.
float SnowBedTrust(float3 bed){
    float4 h=mul(viewProjection,float4(bed,1));
    float2 px=(h.xy/max(h.w,1e-4)*float2(.5,-.5)+.5)*dimensions.xy;
    bool on=h.w>1e-4&&all(px>=0)&&all(px<dimensions.xy);
    px=clamp(px,0,dimensions.xy-1);
    float d=ReadSampleDepth(px,0);
    float3 w=World(px,d);
    float off=smoothstep(2.5,6,abs(w.z-bed.z))*(1-smoothstep(16,32,abs(length(w-camera.xyz)-length(bed-camera.xyz))));
    off=IsSky(d)?1:off;
    [branch]if(on&&off>0){if(IsViewmodelSample(px,0)||PlayerMaskClaims(w))off=0;}
    return on?1-off:1;
}
// HOW WET OPEN GROUND IS where the sky reaches it only in part (the edge of a roof's shelter, the
// sheltered side of a wall in a wind): the share is turned into patches, many where most of the rain
// arrives and few where little does, the same clumps as everywhere else. Used as it was, a plain
// amount, the wet ended along the roof's outline in a ruled line, and the sheltered side of a wall
// was a dry rectangle.
float WetOpen(float open,float clumps){
    return open>=.995?1:smoothstep(1-open-.12,1-open+.12,clumps)*step(.004,open);
}
// STANDING WATER at a point on level ground, 0..1: the same pools ApplyCoatings draws, for the
// things that need to know whether a foot came down in one. Leaves snow out (snow has no pools).
float PuddleAt(float3 p){
    bool raining=weatherParams.z==1&&weatherColor.w>0;
    float local=CL_surfaceRadius>0?1-smoothstep(CL_surfaceRadius*.8,CL_surfaceRadius,length(p.xy-float2(CL_surfaceX,CL_surfaceY))):1;
    float3 cover=WeatherCover(p,float3(0,0,1));
    float clumps=WeatherClumps(p+float3(31,17,0));
    float carried=sqrt(smoothstep(.002,.3,cover.y))*(1-.3*smoothstep(.06,.5,cover.z)*(1-cover.x));
    float reached=max(WetOpen(cover.x,clumps),smoothstep(1-carried-.1,1-carried+.1,clumps)*step(.004,carried));
    float exposure=raining&&shelterInfo.x>=0?reached*saturate(weatherColor.w*2):0;
    float puddle=0;
    if(CL_puddles>0){
        float basin=Fbm(p*CL_maskScale)-(.8-.62*saturate(CL_puddles));
        float heightBand=CL_surfaceRadius>0?saturate(1-abs(p.z-CL_puddleHeight)/max(CL_puddleDepth,1)):1;
        puddle=smoothstep(-.02,.13,basin)*local*heightBand;
        if(raining)puddle*=smoothstep(.1,.45,exposure);
    }
    float pool=smoothstep(.78,.92,clumps+.25*smoothstep(.06,.5,cover.z))*local;
    return max(puddle,pool*exposure*step(.001,CL_wet+CL_puddles))*(1-saturate(CL_snow*4));
}
// CS2'S OWN SNOW (gameSnow*, EnvironmentGameTextures.h): laid top-down over the map, one repeat
// every gameTextureInfo.z units. The mip is picked from how many units a pixel covers (`footprint`,
// already stretched for a surface seen edge-on): the coatings run inside branches, where a lookup
// cannot work out its own derivatives.
float2 GameSnowUv(float3 p){return float2(p.x,-p.y)/gameTextureInfo.z;}
float GameTextureLod(float footprint,float texelsPerUnit){return max(log2(max(footprint*texelsPerUnit,1e-4)),0);}
// EVERYTHING CS2'S SNOW MATERIAL CARRIES at a ground point, as csgo_environment reads it:
//  colour (rgb) and its cavities (the colour map's alpha),
//  the normal (rg, hemi-octahedral: CS2's HemiOctAnisoRoughness) as its lean on level ground,
//  the roughness (the normal map's b: rough ridges, smooth specks that sparkle),
//  the height, which also offsets the lookup toward the eye (parallax), so the crust has depth and
//  its relief stays put on the ground as the camera moves.
// `view` points to the eye. v runs down the map's y.
struct GameSnowSample{float3 color;float cavity,rough,height;float2 lean;};
// THE REPEAT, BROKEN UP. One tile of CS2's snow laid over the whole map showed the same pattern every
// 128 units, everywhere (reported 2026-11-18). A smooth noise over the map picks, region by region (a
// couple of repeats across), one of eight offsets into the texture, and neighbouring regions blend
// into each other (Quilez's "texture repetition", technique 3), so no two stretches of snow read
// alike. Offsets only, never a turn, so the crust's normals stay right. `w`: how much of b.
void GameSnowVariant(float2 uv,out float2 a,out float2 b,out float w){
    float l=Noise3(float3(uv*.45,7.3))*8,ia=floor(l);
    a=frac(sin(float2(3,7)*ia)*43.7585);b=frac(sin(float2(3,7)*(ia+1))*43.7585);
    w=smoothstep(.2,.8,l-ia);
}
float4 GameSnowTexel(Texture2D<float4> map,float2 uv,float lod,float2 a,float2 b,float w){
    return lerp(map.SampleLevel(gameTextureSampler,uv+a,lod),map.SampleLevel(gameTextureSampler,uv+b,lod),w);
}
float GameSnowHeightAt(float3 p,float lod){
    float2 uv=GameSnowUv(p),a,b;float w;GameSnowVariant(uv,a,b,w);
    return GameSnowTexel(gameSnowHeight,uv,lod,a,b,w).r;
}
GameSnowSample GameSnowAt(float3 p,float3 view,float lod){
    GameSnowSample s;
    float2 uv=GameSnowUv(p),va,vb;float vw;
    GameSnowVariant(uv,va,vb,vw);
    float h=GameSnowTexel(gameSnowHeight,uv,lod,va,vb,vw).r;
    // About three units of depth over a repeat, as the parallax offset in world units.
    float2 shift=view.xy/max(view.z,.3)*(h-.5)*3;
    uv=GameSnowUv(p+float3(shift,0));
    s.height=GameSnowTexel(gameSnowHeight,uv,lod,va,vb,vw).r;
    float4 c=GameSnowTexel(gameSnowColor,uv,lod,va,vb,vw);
    float4 n=GameSnowTexel(gameSnowNormal,uv,lod,va,vb,vw);
    s.color=c.rgb;s.cavity=c.a;s.rough=n.b;
    float2 t=float2(n.r+n.g-1,n.r-n.g);
    float3 m=normalize(float3(t,max(1-abs(t.x)-abs(t.y),.05)));
    s.lean=float2(m.x,-m.y)/max(m.z,.2);
    return s;
}
)HLSL"
R"HLSL(
// IS A MAP LAMP IN SIGHT OF THIS WATER? Map lamps have no shadow map in this pass, so a lamp in the
// next room would shine in a puddle through the wall. The line from the water to the lamp is walked
// on screen, densest near the water: where two samples running lie well behind what the frame drew
// there, a wall or a ceiling is in the way. (One sample behind is a pole or a rail the line passes
// behind, not a wall.) What is off screen counts as clear. The walk stops short of the lamp by
// `stopShort` so the fixture's own shade does not hide its bulb.
float MapLampSeen(float3 origin,float3 lamp,float stopShort){
    float3 toward=lamp-origin;float length0=length(toward);
    float reach=max(length0-stopShort,0);
    toward/=max(length0,1e-4);
    float seen=1;int behind=0;
    [loop]for(int i=1;i<=16;++i){
        float u=i/16.;
        float4 c=mul(viewProjection,float4(origin+toward*(reach*u*u),1));
        if(c.w<=1){behind=0;continue;}
        float2 pixel=ClipPixel(c);
        if(any(pixel<0)||any(pixel>=dimensions.xy)){behind=0;continue;}
        float d=ReadSceneMarch(pixel);
        if(IsSky(d)){behind=0;continue;}
        float scene=SceneViewDepth(pixel,d);
        behind=c.w-scene>max(4,scene*.015)?behind+1:0;
        if(behind>=2){seen=0;break;}
    }
    return seen;
}
float3 ApplyCoatings(float3 color,float3 p,float3 normal,float3 coatingNormal,float mapLuminance,bool viewmodel=false) {
    // The Wetness control decides how wet surfaces get. While it rains, only ground the rain
    // actually reaches is wet; picking Rain sets a starting wetness in the UI.
    bool raining=weatherParams.z==1&&weatherColor.w>0;
    [branch] if(CL_snow<=0 && CL_wet<=0 && CL_puddles<=0) return color;
    // THE FIRST-PERSON GUN AND HANDS TAKE NO COATING. CS2's own wet shading already runs on them
    // (the world weather the Environment switches on), and the coatings are keyed to the ground:
    // the wet film drew the floor's rain ripples and its pooled darkening across the weapon.
    if(viewmodel)return color;
    float3 sun=SunDirection(),view=normalize(camera.xyz-p);
    // Players get no world coating at all. Native CS2 shading already owns player, glove and weapon
    // base wetness, and a second film there both flattened skins and emphasized capsule boundaries;
    // world snow keyed to map positions would only smear across a moving body.
    //
    // BUT DEEP SNOW STANDS IN FRONT OF THEIR LEGS. The snow's surface is above the ground the feet
    // are on, so a boot and shin lower than the snow between them and the eye must not be seen: a
    // player stands IN the snow, in the holes their feet pressed. For a pixel of a player lower
    // than the deepest snow, the ground point behind it on the same view ray takes its place: the
    // snow is worked out there as for any ground pixel, and if the ray meets the snow above the
    // height of the leg (`legHeight`), the pixel is snow. Otherwise the player's pixel is kept.
    float3 untouched=color;float legHeight=-1,buried=0;
    if(CoatingActor(p,normal)){
        float ground;
        // (Nor the player's own body, looked down at in first person: it was drawn as a slab of snow.)
        if(viewmodel||CL_snow<=0||footInfo.x<.5||view.z<.06||length(p-camera.xyz)<100||!ActorGround(p,ground))return color;
        legHeight=max(p.z-ground,0);
        if(legHeight>SNOW_TOP)return color;
        p-=view*(legHeight/view.z);
        normal=coatingNormal=float3(0,0,1);
    }
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
    // WHAT REACHES THIS SURFACE, looked up once and shared by snow and rain: the sky map and the
    // cover volume. Snow never lies indoors, falling or not. Without a sky map (an unread map) the
    // coating stays global.
    float3 cover=WeatherCover(p,coatingNormal,raining?1:.12);
    // GUSTS, read once for the snow and the wet ground below (GustFlow): how gusty the air over this
    // ground is, which way it moves, and the pattern it carries along the surface. (Read separately
    // in each, the coatings took the shader compiler over three times as long.)
    float gustAmount=0,gustRun=0,gustLoad=0;float2 gustAir=0;
    [branch]if(CL_gusts>0&&windFluidWindow.z>0&&!viewmodel){
        float4 air=WindAt(p);
        gustAmount=GustAmount(air)*GustShown();
        gustAir=air.xy;
        gustLoad=GustCarried(air);
        [branch]if(gustAmount>.01)gustRun=GustFlow(p,air.xy,float3(180,20,40),1.8);
    }
    float trodden=0,melted=0;
    float snow=0,drift=.5;
    // First-person hands and weapon get no snow either: the coating is keyed to map positions.
    if(!viewmodel){
        // DRIFTS. Broad patches collect more and others are swept thin, so the cover is not one
        // even sheet; where it collects it reads as thicker (relief below).
        drift=Fbm(p*CL_maskScale*.3+31);
        // Open ground is covered; drifts only decide where it lies thin or deep.
        float amount=saturate(CL_snow*lerp(1,1.7,drift));
        // By a door it is trodden and under cover it is scattered (SnowLies).
        float shelterFade=SnowLies(cover,p,trodden);
        // A fire melts the snow round it and a blast throws it clear; it comes back in time.
        [branch]if(footInfo.x>.5){melted=SnowMelted(p);shelterFade*=1-melted;}
        // Snow settles on anything that is not steep: a tiled roof is covered across its tiles, not
        // striped along them.
        snow=local*smoothstep(.2,.65,coatingNormal.z)*CoatingCoverage(amount,noise,.18)*shelterFade;
        // With CS2's snow, a thin patch's edge follows the crust's own relief, high ground keeping
        // its snow longest, instead of fading evenly. Whole cover stays whole.
        [branch]if(gameTextureInfo.x>.5&&snow>0&&snow<1){
            float hgt=GameSnowHeightAt(p,GameTextureLod(footprint,2048/gameTextureInfo.z));
            snow=saturate(snow+(hgt-.5)*2.4*snow*(1-snow));
        }
    }
    snowLaid=saturate(snow);
    if(snow>0) {
        // A coating rounds small upward-facing facets; preserve the map's broad lighting instead
        // of painting an unshadowed, triangle-lit white surface over it.
        float3 snowNormal=normalize(lerp(coatingNormal,float3(0,0,1),snow*.55));
        // FOOTPRINTS dent the snow: a print wherever a foot came down and, where the snow lies deep
        // (the drifts), the hole each foot sinks in it. A dent is lit as a dent: its walls
        // tilt the light and its floor is packed and in its own shade. Thin trodden snow is worn
        // through to the ground under a print.
        // Where it has drifted deep every foot goes in: a hole with walls, a packed floor and a
        // heaped rim, seen with its real depth (SnowTrench).
        float dent=0,sunk=0,trenchLight=1;
        // WHERE ON THE SNOW THIS PIXEL IS. The snow's surface stands above the ground, so the
        // point of it the eye meets is not over the ground point of this pixel: it is nearer the
        // camera, by more the lower the camera looks. Everything drawn ON the snow (its lumps, its
        // grain, its glints, the shade of its drifts) has to be looked up there. Looked up at the
        // ground point, as it was, the patterns slid over the mounds and holes as the camera
        // moved, by up to forty units.
        float3 onSnow=p;float driftSeen=drift;
        {
            float deep=max(SNOW_DEPTH*CL_snowDepth*snow*lerp(.3,1,smoothstep(.35,.7,drift))*(1-.5*trodden),.6);
            // (Kept to twice the distance it used to fade at: from a high camera the holes went
            // while they were still several pixels wide.)
            // Where a fire or a blast is thinning the snow it lies flat: its depth is drawn as if
            // the snow round a point were as deep as at the point, and across a melting edge it is
            // not, which cut the edge into teeth.
            float seen=CoatingDetailFade(footprint,.2)*(1-saturate(melted*4));
            [branch]if(footInfo.x>.5&&seen>0){
                float3 m;float height,met,full,shadow;float2 slope,where;
                float2 tilt=coatingNormal.z>.5?clamp(-coatingNormal.xy/coatingNormal.z,-.5,.5):float2(0,0);
                SnowTrench(p,view,sun,deep,tilt,m,height,met,full,slope,shadow,where);
                [branch]if(any(where!=p.xy))seen*=SnowBedTrust(float3(where,p.z+dot(tilt,where-p.xy)));
                // The grain, lumps and glints stay on the ground point everywhere, in a hole and out
                // of it. (Looked up where the ray met the snow, they slid as the eye moved wherever the
                // snow had been walked on and lay still where it had not, which showed exactly which
                // was which.)
                buried=saturate((met-legHeight+.9)/1.4);
                float3 level=snowNormal;
                snowNormal=normalize(snowNormal+float3(slope,0)*seen);
                dent=m.x*seen;
                sunk=saturate(1-height/max(full,.1))*seen;
                // Lit as a trough whatever lights the map: the wall turned to the sun is brighter than
                // level snow and the wall turned away is darker, the floor sits in the wall's shade.
                trenchLight=(1+.9*(1-.6*CL_relight)*(dot(snowNormal,sun)-dot(level,sun)))*(1-.5*shadow*seen)*(1-.3*sunk*(.8+.4*Noise3(onSnow*.9)));
                snow*=1-m.x*(.3+.45*trodden)*seen*saturate(1.5-deep*.4);
            }
        }
        // CS2'S SNOW MATERIAL (GameSnowAt): the Cobblestone winter snow, all of it. Its relief tilts
        // the normal the snow is lit by, its colour and cavities colour it, its roughness gives it a
        // sheen and decides where it sparkles. (The procedural lumps and grain it replaced are gone.)
        // Until the textures have loaded the snow is plain.
        const bool gameSnow=gameTextureInfo.x>.5;
        float3 levelSnow=snowNormal;
        GameSnowSample game=(GameSnowSample)0;
        game.color=1;game.cavity=1;game.rough=.75;game.height=.5;
        [branch]if(gameSnow){
            game=GameSnowAt(onSnow,view,GameTextureLod(footprint,2048/gameTextureInfo.z));
            snowNormal=normalize(snowNormal+float3(game.lean,0)*snow*up);
        }
        float snowLit=saturate(dot(snowNormal,sun));
        // The sun on snow is the brightest thing on screen, so its shadow's edge is drawn sharp and
        // large: with the one-texel lookup (WorldSunShadowQuick) that edge was the shadow map's own
        // grid, a staircase of squares across the snow. Snow takes the filtered lookup the lit world
        // takes, with its soft edge.
        float sunSoft=shadowInfo.x>=.5?WorldSunShadowSoft(p+sun*shadowInfo.y,snowNormal,viewmodel):1;
        float visibility=CL_contactShadows>0?sunSoft:1;
        float3 snowAlbedo=float3(.94,.97,1);
        // CS2's snow colour and cavities, as their variation around their own average (the last mip):
        // the crust, the packed patches and the blue in its hollows, at the brightness the snow is
        // calibrated to.
        [branch]if(gameSnow){
            float4 mean=gameSnowColor.SampleLevel(gameTextureSampler,float2(.5,.5),16);
            snowAlbedo*=clamp(game.color/max(mean.rgb,.05),.4,1.25)*clamp(game.cavity/max(mean.a,.05),.5,1.1);
        }
        float3 light=SnowLight(p,color,shadowInfo.x>=.5?sunSoft:-1);
        float lightLuminance=dot(max(light,0),float3(.2126,.7152,.0722));
        float inheritedShade=SnowCoverShade(lightLuminance);
        // A fifth of the light's own colour: blue in shade, warm under a lamp, never the ground's paint.
        float3 mappedSnow=snowAlbedo*lerp(float3(1,1,1),light/max(lightLuminance,1e-4),.22)*inheritedShade*1.02;
        mappedSnow*=1+SunColor()*snowLit*visibility*CL_sunRelight;
        float3 relitSnow=snowAlbedo*(CL_ambient+SunColor()*snowLit*visibility*CL_sunRelight);
        float3 snowColor=lerp(mappedSnow,relitSnow,CL_relight);
        snowColor+=snowAlbedo*(FillLights(p,snowNormal,viewmodel)+WorldBounce(p,snowNormal));
)HLSL"
R"HLSL(
        float relief=1;
        float3 hv=normalize(sun+view+float3(1e-6,0,0));
        [branch]if(gameSnow){
            // CS2's crust lit the way the map lights the snow: the sun's share follows the crust's own
            // normal against level snow, the sky's share its tilt, so it reads in sun and in shade.
            // (Through the sun relight term alone, as at first, it was too faint to see.)
            float flatSun=max(dot(levelSnow,sun),.12);
            float direct=clamp(saturate(dot(snowNormal,sun))/flatSun,0,1.7);
            float skyward=.68+.32*saturate(snowNormal.z);
            relief*=lerp(skyward,direct,visibility*saturate(sun.z*3)*.75);
        }
        // The thin edge of a patch is packed and a little grey; the deep middle is clean white.
        relief*=lerp(.86,1.03,smoothstep(.15,.9,snow)*(.5+.5*driftSeen));
        // GUSTS SCOUR THE SNOW: loose powder blown over the crust, only where the gust carries snow
        // (the fluid's load), in soft billows that follow the air there (GustPowder). Where it passes
        // the crust is a shade smoother and lighter, between it a shade greyer: a few percent, matte.
        // (As a broad light-and-dark sheet the floor read as one white pattern sliding over it; as
        // thin bright threads drawn out along the steady wind it read as shiny metal lines, all one
        // way, running on through the walls, reported 2026-11-18.)
        [branch]if(gustAmount*gustLoad>.002){
            float powder=GustPowder(onSnow,gustAir);
            relief*=1+gustAmount*smoothstep(.08,.55,gustLoad)*CoatingDetailFade(footprint,.04)*(smoothstep(.35,.75,powder)*.07-.05)*CL_gustFloor;
        }
        // Trodden snow is packed and dirty: greyer, a little warm from the ground under it, and flat.
        snowColor=lerp(snowColor,snowColor*float3(.8,.77,.74)+color*.1,trodden*.75);
        // GLISTEN (SnowGlints): points of light that come and go as the eye or the sun moves. As
        // bright as the light on the snow there: strong in sun, faint in shade, dim down a hole.
        // (The snow itself is kept a tenth under full white, or a glint would have nowhere to go:
        // drawn at full white, nothing on it could be brighter.)
        float3 sparkle=SnowGlisten(onSnow,hv,view,footprint)*float3(1,.98,.94)*(.3+1.7*lightLuminance)*max(snowLit*visibility,.3*(1-CL_relight))*saturate(trenchLight)*(1-.6*sunk);
        [branch]if(gameSnow){
            // CS2's snow sparkles where its roughness map has smooth specks and dulls on the rough
            // ridges, and it has a sheen: the sun's highlight off the crust, as broad as the crust is
            // rough (GGX, the snow's Fresnel), strongest looking toward the sun.
            sparkle*=lerp(.25,2.2,saturate((.8-game.rough)*6));
            float a=max(game.rough*game.rough,.02),a2=a*a;
            float nh=saturate(dot(snowNormal,hv)),nl=saturate(dot(snowNormal,sun));
            float dd=nh*nh*(a2-1)+1;
            float sheen=a2/(3.14159*dd*dd)*nl*(.02+.98*pow(1-saturate(dot(hv,view)),5))*4;
            sparkle+=SunColor()*sheen*visibility*(1-.6*sunk)*saturate(trenchLight);
        }
        snowColor*=lerp(float3(1,1,1),float3(.55,.61,.74),dent*.8)*clamp(trenchLight,.35,1.22)*lerp(float3(1,1,1),float3(.74,.83,1),sunk);
        color=lerp(color,snowColor*relief+sparkle,legHeight>=0?1:snow);
        // And half a step of noise per pixel, so what gradient is left does not band.
        {
            float4 clip=mul(viewProjection,float4(p,1));
            float2 pixel=(clip.xy/max(clip.w,1e-4)*float2(.5,-.5)+.5)*dimensions.xy;
            color*=1+(frac(52.9829189*frac(dot(floor(pixel),float2(.06711056,.00583715))))-.5)*.012*snow;
        }
    }
    // A pixel of a player: snow where the snow hides the leg, the player as drawn everywhere else.
    if(legHeight>=0)return lerp(untouched,color,buried*step(.001,snow));
)HLSL"
R"HLSL(
    // Only the first-person model sits this close; the film fades in just beyond it. This used to
    // be a hard 72-unit cut, so a wall or floor right in front of the camera dried out.
    float nearFade=smoothstep(18,44,length(p-camera.xyz));
    if(nearFade<=0)return color;
    // Under whole snow there is no wet ground and no pool: every term below is times (1 - snow).
    // Most snow on screen is whole, and it used to go through all of this for nothing.
    if(snow>=.999)return color;
    // RainExposure, from the shared lookup: an unprepared map cannot assert a surface is exposed.
    // RAIN REACHES IN AS PATCHES. Open ground is wet all over. Under cover the wet is what blew and
    // was walked in: the same clumps as the snow, many at the opening and fewer further in.
    float clumps=WeatherClumps(p+float3(31,17,0));
    float carried=sqrt(smoothstep(.002,.3,cover.y))*(1-.3*smoothstep(.06,.5,cover.z)*(1-cover.x));
    float reached=max(WetOpen(cover.x,clumps),smoothstep(1-carried-.1,1-carried+.1,clumps)*step(.004,carried));
    float exposure=raining&&shelterInfo.x>=0?reached*saturate(weatherColor.w*2):0;
    // MELTWATER. Where snow is trodden or scattered it also melts: the ground between the patches is
    // damp, and by a door there are small pools of it.
    float melt=0;
    if(CL_snow>0&&shelterInfo.x>0&&!viewmodel){
        float zone=lerp(smoothstep(.1,.45,cover.y)*(1-smoothstep(.75,1,cover.y)),smoothstep(.06,.5,cover.z),cover.x); // where SnowLies scatters snow
        melt=zone*saturate(CL_snow*1.5)*smoothstep(.2,.55,clumps)*up;
    }
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
    // By a way indoors the ground is worn into dips, and they hold water: more pools near a door.
    float pool=smoothstep(.78,.92,clumps+.25*smoothstep(.06,.5,cover.z))*smoothstep(.95,.995,coatingNormal.z)*local*(1-snow);
    puddle=max(puddle,pool*max(exposure*step(.001,CL_wet+CL_puddles),melt*.5));
    float wet=max(max(CL_wet*local*(raining?exposure:1),melt*.5)*(1-snow),max(puddle,damp))*nearFade;
    puddle*=nearFade;
    if(wet<=0)return color;
    // A stable world-space response estimate for maps without authored F_WETNESS.
    // Screen derivatives of geometric normals are NOT material roughness: they outline
    // every triangle seam and stair edge as a different wet material, changing with resolution.
    // This pass cannot identify material names or rewrite CS2's underlying roughness.
    float porous=saturate(.3+noise*.35);
    float fresnel=.0204+.9796*pow(1-saturate(dot(coatingNormal,view)),5);
    porous*=1-puddle;
    // WET GROUND IS DARKER AND RICHER, NOT PALER. Water in the pores swallows light: a soaked
    // surface loses a quarter to over half of its brightness and gains a little saturation. (A
    // 16 % darkening under a constant sky-coloured film read as frost.)
    float soak=wet*(.24+.38*porous)*(1-fresnel);
    color=max(lerp(dot(color,float3(.2126,.7152,.0722)).xxx,color,1+.2*wet),0)*(1-soak);
    float mapLit=viewmodelInfo.z>.5?0:1-saturate(CL_relight);
    color*=1-puddle*(.3+.2*mapLit);
    float3 wetNormal=normalize(lerp(coatingNormal,float3(0,0,1),puddle));
    // CS2'S BEADS OF WATER (gameDroplets, the texture its own wet surfaces use) stand on wet ground
    // that is not under a pool, one repeat every 32 units: they bend what the film mirrors and catch
    // the sun as points of light. They gather in a ring round each pool's edge and in scattered spots
    // of the map-fixed clumps, not over every wet surface (every wet street beaded, reported
    // 2026-11-18).
    float beadMask=0;float2 beadLean=0;
    [branch]if(gameTextureInfo.y>.5&&up>0){
        float4 bead=gameDroplets.SampleLevel(gameTextureSampler,float2(p.x,-p.y)/32,GameTextureLod(footprint,256./32));
        beadLean=(bead.rg*2-1)*float2(1,-1);
        float beadZone=max(smoothstep(.01,.2,puddle)*(1-smoothstep(.45,.85,puddle)),smoothstep(.66,.82,WeatherClumps(p))*.8);
        beadMask=saturate(bead.a*4)*saturate(wet*1.5)*(1-puddle)*beadZone*up*CoatingDetailFade(footprint,.4);
        wetNormal=normalize(wetNormal+float3(beadLean*beadMask*1.5,0));
    }
    if(exposure>0&&up>0){
        float2 q=p.xy*.065;
        float2 slope=RainImpulse(q,max(length(ddx(q)),length(ddy(q))),climateClock.x);
        wetNormal=normalize(wetNormal+float3(slope*exposure*up*CL_ripple,0));
    }
    // GUSTS OVER WATER: where a gust runs over the wet street and the
    // puddles it raises wavelets, crests across the wind, carried downwind. They break up what the
    // film mirrors, so the gust is seen as a rougher, darker patch sweeping over the water, its
    // wavelets glinting where they catch the sun.
    float gustRough=gustAmount*lerp(.35,1,smoothstep(.38,.72,gustRun))*CL_gustFloor;
    [branch]if(gustRough>.01&&up>0){
        float2 ripples=GustRipples(p)*CoatingDetailFade(footprint,.12);
        wetNormal=normalize(wetNormal+float3(ripples*gustRough*(.15+.55*puddle)*up,0));
    }
    // A STEP IN WATER sends a train of ripples out from where the foot came down. They bend what
    // the water mirrors; nothing is painted on it. Strong in a pool, slight on merely wet ground.
    [branch]if(footInfo.x>.5&&up>0){
        float2 push=0;
        [loop]for(int s=0;s<FOOT_STEPS;++s){
            float age=climateClock.x-footSteps[s].w;
            if(age<0||age>1.6||abs(p.z-footSteps[s].z)>24)continue;
            float2 d=p.xy-footSteps[s].xy;float r=length(d);
            if(r>64)continue;
            float ring=r-(3+age*30)+1.0*sin(d.x*.31+s*1.7)*sin(d.y*.27+s);
            // A landing rings the water harder than a walk (FootImpact).
            float impact=footImpact[s/4][s%4];
            push+=d/max(r,.05)*sin(ring*.5)*exp(-ring*ring*.012)*pow(1-age/1.6,3)*.3*(impact>0?.45+.55*impact:1);
        }
        wetNormal=normalize(wetNormal+float3(push*up*(.25+2.2*puddle),0));
    }
    float3 reflected=reflect(-view,wetNormal);
    // One filtered sun lookup serves both the mirrored sky and the highlight: the ripple-bent normal
    // moves the receiver offset by a fraction of a unit, not across a shadow edge.
    float sunVisibility=CL_contactShadows>0?WorldSunShadowQuick(p+sun*shadowInfo.y,coatingNormal,viewmodel):1;
    // THE MIRROR is water's own Fresnel and nothing more: next to none looking down at it, strong
    // toward the horizon. A rough porous surface breaks the film up and mirrors less. It shows the
    // scene (a short screen-space trace); where the trace finds nothing it shows the sky if the sky
    // is open above, and otherwise the surface's own colour.
    float film=saturate(max(wet*fresnel*(.45+.55*(1-porous))*(1-.5*mapLit),puddle*max(fresnel,.035+.06*mapLit)));
    film*=1-.45*gustRough;
    // How much of the mirror is sky goes by how open the sky is above, and where that is only partly
    // (a roof's edge, the sheltered side of a wall) it is turned into patches like the wet itself,
    // softer at their edges. As a plain amount the blue sheen stopped along the roof's outline in a
    // ruled line, and under a building you could see exactly where it ended.
    float mirrorsSky=cover.x>=.995?1:smoothstep(1-cover.x-.3,1-cover.x+.3,clumps)*step(.004,cover.x);
    // And a mirror shows what lies ALONG the reflected ray, not what is straight overhead. In an alley
    // with a strip of sky above, the ground mirrors the houses across the way; shown the sky there
    // instead, every wet patch under that strip was a bright blue plate. The ray is followed out
    // over the roofs' height map: where it runs in under a roof line it has met a building, and
    // the mirror keeps the ground's own tone there (the screen trace below fills in what it finds).
    [branch]if(CL_sky>0&&mirrorsSky>0&&shelterInfo.x>0){
        static const float kOut[5]={40,100,220,420,760};
        float3 along=normalize(float3(reflected.xy,max(reflected.z,.03)));
        float clear=1;
        [loop]for(int k=0;k<5;++k)clear*=SkyTap(p+along*kOut[k]);
        mirrorsSky*=clear;
    }
    float3 environment=CL_sky>0?lerp(color,SkyColor(reflected,sunVisibility),CL_sky*mirrorsSky):color;
    // OFF SCREEN: where the trace below finds nothing, CS2's own probes (or the live capture under
    // Environment lighting) instead of the surface's own colour (OffScreenReflection). With the
    // Environment's sky on, the probe gives the buildings and our sky the open sky, as above.
    [branch]if(reflectionInfo.x>0&&film>.015){
        float found;float3 probe=OffScreenReflection(p,reflected,.12+porous*.13+gustRough*.2,found);
        if(found>0)environment=CL_sky>0?lerp(probe,SkyColor(reflected,sunVisibility),CL_sky*mirrorsSky):probe;
    }
    [branch]if(film>.015)environment=WetScreenReflection(p,wetNormal,reflected,environment);
    environment=WorldReflectionRoughness(p,wetNormal,reflected,environment,.12+porous*.13+gustRough*.2);
    color=lerp(color,environment,film);
    // Rough water is darker too: it sends less of the bright sky back. A gust is seen as a darker
    // patch sweeping over the wet street, wherever the street mirrors much or little.
    color*=1-.14*gustRough*saturate(wet*1.5);
    float3 halfDir=normalize(sun+view+float3(1e-7,0,0));
    // EACH BEAD IS A LITTLE DOME OF WATER, seen whatever the light does: its rim darker (the light is
    // bent away), its crown mirroring the sky over it, and a point of sun on it from almost any angle,
    // since some part of a dome always faces halfway between the eye and the sun. Bending only the
    // film's normal did nothing to see: under the map's own sky the film mirrors the ground's own colour.
    [branch]if(beadMask>.01){
        float tilt=saturate(length(beadLean));
        float3 dome=normalize(float3(beadLean*1.3,1));
        float3 bounce=reflect(-view,dome);
        float lum=max(dot(color,float3(.2126,.7152,.0722)),.02);
        float3 skyOver=CL_sky>0?SkyColor(bounce,sunVisibility):fogAmbient.rgb*lum*2.2;
        color*=1-.4*beadMask*tilt*tilt;
        color+=skyOver*beadMask*saturate(bounce.z*1.5)*(.25+.5*(1-tilt))*mirrorsSky;
        color+=SunColor()*sunVisibility*pow(saturate(dot(dome,halfDir)),240)*beadMask*2.5;
    }
    float exponent=lerp(28,100,CoatingDetailFade(footprint,.12))*(1-porous*.4);
    color+=SunColor()*CL_sunRelight*sunVisibility*saturate(dot(wetNormal,sun))*pow(saturate(dot(wetNormal,halfDir)),exponent)*wet*.3;
    // A LAMP SEEN IN WATER (WaterLightGlint in EnvironmentCoatingMath.h): water's Fresnel times a
    // normalized lobe, near a mirror on a puddle. Placed lights draw it with the Lighting card on or
    // off. (The old pow(N.H,n)*.25 put a lamp on a puddle no brighter than its own pool.)
    float glintExponent=WaterGlintExponent(puddle,porous,CoatingDetailFade(footprint,.12));
    [loop]for(int l=0;l<(int)lighting.x;++l){
        float3 dir;float atten=StudioAttenuation(l,0,p,dir);
        if(atten<=0)continue;
        atten*=WorldLightShadowSample(l,p+wetNormal*shadowInfo.y,0,viewmodel);
        float3 h=normalize(view+dir);
        color+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*
            WaterLightGlint(dot(wetNormal,h),dot(wetNormal,dir),dot(view,h),glintExponent)*wet;
    }
    // THE MAP'S OWN LAMPS IN THE WATER (EnvironmentPuddleLampMath.h): the hanging bulb over a puddle
    // shows in it, with the Lighting card on or off. Their light and falloff are the lit programs'
    // own (LiteLampRadiance); the highlight is the placed lights' water term. A map lamp has no
    // shadow here, so whether the lamp is in sight of the water is read off the screen (MapLampSeen).
    [loop]for(int m=0;m<(int)mapLampInfo.x;++m){
        float4 at=mapLamps[m*4],shine=mapLamps[m*4+1],second=mapLamps[m*4+2],third=mapLamps[m*4+3];
        float3 delta=at.xyz-p;
        float d2=max(dot(delta,delta),1e-5),dist=sqrt(d2);
        float3 dir=delta/dist,h=normalize(view+dir);
        float glint=WaterLightGlint(dot(wetNormal,h),dot(wetNormal,dir),dot(view,h),glintExponent)*wet;
        if(glint<1e-3)continue;
        float fade;
        [branch]if(shine.w<.5){
            // A bulb: the range window, the inverse square past its soft core, its cone.
            fade=saturate(third.x+second.w*dist);
            fade=fade*fade*(3-2*fade)*at.w/max(at.w,d2);
            if(third.z!=0)fade*=saturate(third.y+third.z*dot(second.xyz,dir));
        }else{
            // A shaded lamp: its bulb, seen from anywhere below the shade, as bright as its beam is
            // this far from the beam's eye.
            float3 e=second.xyz-p;
            fade=at.w/max(at.w,max(dot(e,e),1e-5))*smoothstep(0,.25,dot(third.xyz,-dir));
        }
        float3 lit=shine.rgb*fade*glint;
        if(max(lit.r,max(lit.g,lit.b))<1e-4)continue;
        color+=lit*MapLampSeen(p+wetNormal*2,at.xyz,sqrt(at.w)+12);
    }
    return color;
}
)HLSL"
R"HLSL(
// The lighting of a surface, without its coatings. Kept apart from Surface so the composite can
// run the coatings ONCE for both of its paths: the compiler expands every call in place, and two
// copies of ApplyCoatings in one program took it six minutes instead of three.
float3 SurfaceLight(float3 color,float3 p,float3 normal,bool viewmodel=false,float contactOcclusion=1,float3 nativeDirect=float3(-1,-1,-1),float3 nativeFixtures=0) {
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
    return color;
}
float3 Surface(float3 color,float3 p,float3 normal,float3 coatingNormal,bool viewmodel=false,float contactOcclusion=1,float3 nativeDirect=float3(-1,-1,-1),float3 nativeFixtures=0) {
    float mapLuminance=dot(max(color,0),float3(.2126,.7152,.0722));
    return ApplyCoatings(SurfaceLight(color,p,normal,viewmodel,contactOcclusion,nativeDirect,nativeFixtures),p,normal,coatingNormal,mapLuminance,viewmodel);
}
// THE CLOUD LAYER. Shape after Schneider, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn"
// (SIGGRAPH 2015, GPU Pro 7): a cumulus height gradient (the profile fede-vaccaro/TerrainEngine-OpenGL
// uses, MIT), coverage applied as remap(base * gradient, 1 - coverage, 1, 0, 1) * coverage, and a
// high-frequency noise eroding the edges. Lighting after SebLague/Clouds (MIT): Beer's law toward the
// light with a darkness floor, and a dual-lobe Henyey-Greenstein phase.
//
// RAIN DARKENS THE CLOUDS (and nothing else): HZD's precipitation raises coverage and density and
// darkens the layer. Here it also absorbs more light on its way through and lowers the darkness floor,
// so a raining or storming sky turns heavy and grey. 0..1, scaled by the Rain darkening control.
float CloudPrecipitation() {
    float rain=weatherParams.z==1?saturate(weatherColor.w*1.5):0;
    return saturate(max(rain,CL_storm>0?1:0))*saturate(CL_cloudRain);
}
float CloudRemap(float v,float lo,float hi) { return saturate((v-lo)/max(hi-lo,1e-4)); }
float CloudDensity(float3 p,float precipitation=0,bool detail=true) {
    float h=(p.z-CL_cloudBase)/CL_cloudDepth;
    if(h<=0||h>=1)return 0;
    float3 q=p*CL_cloudScale+float3(climateClock.y*CL_cloudScale,climateClock.y*CL_cloudScale*.28,0);
    // Value-noise fBm sits close to 0.5; stretched as FogVariation does, so the remap can cut shapes.
    float base=saturate((Fbm(q)-.5)*2.6+.5);
    float gradient=smoothstep(0,.1625,h)-smoothstep(.88,.98,h);
    float coverage=saturate(CL_coverage+.35*precipitation);
    float cloud=CloudRemap(base*gradient,1-coverage,1)*coverage;
    [branch]if(detail&&cloud>0){
        // Wispy at the base, billowy toward the top (HZD's high-frequency modifier).
        float fine=Noise3(q*5.3+float3(0,0,climateClock.x*.02))*.65+Noise3(q*11.1-7)*.35;
        fine=lerp(fine,1-fine,saturate(h*10));
        cloud=CloudRemap(cloud,fine*.2,1);
    }
    return cloud*CL_density*(1+1.5*precipitation)*3;
}
// Dual-lobe Henyey-Greenstein (SebLague/Clouds defaults: forward .83, back .3, base .8, factor .15).
float CloudHg(float cosine,float g) { float g2=g*g;return (1-g2)/(12.566371*pow(max(1+g2-2*g*cosine,1e-4),1.5)); }
float CloudPhase(float cosine) { return .8+(CloudHg(cosine,.83)*.5+CloudHg(cosine,-.3)*.5)*.15; }
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
// THE SKY LIGHT IN THE FOG. With the Environment sky on, the fogs take their light from that sky
// (zenith, and the horizon away from the sun), so a sunset or a night sky changes the fog with it.
// Otherwise it is the map's own sky colour (fogAmbient, from the map's light_environment).
float3 FogAmbient(){
    float3 ambient=fogAmbient.rgb;
    [branch] if(CL_sky>0){
        float3 sun=SunDirection();
        float2 away=length(sun.xy)>1e-3?-normalize(sun.xy):float2(1,0);
        float3 sky=(SkyColor(float3(0,0,1))+SkyColor(normalize(float3(away,.2))))*.5;
        ambient=lerp(ambient,sky*.9,saturate(CL_sky));
    }
    return ambient;
}
// How much sunlight still reaches a point inside a fog: the fog between it and the sun dims it (so a
// thick bank is bright on top and grey underneath). A little light always gets through by bouncing
// around inside the fog, which a single exponential would black out.
float FogSunTransmittance(float opticalDepth){return exp(-opticalDepth)*.75+exp(-opticalDepth*.25)*.25;}

)HLSL"
R"HLSL(
// GROUND FOG. A layer lying on the floor: its thickness is measured from the floor under each point
// (the floor map, EnvironmentGroundFogMath.h), the fluid (EnvironmentGroundFog.cpp) thins it where
// players walked, and drifting billows raise and lower its top.
static const float kGroundFogNoFloor=-5e5;
// The floor under p: the highest floor at or below it, blended over the four nearest texels that have
// one. Only texels near the closest one's height blend: a wall or ledge between two floors is a real
// jump, and averaging across it made a floor halfway up the wall with fog floating on it. Mirrors
// GroundFogFloorBelow. Without a floor map the floor is the fog's Base height.
// `held`: how much of the layer's depth that floor holds, thinning toward any drop.
float GroundFogFloor(float3 p,out float held){
    held=1;
    if(groundFogFloor.w<.5)return CL_fogBase;
    float2 f=(p.xy-groundFogFloor.xy)/groundFogFloor.z-.5;
    int2 base=int2(floor(f));float2 t=f-float2(base);
    float hs[4],ws[4],ts[4],best=-1,reference=-1e6;
    [unroll]for(int k=0;k<4;++k){
        int2 q=base+int2(k&1,k>>1);
        hs[k]=-1e6;ts[k]=1;ws[k]=((k&1)?t.x:1-t.x)*((k>>1)?t.y:1-t.y)+1e-4;
        if(any(q<0)||any(q>=int2(groundFogFloorSize.xy)))continue;
        float4 layers=groundFogFloorMap.Load(int3(q,0)),thick=groundFogThickness.Load(int3(q,0));
        [unroll]for(int j=3;j>=0;--j)if(layers[j]>kGroundFogNoFloor&&layers[j]<=p.z+8){hs[k]=layers[j];ts[k]=thick[j];}
        if(hs[k]>kGroundFogNoFloor&&ws[k]>best){best=ws[k];reference=hs[k];}
    }
    if(best<0)return -1e6;
    float sum=0,weight=0,heldSum=0;
    [unroll]for(int k=0;k<4;++k)if(hs[k]>kGroundFogNoFloor&&abs(hs[k]-reference)<40){sum+=hs[k]*ws[k];heldSum+=ts[k]*ws[k];weight+=ws[k];}
    held=heldSum/weight;
    return sum/weight;
}
// How much fog the fluid holds at a map point: 1 at rest, less where bodies thinned it, up to 1.3 in
// the rim they shove it into. Outside the grid (past the map) it is at rest.
float2 GroundFogUv(float2 xy){return (xy-groundFogWindow.xy)*groundFogWindow.zw;}
float GroundFogFluid(float2 xy){
    if(groundFogWindow.z<=0)return 1;
    float2 uv=GroundFogUv(xy);
    if(any(uv<=0)||any(uv>=1))return 1;
    return clamp(groundFogState.SampleLevel(groundFogSampler,uv,0).z,0,1.3);
}
// The same field softened: the top of the layer follows this, so it stays a gentle dune.
float GroundFogSmoothed(float2 xy){
    if(groundFogWindow.z<=0)return 1;
    float2 uv=GroundFogUv(xy);
    if(any(uv<=0)||any(uv>=1))return 1;
    return clamp(groundFogSmooth.SampleLevel(groundFogSampler,uv,0),0,1.3);
}
// The same field with the air's velocity in xy, which the wisps bend with.
float3 GroundFogFlow(float2 xy){
    if(groundFogWindow.z<=0)return float3(0,0,1);
    float2 uv=GroundFogUv(xy);
    if(any(uv<=0)||any(uv>=1))return float3(0,0,1);
    float3 s=groundFogState.SampleLevel(groundFogSampler,uv,0).xyz;
    return float3(s.xy,clamp(s.z,0,1.3));
}
// How dense the layer is at height h, for a local top. Softness 0 is full through most of the layer with
// a defined top; 1 thins all the way up from the floor and reaches higher, so there is no surface at all.
// Mirrors GroundFogProfile in EnvironmentGroundFogMath.h.
float GroundFogProfile(float h,float top){
    if(top<=0)return 0;
    if(h<=0)return 1;
    float soft=CL_groundFogSoftness,reach=top*(1+1.2*soft);
    if(h>=reach)return 0;
    float start=.55*(1-soft);
    float t=saturate((h-start*reach)/((1-start)*reach));
    return pow(1-t*t*(3-2*t),1+1.5*soft);
}
// As high over its floor as this layer's fog can stand: the tallest top GroundFogAt makes (billows
// 1.28, heaped fluid 1.25^.8 = 1.195, wisps 1.21, so 1.85) with a little to spare. At 1.55 the heaped,
// wisped peaks were cut while still about a third dense: a flat lid across the top of the layer.
float GroundFogReach(){return CL_groundFogHeight*1.86*(1+1.2*CL_groundFogSoftness);}
// The field that tears the fog into wisps, 0 to 1. It is stretched along the drift and flattened into thin
// sheets of height: a ray near the horizon stays inside one sheet for a long way, and that is what reads as
// a long streak. Uniform noise in height averages away along such a ray and leaves smooth fog. `wave` (the
// billows, 0 to 1) bends the sheets, because flat ones draw ruler-straight bars across the screen. The moving
// air bends them as well, so a wake drags the streaks along.
float GroundFogWispField(float3 p,float2 air,float wave){
    float size=CL_groundFogWispSize,streak=CL_groundFogStreak;
    float angle=CL_groundFogWindAngle*.0174533,ca=cos(angle),sa=sin(angle);
    float2 bent=p.xy-groundFogFloorSize.zw-clamp(air*.35,-120,120);
    float2 w=float2((bent.x*ca+bent.y*sa)/(1+streak*8),bent.y*ca-bent.x*sa)/size;
    float layer=(p.z+(wave-.5)*size*3)/(size*lerp(1,.4,streak));
    float churn=climateClock.x*CL_groundFogChurn;
    return Noise3(float3(w,layer+churn*.1))*.62+Noise3(float3(w*2.3+3.7,layer*2.3+churn*.17))*.38;
}
// The fluid decides how dense the fog is and how tall it stands, so the top of the layer is a surface
// the flow shapes. The top follows the softened field and stops at 1.25, because following the fine
// marbling directly made it spike wherever fog was pushed or heaped; the marbling shows in the density.
// Mirrors GroundFogDensityShare and GroundFogTopShare in the math header.
float GroundFogDensityShare(float fluid){float a=saturate(fluid);return a*a*(3-2*a)*(1+.4*max(fluid-1,0));}
float GroundFogTopShare(float smoothed){return pow(clamp(smoothed,0,1.25),.8);}
// Dense enough at Density 1 to hide the floor: about two optical depths straight down through the layer.
static const float kGroundFogExtinction=.045;
// Extinction per unit at p, with the layer's local top, p's height over its floor, and that floor.
float GroundFogAt(float3 p,out float top,out float h,out float floorZ,out float held){
    top=0;h=0;
    floorZ=GroundFogFloor(p,held);
    if(floorZ<=kGroundFogNoFloor||held<.02)return 0;
    h=p.z-floorZ;
    if(h>GroundFogReach()||h<-24)return 0;
    float3 flow=GroundFogFlow(p.xy);
    float fluid=flow.z;
    if(fluid<=.005)return 0;
    float2 drifted=p.xy-groundFogFloorSize.zw;
    float churn=climateClock.x*CL_groundFogChurn;
    // Two octaves of slowly drifting billows, on top of the relief the flow carries.
    float2 q=drifted/CL_groundFogBillowSize;
    float billow=Noise3(float3(q,churn*.03))*.62+Noise3(float3(q*2.3+7.1,churn*.05))*.38;
    float wave=billow;
    billow=lerp(.5,billow,CL_groundFogDetail);
    top=CL_groundFogHeight*(.72+.56*billow)*GroundFogTopShare(GroundFogSmoothed(p.xy))*held;
    // Wisps tear the layer. They fade with distance, where they are finer than a march step can see.
    float away=saturate(length(p-camera.xyz)/5000);
    float tear=CL_groundFogWisps*(1-away*away);
    float wisp=1;
    [branch] if(tear>.001){
        float torn=smoothstep(.2,.8,GroundFogWispField(p,flow.xy,wave));
        wisp=lerp(1,torn*1.7,tear);
        top*=lerp(1,.65+.7*torn,tear*.6);
    }
    float profile=GroundFogProfile(h,top);
    if(profile<=0)return 0;
    return CL_groundFog*kGroundFogExtinction*GroundFogDensityShare(fluid)*profile*wisp;
}
)HLSL"
R"HLSL(
// How much fog lies between a point and the sun: a short march through the fluid. This is what shades
// the layer, bright on sun-facing slopes and darker behind a heap, with the fog above dimming the fog
// below. The floor stays the sample's own: the fog above a point is measured from the same ground.
float GroundFogLightDepth(float3 q,float floorZ,float held,float3 sun){
    float ds=CL_groundFogHeight*.5/max(sun.z,.25),tau=0;
    [unroll]for(int k=0;k<4;++k){
        float3 s=q+sun*ds*(k+.5);
        float fluid=GroundFogSmoothed(s.xy);
        tau+=GroundFogDensityShare(fluid)*GroundFogProfile(s.z-floorZ,CL_groundFogHeight*GroundFogTopShare(fluid)*held);
    }
    // Thin soft mist scatters light from all sides and barely shades itself. Counting all of it as blocking
    // the sun left only blue sky light in it.
    return tau*ds*CL_groundFog*kGroundFogExtinction*(1-.8*CL_groundFogSoftness);
}
// How close the ray q + r*s (s >= 0, r of unit length) comes to the segment a..b, and how far along
// the ray that is (`s`).
float RaySegmentGap(float3 q,float3 r,float3 a,float3 b,out float s){
    float3 ab=b-a,aq=q-a;
    float abab=max(dot(ab,ab),1e-4),abr=dot(ab,r),abaq=dot(ab,aq),raq=dot(r,aq);
    float denom=abab-abr*abr;
    float u=denom>1e-3?saturate((abaq-abr*raq)/denom):0;
    s=max(abr*u-raq,0);
    u=saturate(dot(aq+r*s,ab)/abab);
    return length(aq+r*s-ab*u);
}
// One part's shadow at q: soft by the sun's own size and the scattering, more the further it falls.
float FogBodyPartShadow(float3 q,float3 sun,float3 a,float3 b,float radius){
    float s;float gap=RaySegmentGap(q,sun,a,b,s);
    float soft=.8+s*.012;
    return smoothstep(radius-soft,radius+soft,gap);
}
// THE PLAYERS' SHADOWS ON THE FOG, shaped like them: each body is the capsules between its bones, as
// it is drawn this frame (actorBones), so the shadow walks, crouches and holds a gun with it. A body
// whose bones could not be read is a slim upright capsule. One upright capsule for every body was
// a pill laid along the sun (reported 2026-11-17). The sun's shadow map holds only the players CS2
// drew to the screen this frame, never the first-person player, and on opaque fog a body without a
// shadow seems to float above the layer.
float GroundFogBodyShadow(float3 q,float3 sun){
    float visible=1;
    int count=(int)coatingActorCount.x;
    [loop]for(int i=0;i<count;++i){
        if(coatingActors[i].w<=0)continue;
        float3 o=coatingActors[i].xyz;
        // Only where the line to the sun passes within a body's reach of its middle.
        float3 d=o+float3(0,0,36)-q;float along=dot(d,sun);
        if(along<-48||dot(d,d)-along*along>3600)continue;
        int base=i*16;
        [branch]if(actorBones[base].w>.5){
            [loop]for(int k=0;k<16;++k){
                float3 part=kBodyPartsHlsl[k];
                float4 a=actorBones[base+(int)part.x],b=actorBones[base+(int)part.y];
                if(a.w<.5||b.w<.5)continue;
                float3 from=a.xyz,to=b.xyz;
                if(k==3){float3 up=from-actorBones[base+2].xyz;from+=(dot(up,up)>1?normalize(up):float3(0,0,1))*3.5;to=from;}
                visible=min(visible,FogBodyPartShadow(q,sun,from,to,part.z));
            }
        }else visible=min(visible,FogBodyPartShadow(q,sun,o+float3(0,0,10),o+float3(0,0,60),8));
        if(visible<.01)break;
    }
    return visible;
}
)HLSL"
R"HLSL(
// The ground layer, marched before the height fog (it lies nearest the floor, in front of most of
// it). Steps are fine inside the layer and long above it: the gap to the layer's highest possible top,
// over how steeply the ray descends, is how far the ray travels before it can meet fog. Inside the
// layer each step is a fixed share longer than the last, so the march reaches about 5500 units (9000
// in export). A slower growth stopped it near 400 units, and the fog then vanished in the distance
// whenever the camera stood in the layer and came back when it rose above it (reported 2026-11-08).
// Steps also shorten to the fog's own thickness: in dense fog a long step goes opaque in one sample, so
// the surface snapped to the step positions and showed as terraces.
// THIN LAYERS. In the layer, or about to enter it, no step crosses more than an eighth of its depth
// on the way up or down. A ray going down through a Depth 8 layer otherwise crossed all of it in one
// step, and whether a pixel saw any fog came down to where that one sample landed: the fog vanished
// at some view angles and came back at others, and crawled with grain (reported 2026-11-17).
static const float kGroundFogShadeFloor=.2;
// How much of the sky reaches the fog at q: open sky straight above it (the sky map), or open air
// that reaches in under cover (the cover volume, 1 beside open sky and falling with every step under
// a roof), with a little left for light bounced in off walls. Fog in a covered room or inside a stack
// of crates took the whole sky and glowed white in the dark (reported 2026-11-17). Fails open while
// the sky map is not drawn yet. The fog point is tested where it is: the sky map's thickness margin
// (shelterInfo.z) is for surfaces, and lifted by it a point under a plank tested above the plank and
// took the open sky. Air reaching in from the side counts as its square, so the inside of a stack of
// crates open toward the camera is dim rather than lit as if outside.
float GroundFogSkyOpen(float3 q){
    if(shelterInfo.x<=0)return 1;
    float reach=CoverReach(q).x;
    return lerp(.1,1,max(SkyTap(q),reach*reach));
}
void GroundFogMarch(float3 ray,float distance,float jitter,float3 ambient,inout float3 color,inout float trans){
    [branch] if(CL_groundFog<=0)return;
    float range=min(distance,9000),maxTop=GroundFogReach();
    float3 sun=SunDirection();
    // Dense low fog scatters light many times, so the sun lights it from every side, not only along
    // the view: near white in the sun, a soft grey in shade. Shade stays near neutral, because a sky-
    // blue tint across a whole floor reads as a coloured film rather than as fog in shadow.
    float3 sunLight=SunColor()*(Phase(dot(ray,sun))*.2+.5)*CL_groundFogSun;
    float3 sky=lerp(dot(ambient,float3(.2126,.7152,.0722)).xxx,ambient,.5)*.85*CL_groundFogShade*
               float3(CL_groundFogShadeR,CL_groundFogShadeG,CL_groundFogShadeB);
    float3 tint=float3(CL_groundFogColorR,CL_groundFogColorG,CL_groundFogColorB);
    bool exporting=volumeDimensions.z>40;
    int steps=exporting?160:96;
    float growth=exporting?.045:.07;
    float climb=max(abs(ray.z),.01),slab=maxTop*.125;
    float t=0,limit=1e9;
    [loop]for(int i=0;i<steps&&t<range;++i){
        float3 p=camera.xyz+ray*t;
        float heldHere;float floorHere=GroundFogFloor(p,heldHere);
        float over=floorHere<=kGroundFogNoFloor?1e4:p.z-floorHere,above=over-maxTop;
        float dt=clamp(3+t*growth,3,240);
        if(over<maxTop*2)dt=min(dt,max((max(above,0)+slab)/climb,1.5));
        if(above>0)dt=max(dt,min(above/max(-ray.z,.04)*.6,300));
        dt=min(min(dt,limit),range-t);
        float3 q=camera.xyz+ray*(t+dt*jitter);
        float top,h,floorZ,held;float sigma=GroundFogAt(q,top,h,floorZ,held);
        // Clear air: carry on, letting a step cut short by dense fog grow back.
        if(sigma<=0){t+=dt;limit*=1.6;continue;}
        // Too long a step for fog this thick: take this stretch again in short steps.
        float fine=clamp(.7/sigma,3,240);
        if(dt>fine*1.5){limit=fine;continue;}
        limit=fine*1.25;
        t+=dt;
        // THE SHADOWS ON IT, by the ground fog's own Shadows slider (Light shafts are the height fog's).
        // The sun's shadow is averaged along the step, so a shadow's edge does not move with each
        // pixel's jitter: looking along it, that moved the edge a whole step and drew it as a sawtooth.
        // It is also averaged sideways over a few units (a quarter of the layer's depth, 4 to 16):
        // light is scattered that far inside low fog, so a roof's tiles or a tree's twigs do not rule
        // their outline across it. Spread over the layer's whole depth, as it was, the shadows washed
        // out until buildings seemed to cast none on the fog (reported 2026-11-17).
        float shadow=1;
        [branch]if(CL_groundFogShadows>0){
            // The four readings are turned a different way at every pixel and every frame: at four
            // fixed places they drew a shadow's edge four times over, as steps. Turned, they are grain
            // that the volume's temporal blend and blur smooth into one soft edge.
            static const float2 kAcross[4]={float2(1,0),float2(0,1),float2(-1,0),float2(0,-1)};
            float turn=jitter*6.2831853;float2 spin=float2(cos(turn),sin(turn));
            float spread=clamp(CL_groundFogHeight*.25,4,16),lit=0;
            [unroll]for(int k=0;k<4;++k){
                float2 o=float2(kAcross[k].x*spin.x-kAcross[k].y*spin.y,kAcross[k].x*spin.y+kAcross[k].y*spin.x)*spread*(.25+.25*k);
                lit+=WorldSunShadow(camera.xyz+ray*(t-dt*(k+.5)/4)+float3(o,0)+sun*shadowInfo.y);
            }
            lit/=4;
            // The shadow map holds the players CS2 drew this frame; the bodies hold every player.
            if(sun.z>.02)lit=min(lit,GroundFogBodyShadow(q,sun));
            // Fog in a building's shade is still lit by the sunlit fog beside it: it is dimmer, not a
            // different colour laid on the floor in the shape of the roof.
            shadow=lerp(1,lerp(kGroundFogShadeFloor,1,lit),CL_groundFogShadows);
        }
        float3 light=sky*GroundFogSkyOpen(q)*lerp(.5+.35*CL_groundFogSoftness,1,saturate(h/max(top,1)))+
            sunLight*shadow*FogSunTransmittance(GroundFogLightDepth(q,floorZ,held,sun));
        [loop]for(int l=0;l<min((int)lighting.x,4);++l){
            float3 dir;float atten=StudioAttenuation(l,0,q,dir);
            if(atten<=.001)continue;
            if(CL_groundFogShadows>0)atten*=lerp(1,WorldLightShadowSample(l,q,0),CL_groundFogShadows);
            light+=lightColorIntensity[l].rgb*lightColorIntensity[l].w*atten*Phase(dot(ray,dir));
        }
        float a=1-exp(-sigma*dt);
        // THE OTHER FOG LIES IN FRONT OF IT TOO. The ground layer is marched first, so on its own it
        // was drawn at full strength however much height fog stood between it and the eye: white
        // billows cut out of a blue haze. Each piece of it is seen through the height fog up to it,
        // and takes that fog's colour for the share it hides.
        float3 lit=light*tint;
        [branch]if(CL_fogDensity>0&&CL_localFogRadius<=0){
            float along=max(0,t-CL_fogStart);
            float thick=CL_fogDensity*.001*.5*(exp(-max(0,camera.z-CL_fogBase)/CL_fogHeight)+exp(-max(0,q.z-CL_fogBase)/CL_fogHeight));
            float seen=exp(-thick*along);
            lit=lerp((ambient+SunColor()*Phase(dot(ray,sun))*.25)*fogColorDensity.rgb,lit,seen);
        }
        color+=trans*a*lit;trans*=1-a;
        if(trans<.01)break;
    }
}
)HLSL"
R"HLSL(
// WHAT A GUST BLOWS ALONG THE GROUND: a thin volume lying on the floor, as thick at each place as the
// wind fluid's load there (GustCarried), eaten into lumps and wisps by a billow noise the air drags
// along, lit by the sun through itself. A surface-aligned smoke volume, sized against a box as wide
// as the simulated area (the values below are in CS2 inches):
//   snow  sun-lit, noisy, over a 50 m area  (a box 1968.5 units across),
//   ash   sun-lit, noisy, a sand storm's shape in ash's greys, over a 50 m area,
//   rain  unlit spray, noisy, over an 80 m area (3150 units).
// THE SHAPE. The layer is a slab round a plane `lift` over the floor. Where the load is d (raised to
// `power`), the slab's core reaches `extrude` x d out from the plane, `offset` further (negative:
// nearer), with a soft edge `soften` deep; a background layer as high as extrude + soften thins
// toward its top (to the power `falloff`) and carries a share `base` of it. The noise eats both: the
// core keeps 1 - erode x (1 - noise) of itself, the background only where noise beats base/baseNoise.
// It is a layer of the fog volume rather than a box of its own, so it
// takes the volume's temporal blend and its sun shadows.
float PrecipitationOpenSky(float3 world); // EnvironmentShaders.h, after this file
float PrecipitationRegion(float3 p);
struct GustLook{
    float extrude,soften,offset,lift; // units
    float sigma,power,gain,most;      // extinction per unit of a full sample; load exponent; load scale; the most it takes
    float base,baseNoise,falloff;     // the background layer
    float erode,period,cycle;         // noise: how hard it eats the core, its repeat (units), the flow map's cycle (s)
    float shadow,lit;                 // how strongly it shades itself toward the sun; 0 = unlit (lit by the sky alone)
    float rampPow,rampMul,rampAdd;    // the colour ramp from thin to dense
    float3 thin,dense;                // colours at the two ends of the ramp
    float top;                        // the highest it can reach over its floor (units)
};
GustLook GustLookFor(float type){
    GustLook g;
    if(type==2){ // snow
        g.extrude=29.5;g.soften=39.4;g.offset=-39.4;g.lift=0;
        // A bank stands about 70 units, a drift a hand. Half the reference's density: at its value a
        // bank filled a street like a solid grey bed (reported 2026-11-18).
        g.sigma=300/1968.5;g.power=2;g.gain=1.5;g.most=3.2;
        g.base=.25;g.baseNoise=.7;g.falloff=2;
        g.erode=2;g.period=1968.5/4;g.cycle=1/(.8*.5);
        g.shadow=.3;g.lit=1;g.rampPow=1;g.rampMul=.9;g.rampAdd=.5;
        // A soft grey where thin to snow's white where dense: neutral, as the snow on the ground is.
        // (A cool blue-grey laid a blue film over the whole floor.)
        g.thin=float3(.82,.83,.86);g.dense=float3(.97,.97,.98);
        g.top=110;
    }else if(type==3){ // ash: a sand storm's shape in ash's greys
        g.extrude=23.6;g.soften=29.5;g.offset=-39.4;g.lift=0;
        // A sand storm lifts about three times snow's load, but ash is lighter than sand: at sand's
        // density the densest banks stood as solid grey blocks (reported 2026-11-18).
        g.sigma=800/1968.5*.4;g.power=1;g.gain=1.4;g.most=2.5;
        g.base=.2;g.baseNoise=.8;g.falloff=2;
        g.erode=3;g.period=1968.5/3;g.cycle=1/(1*.5);
        g.shadow=.4;g.lit=1;g.rampPow=2;g.rampMul=.9;g.rampAdd=0;
        // Warm greys: dark where thin, pale ash where dense.
        g.thin=float3(.26,.25,.24);g.dense=float3(.56,.54,.51);
        g.top=80;
    }else{ // rain: low spray
        g.extrude=15.7;g.soften=94.5;g.offset=-63;g.lift=15.7;
        g.sigma=20/3150.;g.power=.1;g.gain=1;g.most=4;
        g.base=-1;g.baseNoise=-4;g.falloff=5;
        g.erode=4;g.period=3150/4.;g.cycle=1/(.4*.5);
        g.shadow=0;g.lit=0;g.rampPow=6;g.rampMul=4;g.rampAdd=0;
        // Spray: grey where thin, near white where dense.
        g.thin=float3(.42,.43,.45);g.dense=float3(.86,.88,.90);
        g.top=130;
    }
    // The Weather card's Ground drift rows scale each weather's own look: how dense it is, how high
    // it stands, how much the noise tears it (0 leaves an even sheet).
    g.sigma*=CL_gustDensity;
    float tall=CL_gustHeight;
    g.extrude*=tall;g.soften*=tall;g.offset*=tall;g.lift*=tall;g.top*=tall;
    g.erode*=CL_gustWisps;g.baseNoise*=CL_gustWisps;
    return g;
}
// The billow noise at q, dragged along by the air (flow mapping): all of it slides
// downwind with the steady wind (a rigid slide, which never tears it), and where the air moves
// otherwise two copies half a cycle apart are dragged that way and cross-faded, each shown when its
// drag is smallest. 0..1.
float GustNoise(float3 q,float2 air,GustLook g){
    float2 steady=weatherVelocity.xy;
    float3 at=float3(q.xy-steady*climateClock.x,q.z)/g.period;
    float t=climateClock.x/g.cycle;
    float a=frac(t)-.5,b=frac(t+.5)-.5,w=abs(a*2);
    float3 drag=float3(GustDragRest(air-steady,g.period,g.cycle)*g.cycle,0)/g.period;
    float na=gustNoise.SampleLevel(gustNoiseSampler,at-drag*a,0);
    float nb=gustNoise.SampleLevel(gustNoiseSampler,at-drag*b,0);
    return lerp(na,nb,w);
}
// THE FLOOR A GUST'S DRIFT LIES ON: the ground, not what stands on it, and no step at a kerb. Laid on
// the ground fog's floor map as it is, the drift lay on top of every crate, car and barrel, cut off at
// their edges as if with a knife, and stepped at every kerb (reported 2026-11-18). The drift floor
// (GroundFogDriftFloor: each column lowered to the ground round it and smoothed, stored over 100000)
// is read smoothly, and stands for this point's floor when it is that floor or below it by no more
// than a storey; a higher value is a deck over this street, and then the floor map's own floor is
// taken. Without a floor map, the eye's own ground.
static const float kGustDriftFloorBias=100000;
float GustFloor(float3 p){
    float held;float f=GroundFogFloor(p,held);
    if(groundFogFloor.w<.5||f<=kGroundFogNoFloor)return camera.z-64;
    float2 uv=(p.xy-groundFogFloor.xy)/(groundFogFloor.z*groundFogFloorSize.xy);
    float drift=gustFloorMap.SampleLevel(groundFogSampler,uv,0)-kGustDriftFloorBias;
    return drift<=f+8&&drift>=f-150?drift:f;
}
// THE LIGHT ON THE SNOW UNDER A POINT OF THE DRIFT, the way the snow on the ground takes it
// (ApplyCoatings' mapped snow: the frame's light, its luminance through SnowCoverShade, a fifth of its
// colour). Read in the frame where that floor point is, five taps round it, so the drift is lit by the
// map's own sun and shade, whatever the Environment's sun is set to. Lit by that sun and sky instead,
// it came out a grey bed on a sunlit street under map lighting (reported 2026-11-18). A point whose
// floor is off screen or hidden takes the frame behind itself.
float3 GustFloorLight(float3 q,float floorZ){
    float3 at=float3(q.xy,floorZ);
    float4 h=mul(viewProjection,float4(at,1));
    float2 pixel=(h.xy/max(h.w,1e-4)*float2(.5,-.5)+.5)*dimensions.xy;
    bool seen=h.w>1e-4&&all(pixel>=0)&&all(pixel<dimensions.xy);
    if(seen)seen=length(World(pixel,ReadCombinedSample(pixel,0))-at)<40;
    [branch]if(!seen){float4 g=mul(viewProjection,float4(q,1));pixel=(g.xy/max(g.w,1e-4)*float2(.5,-.5)+.5)*dimensions.xy;}
    float3 light=ReadSource(pixel,0)*2;
    light+=ReadSource(pixel+float2(-12,-9),0)+ReadSource(pixel+float2(12,-9),0)+ReadSource(pixel+float2(-12,9),0)+ReadSource(pixel+float2(12,9),0);
    light=max(light/6,0);
    float l=dot(light,float3(.2126,.7152,.0722));
    return lerp(float3(1,1,1),light/max(l,1e-4),.22)*SnowCoverShade(l)*1.02;
}
// The slab's density at q over a floor at floorZ, 0..1, before the noise (core) and the background
// layer's share (back): a plane extruded by the load, with a soft edge, and a fade gradient.
void GustSlab(float h,float d,GustLook g,out float core,out float back){
    core=1-saturate((h-g.offset-d*g.extrude+g.soften*.5)/g.soften);
    back=pow(1-saturate(h/(g.extrude+g.soften)),g.falloff)*d;
}
// Extinction per unit at q (and the load d, for the colour ramp). `noisy` false skips the noise
// (the march toward the sun), taking the share it keeps on average.
// One return: called inside branches of the big composite, where helpers that return early have
// overflowed the compiler's stack.
float GustVolumeAt(float3 q,float floorZ,GustLook g,bool noisy,out float d){
    d=0;
    float sigma=0;
    float h=abs(q.z-floorZ-g.lift);
    [branch]if(h<=g.top+g.lift){
        float4 air=WindAt(q);
        float load=GustCarried(air)*g.gain;
        [branch]if(load>.002){
            d=min(pow(load,g.power),g.most);
            float core,back;GustSlab(h,d,g,core,back);
            [branch]if(core>0||back>0){
                float m=noisy?1-GustNoise(q,air.xy,g):.5;
                sigma=saturate(saturate(core-m*g.erode*core)+saturate(g.base-m*g.baseNoise)*back)*g.sigma;
            }
        }
    }
    return sigma;
}
)HLSL"
R"HLSL(
void GustVolumeMarch(float3 ray,float distance,float jitter,float3 ambient,inout float3 color,inout float trans){
    [branch]if(CL_gusts<=0||weatherParams.z<1||weatherParams.z>3||windFluidWindow.z<=0||weatherColor.w<=0)return;
    GustLook g=GustLookFor(weatherParams.z);
    float show=GustShown();
    // Drawn out to 3000 units from the eye, faded over the last 600: the fluid covers the map, the
    // march does not need to.
    float range=min(distance,3000),reach=g.top+g.lift;
    float3 sun=SunDirection();
    float3 sunLight=SunColor()*(Phase(dot(ray,sun))*.2+.5);
    // The sky lights it mostly as grey: a fifth of its blue, as the snow on the ground takes it.
    float3 sky=lerp(dot(ambient,float3(.2126,.7152,.0722)).xxx,ambient,.2)*.9;
    float climb=max(abs(ray.z),.01);
    float t=0;
    [loop]for(int i=0;i<80&&t<range;++i){
        float3 p=camera.xyz+ray*t;
        float floorHere=GustFloor(p);
        float over=p.z-floorHere,above=over-reach;
        // Long steps over the layer and in the distance, short ones in it: no step crosses more than
        // a sixth of its depth on the way up or down.
        float dt=clamp(4+t*.045,4,180);
        if(over<reach*2)dt=min(dt,max((max(above,0)+reach/6)/climb,2));
        if(above>0)dt=max(dt,min(above/max(-ray.z,.04)*.6,300));
        if(over<-40&&ray.z<0)break; // under the floor
        dt=min(dt,range-t);
        float3 q=camera.xyz+ray*(t+dt*jitter);
        t+=dt;
        float region=PrecipitationRegion(q);
        if(region<=0)continue;
        // The sample's own floor: a step runs up to 180 units on from p, and over a stair or a kerb
        // the floor at p left the drift floating over the higher ground or sunk into the lower.
        float floorZ=GustFloor(q);
        float d;float sigma=GustVolumeAt(q,floorZ,g,true,d);
        if(sigma<=0)continue;
        // Out in the open air, and as far under cover as open air reaches (a doorway, an arch).
        float open=max(PrecipitationOpenSky(q),CoverReach(q).x);
        sigma*=region*open*show*(1-smoothstep(2400,3000,t));
        if(sigma<=0)continue;
        float a=1-exp(-sigma*dt);
        // The colour ramp: thin wisps toward the first colour, the dense body toward the second.
        float ramp=saturate(pow(d,g.rampPow)*g.rampMul+g.rampAdd);
        float3 albedo=lerp(g.thin,g.dense,ramp)*weatherColor.rgb/max(max(weatherColor.r,max(weatherColor.g,weatherColor.b)),.05);
        float3 light;
        // A HOUSE'S SHADOW ON A BANK IS SOFT. Light is scattered about inside blown snow or spray, so
        // the shade a house casts into it blurs over a few paces and never goes dark: read at one
        // point, the bank went from sun-white to grey along the exact line of the shadow on the floor
        // and looked cut off there, worst where a gust ran down a street half in shade (reported
        // 2026-11-18). Four reads across 48 units, and a quarter of the light kept.
        float shade=0;
        [loop]for(int k=0;k<4;++k)shade+=WorldSunShadow(q+sun*shadowInfo.y+float3(kSoftTaps[k*3]*48,0));
        shade=lerp(.25,1,shade*.25);
        [branch]if(g.lit>.5){
            // Lit as the snow under it (GustFloorLight), a little darker in its own shade (a short
            // march toward the sun, its density times `shadow`, softened by the shade above) and a
            // little brighter looking toward the sun.
            float tau=0,ds=12/max(sun.z,.3);
            [loop]for(int k=0;k<4;++k){float dk;tau+=GustVolumeAt(q+sun*ds*(k+.5),floorZ,g,false,dk);}
            light=GustFloorLight(q,floorZ)*(lerp(.8,1,exp(-tau*ds*g.shadow)*lerp(.6,1,shade))+Phase(dot(ray,sun))*.25*shade);
        }else{
            // Unlit: spray in a storm, lit by the sky with a little of the sun.
            light=sky*GroundFogSkyOpen(q)*1.1+sunLight*.35*shade;
        }
        float3 lit=albedo*light;
        // The height fog in front of it, as for the ground fog.
        [branch]if(CL_fogDensity>0&&CL_localFogRadius<=0){
            float along=max(0,t-CL_fogStart);
            float thick=CL_fogDensity*.001*.5*(exp(-max(0,camera.z-CL_fogBase)/CL_fogHeight)+exp(-max(0,q.z-CL_fogBase)/CL_fogHeight));
            lit=lerp((ambient+SunColor()*Phase(dot(ray,sun))*.25)*fogColorDensity.rgb,lit,exp(-thick*along));
        }
        color+=trans*a*lit;trans*=1-a;
        if(trans<.01)break;
    }
}
)HLSL"
R"HLSL(
// How a march places its samples in each step. STILL: one offset per volume texel, fixed, for the
// coarse sky march the blur smooths. BLENDED: the live march, jittered anew every frame and averaged
// by VolumeTemporalPS. EXACT: a full-resolution pixel the coarse volume has no match for (a crack,
// a near outline), sampled mid-step. Those used the coarse texel's offset, the same for a 4 x 4
// block and different for the next, so a thin layer seen through a crack came out as a row of
// fogged and clear squares (the checkerboard reported 2026-11-17).
static const int kMarchStill=0,kMarchBlended=1,kMarchExact=2;
float4 VolumeAt(float2 pixel,float d,int sampling=0) {
    bool sky=IsSky(d);
    float3 farPoint=World(pixel,camera.w>0?.00001:.99999);
    float3 ray=normalize(farPoint-camera.xyz);
    float distance=FogDistance(pixel,d);
    float3 color=0;float trans=1;int steps=(int)volumeDimensions.z;
    // A new offset every frame in the live view (temporalInfo.y), which VolumeTemporalPS averages.
    float jitter=sampling==kMarchExact?.5:frac(InterleavedGradient(floor(pixel*volumeDimensions.xy/dimensions.xy))+(sampling==kMarchBlended?temporalInfo.y:0));
    float3 ambient=(CL_fogDensity>0||CL_groundFog>0||CL_gusts>0)?FogAmbient():0;
    GroundFogMarch(ray,distance,jitter,ambient,color,trans);
    GustVolumeMarch(ray,distance,jitter,ambient,color,trans);
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
            // Exponential fog integrates toward the sun in closed form: density here times the scale
            // height over the sun's height, so deep, thick fog is lit by the sky more than the sun.
            float selfShadow=FogSunTransmittance(density*CL_fogHeight/max(SunDirection().z,.08));
            float3 illumination=ambient+SunColor()*Phase(dot(ray,SunDirection()))*.25*visibility*selfShadow;
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
        // Lit like the near interval (unshadowed sun), so the two meet without a brightness step.
        float3 farLight=ambient+SunColor()*Phase(dot(ray,SunDirection()))*.25;
        color+=trans*a*farLight*fogColorDensity.rgb;trans*=1-a;
    }
    if(CL_clouds>0&&!IsSkyboxGeometry(d)&&abs(ray.z)>.0001) {
        float t0=(CL_cloudBase-camera.z)/ray.z,t1=(CL_cloudBase+CL_cloudDepth-camera.z)/ray.z;
        float enter=max(0,min(t0,t1)),leave=min(distance,max(t0,t1));leave=min(leave,24000);
        if(leave>enter) {
            float dt=(leave-enter)/steps,cloudTrans=1;float3 cloudColor=0;
            float precipitation=CloudPrecipitation();
            float3 key=SunDirection(),keyColor=SunColor();
            float phase=CloudPhase(dot(ray,key));
            // The light toward the key light is marched to the top of the layer (6 steps, as
            // TerrainEngine-OpenGL's cone march), through the cheap shape without edge detail.
            float lightLength=min(CL_cloudDepth/max(key.z,.15),CL_cloudDepth*3)/6;
            float absorption=.0035*(1+2*precipitation)*CL_clouds;
            float darkness=.2*(1-.75*precipitation);
            // Sky light: the Environment sky's zenith and its horizon away from the sun (as the fogs
            // take it), or the previous fixed colours over the map's own sky. Brighter toward the
            // top of the layer (after HZD). Rain takes most of it away.
            float3 skyLight=lerp(float3(.10,.14,.22),float3(.32,.39,.5),saturate(CL_sunElevation/30));
            [branch]if(CL_sky>0){
                float3 sun=TrueSunDirection();
                float2 away=length(sun.xy)>1e-3?-normalize(sun.xy):float2(1,0);
                skyLight=(SkyColor(float3(0,0,1))+SkyColor(normalize(float3(away,.2))))*.5;
            }
            skyLight*=1-.65*precipitation;
            [loop] for(int k=0;k<steps;++k) {
                float3 p=camera.xyz+ray*(enter+(k+jitter)*dt);
                float density=CloudDensity(p,precipitation);
                if(density>.001) {
                    float shadow=0;
                    [loop]for(int sh=0;sh<6;++sh)shadow+=CloudDensity(p+key*lightLength*(sh+.5),precipitation,false);
                    float toLight=exp(-shadow*lightLength*absorption);
                    toLight=darkness+toLight*(1-darkness);
                    float h=saturate((p.z-CL_cloudBase)/CL_cloudDepth);
                    float3 light=skyLight*lerp(.55,1,h);
                    light+=keyColor*WorldSunShadow(p)*toLight*phase*(1-.5*precipitation);
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
    VolumeMarchOutput o;o.scene=VolumeAt(p,d,kMarchBlended);o.geometry=float2(FogDistance(p,d),IsSky(d));return o;
}
float2 VolumeGeometry(int2 q) {return volumeGeometryTexture.Load(int3(q,0));}
// THE VOLUME'S TEMPORAL BLEND (live view). The march is jittered differently every frame and each
// frame is blended into what the last frames found at the same place, so one march's grain averages
// out over about eight frames instead of crawling across the fog as the camera moves (reported
// 2026-11-17). Last frame's texel is found by putting this texel's surface (a far point for open sky)
// through last frame's view. It counts only where last frame saw the same surface there, and only
// within the range of this frame's march around the texel, so what moved leaves no trail.
float4 VolumeTemporalPS(Screen i):SV_Target{
    int2 q=int2(i.position.xy),size=int2(volumeDimensions.xy);
    float4 current=volumeTexture.Load(int3(q,0));
    [branch]if(temporalInfo.x<.5)return current;
    float2 geometry=VolumeGeometry(q);bool sky=geometry.y>0;
    float4 lo=current,hi=current;
    [unroll]for(int ny=-1;ny<=1;++ny)[unroll]for(int nx=-1;nx<=1;++nx){
        int2 t=clamp(q+int2(nx,ny),int2(0,0),size-1);
        float2 g=VolumeGeometry(t);
        if((g.y>0)!=sky||(!sky&&abs(g.x-geometry.x)>max(16,geometry.x*.05)))continue;
        float4 n=volumeTexture.Load(int3(t,0));lo=min(lo,n);hi=max(hi,n);
    }
    float2 pixel=(float2(q)+.5)*dimensions.xy/volumeDimensions.xy;
    float3 ray=normalize(World(pixel,camera.w>0?.00001:.99999)-camera.xyz);
    float3 w=camera.xyz+ray*min(geometry.x,24000);
    float4 clip=mul(previousViewProjection,float4(w,1));
    if(clip.w<=1e-3)return current;
    float2 uv=clip.xy/clip.w*float2(.5,-.5)+.5;
    if(any(uv<=0)||any(uv>=1))return current;
    float expected=length(w-previousCamera.xyz);
    float2 v=uv*volumeDimensions.xy-.5;int2 b=int2(floor(v));float2 f=v-float2(b);
    float4 sum=0;float weights=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
        int2 t=clamp(b+int2(x,y),int2(0,0),size-1);
        float2 g=volumeHistoryGeometry.Load(int3(t,0));
        float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        if((g.y>0)!=sky)weight=0;
        else if(!sky)weight*=exp(-abs(g.x-expected)/max(8,expected*.03));
        sum+=volumeHistoryTexture.Load(int3(t,0))*weight;weights+=weight;
    }
    if(weights<.25)return current;
    return lerp(clamp(sum/weights,lo,hi),current,temporalInfo.z);
}
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
// How much of the ray to maxT lies in front of the effect on it, from where effects are known to be.
// `known`: how sure, fading to 0 at an anchor's edge. Reference: FogEffectShare in EnvironmentGroundFogMath.h.
float FogEffectShare(float3 dir,float maxT,out float known){
    known=0;float sum=0,weight=0;
    int count=(int)effectAnchorCount.x;
    for(int i=0;i<count;++i){
        float r=effectAnchors[i].w;if(r<=0)continue;
        float3 o=effectAnchors[i].xyz-camera.xyz;
        float t=dot(o,dir);if(t<=0)continue;
        float w=1-smoothstep(r*.6,r,length(o-dir*t));
        if(w<=0)continue;
        known=max(known,w);
        sum+=w*max(t-r*.3,0);weight+=w;
    }
    return weight>0?saturate(sum/weight/max(maxT,1)):1;
}
// The ground fog's optical depth over the first `reach` units of a ray from the camera. Only for pixels
// with an effect on them, so eight even steps are enough for what is multiplied onto that effect.
float GroundFogDepthAlong(float3 dir,float reach){
    [branch] if(CL_groundFog<=0||reach<=0)return 0;
    float dt=reach/8,tau=0;
    [loop]for(int k=0;k<8;++k){
        float top,h,floorZ,held;
        tau+=GroundFogAt(camera.xyz+dir*dt*(k+.5),top,h,floorZ,held);
    }
    return tau*dt;
}
)HLSL"
R"HLSL(
// MSAA cost split, as for the atmosphere pass (AtmospherePS): the fog and cloud composite shaded
// every sample of every pixel. With shadowDebug.z set, CompositePixelPS shades the pixels whose
// samples lie on one surface once, and CompositePS keeps per-sample shading for the edge pixels.
// The split is used only while nothing after the atmosphere pass (props, precipitation) has given
// the samples of an interior pixel different colours.
bool CachedAtmosphereEdge(float2 pixel);
float4 Composite(float2 p,uint sample);
float4 CompositeAt(float2 p,uint sample,float d,float3 color);
// SHARED EXACT WALKS. The pixels the coarse volume has no match for (an outline: a pole, a roof
// edge, a player against the far street) are walked from the eye at full resolution, and a walk
// with blown snow in it is long. Walked inside the composite they were a thin line of pixels each
// holding up the GPU's whole group of neighbours, which had nothing to do: 8.5 ms of a rainy Dust 2
// frame went on the gust part of those walks (2026-11-19). And at an MSAA edge every sample of the
// pixel walked, two or four times the same ray.
// So the walks are listed first (ExactListPS: up to two per pixel, one per depth its samples
// stand at), walked packed side by side (ExactWalkPS), and read back by the composite
// (ExactShared). The sample that is listed walks exactly as the composite would have; the other
// samples at the same depth take its walk, a ray a fraction of a pixel away.
static const uint kExactWalkWidth=1024,kExactWalkRows=256,kExactWalkCapacity=kExactWalkWidth*kExactWalkRows;
Texture2D<uint4> exactIndex : register(t53);           // per pixel: walk number+1 of up to two walks, and each one's distance
Texture2D<float4> exactWalks : register(t54);          // the walks, kExactWalkWidth to a row
Buffer<uint> exactCount : register(t55);               // how many walks were listed (CopyStructureCount)
StructuredBuffer<uint4> exactEntries : register(t56);  // pixel x | y<<16, sample, depth bits
RWStructuredBuffer<uint4> exactEntriesOut : register(u1);
// Two distances count as the same surface within a part in 250 (a unit up close).
bool ExactSameDepth(float a,float b) {return abs(a-b)<=max(1,b*.004);}
// One return: called inside branches of the big composite (see GustVolumeAt).
bool ExactShared(float2 p,float distance,out float4 v) {
    uint4 e=exactIndex.Load(int3(int2(p),0));
    uint at=e.x&&ExactSameDepth(distance,asfloat(e.z))?e.x:e.y&&ExactSameDepth(distance,asfloat(e.w))?e.y:0;
    v=0;
    [branch]if(at>0){--at;v=exactWalks.Load(int3(at%kExactWalkWidth,at/kExactWalkWidth,0));}
    return at>0;
}
float4 CompositePS(Screen i,uint sample:SV_SampleIndex):SV_Target {
#if COLOR_MS
    [branch]if(shadowDebug.z>.5&&!CachedAtmosphereEdge(SamplePixel(i.position.xy,0)))discard;
#endif
    return Composite(SamplePixel(i.position.xy,sample),sample);
}
float4 CompositePixelPS(Screen i):SV_Target {
#if COLOR_MS
    float2 pixel=SamplePixel(i.position.xy,0);
    [branch]if(CachedAtmosphereEdge(pixel))discard;
    return Composite(pixel,0);
#else
    discard;
    return 0;
#endif
}
float4 Composite(float2 p,uint sample) {
    float d=ReadCombinedSample(p,sample);
    float3 color=ReadColor(p,sample);
    // An edge pixel against the sky (EdgeSkyShare) is two layers: the thing in front, and the sky
    // as drawn for the samples that missed it, each given its own clouds and fog. One copy of the
    // composite in a loop, so the shader does not carry it twice.
    float share=IsSky(d)?0:EdgeSkyShare(p);
    float3 sky=0;
    [branch]if(share>0){float found;sky=EdgeSkyColor(p,found,false);if(found<=0)share=0;}
    float3 surface=share>0?max((color-share*sky)/max(1-share,.125),0):color;
    float4 sum=0;
    [loop]for(uint layer=0;layer<(share>0?2u:1u);++layer)
        sum+=(layer?share:1-share)*CompositeAt(p,sample,layer?SkyDepth():d,layer?sky:surface);
    return sum;
}
// The coarse volume's texels around p that match its depth, weighted, and how much they weigh.
float4 CoarseVolume(float2 p,float d,float distance,out float weights) {
    float2 uv=p/dimensions.xy;
    bool sky=IsSky(d);
    float2 v=uv*volumeDimensions.xy-.5;int2 lo=int2(floor(v));float2 f=frac(v);
    float4 sum=0;weights=0;
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
    return sum;
}
float4 CompositeAt(float2 p,uint sample,float d,float3 color) {
    float2 uv=p/dimensions.xy;
    bool sky=IsSky(d);
    float distance=FogDistance(p,d);
    float weights;float4 sum=CoarseVolume(p,d,distance,weights);
    // A minority MSAA sample can have no representative in the coarse depth image.
    // Integrate that sample directly; never leave unclouded sky holes around a dark silhouette.
    // HLSL's conditional operator evaluates both operands. Keep the full raymarch
    // behind a real branch: it is needed only for unmatched depth/MSAA edges,
    // not for every sample of every full-resolution pixel.
    float4 volume;
    // (When the coarse texels that match this pixel's depth weigh next to nothing, their value is one
    // far corner's, and along the outline of anything close, a barrel a pace away, the fog was drawn
    // in the coarse image's squares. Those pixels are walked exactly as well.)
    bool exact=weights<=.2;
    volume=exact?float4(0,0,0,1):sum/weights;
    // FOG BEHIND A SMOKE. Where a CS2 smoke covers the pixel, only the fog BETWEEN the camera and
    // the smoke may show; the fog integrated past it (a deep doorway behind the cloud) used to be
    // painted over the smoke. That share is re-integrated up to where the smoke starts; with no
    // placed smokes to measure against, the covered share gets no added fog at all.
    // Only the share of the coverage that is a placed smoke the ray passes through: glass, flames
    // and leaf cards in the same OIT buffer keep the full fog of the surface behind them.
    float smoke=SmokeCover(p);
    float frontDepth=0;int frontKind=0;   // in front of the smoke: 0 clear air, 1 the surface's fog, 2 walked
    [branch] if(smoke>0) {
        float3 farPoint=World(p,sky?(camera.w>0?.00001:.99999):d);
        float3 dir=normalize(farPoint-camera.xyz);
        float entry;
        smoke*=SmokeRayShare(dir,distance,entry);
        if(entry>=distance) frontKind=1;   // the smoke starts beyond the surface
        else if(entry>1) {
            float4 clip=mul(viewProjection,float4(camera.xyz+dir*entry,1));
            frontDepth=clip.z/clip.w;frontKind=2;
        }
    }
    // The exact walks, this pixel's own (walk 0, when no coarse texel matched its depth) and the one
    // up to the smoke (walk 1), share one call in a loop, so the shader carries the long march once.
    float4 front=float4(0,0,0,1);
    [loop]for(uint walk=exact?0u:1u;walk<(smoke>0&&frontKind==2?2u:1u);++walk){
        // This pixel's own walk is usually waiting in the shared list (ExactListPS); only a sample
        // the list has no walk for (a third depth in one pixel, a full list) walks here.
        bool listed=false;float4 walked=0;
        [branch]if(!walk)listed=ExactShared(p,distance,walked);
        [branch]if(!listed)walked=VolumeAt(p,walk?frontDepth:d,kMarchExact);
        if(walk)front=walked;else volume=walked;
    }
    [branch] if(smoke>0) {
        if(frontKind==1) front=volume;
        volume=lerp(volume,front,smoke);
    }
    // EFFECTS IN THE FOG. A particle has no depth, so the fog above was measured to the surface behind
    // it. Only the fog between the camera and the effect belongs over it: all of it for a fire far down
    // a fog bank, which sinks in smoothly, hardly any for a close explosion cloud. So a cloud across a
    // depth edge (a wall and the far end of an alley behind it) carries the same fog on both sides.
    // Unplaced light (a tracer's far end) is taken as most of the way to the surface; anything else
    // unplaced, like glass, keeps the surface's fog: it must not stand out of the fog.
    // The player's own gun and arms stay readable however thick the fog around the camera is.
    [branch] if(IsViewmodelSample(p,sample)) volume=lerp(volume,float4(0,0,0,1),.85);
    else if(fogAmbient.w>.5){
        float2 effect=fogFrontMask.Load(int3(int2(p),0)).rg;
        // The frame only shows how much a cloud darkened what was behind it, and a cloud has some light
        // of its own, so it hides more than that. Over a dark background it shows nothing: there, what
        // the cloud hides close by counts for pixels dark enough to be under it.
        effect.g=max(saturate(effect.g*1.3),fogFrontNearby.SampleLevel(groundFogSampler,uv,0)*
                     (1-smoothstep(.06,.22,dot(color,float3(.2126,.7152,.0722)))));
        effect*=1-smoke;
        [branch] if(max(effect.r,effect.g)>.002){
            float3 farPoint=World(p,sky?(camera.w>0?.00001:.99999):d);
            float3 dir=normalize(farPoint-camera.xyz);
            float known;float along=FogEffectShare(dir,distance,known);
            float through=clamp(volume.a,1e-4,1);
            float kept=pow(through,effect.r>=effect.g?lerp(1,.7,smoothstep(.3,.8,effect.r)):1);
            // Placed: what really lies on the stretch to it. A blast clears the air around itself, so
            // the fog is not spread evenly along the ray. Height fog is, near enough.
            [branch] if(known>0)
                kept=lerp(kept,exp(-GroundFogDepthAlong(dir,min(along*distance,9000)))*
                               (CL_fogDensity>0?pow(through,along*(CL_groundFog>0?.5:1)):1),known);
            kept=max(kept,through);
            // The fog behind an effect is hidden as far as the effect hides things. What shows through
            // a thin cloud is the scene behind it, which keeps all its fog: only a nearly solid cloud's
            // own colour gets the lighter fog of the stretch in front of it.
            float dark=effect.g*effect.g;dark*=dark;
            // A placed flame's light is much brighter than the frame shows after tonemapping, so it
            // holds more of the pixel than its share says, and fog does not wash over it.
            float flame=saturate(effect.r*effect.r*(1+.4*known));
            volume=float4(lerp(volume.rgb,volume.rgb*(1-kept)/max(1-through,1e-4),max(flame,effect.g)),
                          lerp(volume.a,kept,max(flame,dark)));
        }
    }
    // Distant 3D-skybox buildings live in the leftover depth range; VolumeAt gives them the medium
    // out to kSkyboxFogDistance, so they sink into the fog with the sky behind them.
    return float4(color*volume.a+volume.rgb,1);
}
// The list (see ExactShared): every sample the composite will shade, through the same coarse test.
// Samples at one depth share a walk; a third depth in the pixel walks inside the composite.
uint4 ExactListPS(Screen i):SV_Target {
    uint n=1;
#if COLOR_MS
    uint w,h;sourceColor.GetDimensions(w,h,n);
    // Interior pixels are shaded once, from sample 0 (CompositePixelPS), while the split is on.
    if(shadowDebug.z>.5&&!CachedAtmosphereEdge(SamplePixel(i.position.xy,0)))n=1;
#endif
    uint4 o=0;
    [loop]for(uint s=0;s<n;++s){
        float2 p=SamplePixel(i.position.xy,s);
        float d=ReadCombinedSample(p,s);
        float distance=FogDistance(p,d),weights;
        CoarseVolume(p,d,distance,weights);
        bool known=(o.x&&ExactSameDepth(distance,asfloat(o.z)))||(o.y&&ExactSameDepth(distance,asfloat(o.w)));
        [branch]if(weights<=.2&&!known&&!o.y){
            uint at=exactEntriesOut.IncrementCounter();
            [branch]if(at<kExactWalkCapacity){
                exactEntriesOut[at]=uint4(uint(i.position.x)|(uint(i.position.y)<<16),s,asuint(d),0);
                if(o.x){o.y=at+1;o.w=asuint(distance);}else{o.x=at+1;o.z=asuint(distance);}
            }
        }
    }
    if(!o.x)discard;
    return o;
}
// The packed walks: a rectangle as many rows tall as the list needs.
Screen ExactWalkVS(uint id:SV_VertexID) {
    float rows=ceil(min(exactCount[0],kExactWalkCapacity)/float(kExactWalkWidth));
    float bottom=1-2*rows/kExactWalkRows;
    Screen o;o.position=float4(id==1||id==4||id==5?1:-1,id==2||id==3||id==5?bottom:1,0,1);
    return o;
}
float4 ExactWalkPS(Screen i):SV_Target {
    uint at=uint(i.position.y)*kExactWalkWidth+uint(i.position.x);
    float4 v=float4(0,0,0,1);
    [branch]if(at<min(exactCount[0],kExactWalkCapacity)){
        uint4 e=exactEntries[at];
        v=VolumeAt(SamplePixel(float2(e.x&0xffff,e.x>>16)+.5,e.y),asfloat(e.z),kMarchExact);
    }
    return v;
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
