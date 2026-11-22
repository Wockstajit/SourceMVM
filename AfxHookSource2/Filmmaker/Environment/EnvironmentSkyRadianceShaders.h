// Shared by the native sky draw and the atmospheric/reflection passes.
// C++ twin and sources: EnvironmentSkyMath.h (change both together).
//  - Atmosphere: wwwtyro/glsl-atmosphere (The Unlicense), single-scattering Rayleigh + Mie march.
//  - Night sky and moon: Jensen et al., "A Physically-Based Night Sky Model" (SIGGRAPH 2001):
//    moonlit sky from the same scattering lit by the moon, a lit-sphere moon with the
//    Lommel-Seeliger law, and earthshine on its dark side.
R"HLSL(
float SkyHash(float3 p) {
    p=frac(p*.1031); p+=dot(p,p.yzx+33.33); return frac((p.x+p.y)*p.z);
}
float SkyNoise(float3 p) {
    float3 a=floor(p),f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(lerp(SkyHash(a),SkyHash(a+float3(1,0,0)),f.x),lerp(SkyHash(a+float3(0,1,0)),SkyHash(a+float3(1,1,0)),f.x),f.y),
        lerp(lerp(SkyHash(a+float3(0,0,1)),SkyHash(a+float3(1,0,1)),f.x),lerp(SkyHash(a+float3(0,1,1)),SkyHash(a+1),f.x),f.y),f.z);
}
struct SkyLights {
    float3 sun;        // direction to the sun
    float3 sunTint;    // authored sun colour (multiplies the sky as it does the light)
    float3 sunlight;   // sun disc radiance
    float sunElevation,haze,stars,visibility;
    float3 moon;       // direction to the moon
    float moonRadius;  // angular radius, radians
    float3 moonTint;
    float moonBrightness,moonPhase;
};
static const float kSkyPlanet=6371e3,kSkyAtmosphere=6471e3,kSkyEye=1e3;
static const float3 kSkyRayleigh=float3(5.5e-6,13.0e-6,22.4e-6);
static const float kSkyMie=21e-6,kSkyRayleighHeight=8e3,kSkyMieHeight=1.2e3,kSkyMieG=.758,kSkySun=22;
static const float kSkyExposure=.75,kSkyMoonScale=.07;
float2 SkySphere(float3 o,float3 d,float r) {
    float b=2*dot(d,o),c=dot(o,o)-r*r,q=b*b-4*c;
    if(q<0)return float2(1e5,-1e5);
    q=sqrt(q);return float2(-b-q,-b+q)*.5;
}
float2 SkyPhases(float mu) {
    float gg=kSkyMieG*kSkyMieG;
    return float2(3/(16*3.14159265)*(1+mu*mu),
                  3/(8*3.14159265)*((1-gg)*(mu*mu+1))/(pow(max(1+gg-2*mu*kSkyMieG,1e-4),1.5)*(2+gg)));
}
// The light-ray half of the march: optical depth from a point to the top of the atmosphere.
float2 SkyLightDepth(float3 pos,float3 light,int steps) {
    float lightStep=SkySphere(pos,light,kSkyAtmosphere).y/steps;
    float2 depth=0;
    [loop]for(int j=0;j<steps;++j){
        float h=length(pos+light*((j+.5)*lightStep))-kSkyPlanet;
        depth+=float2(exp(-h/kSkyRayleighHeight),exp(-h/kSkyMieHeight))*lightStep;
    }
    return depth;
}
// One march along the view ray serves the sun and, at night, the moon (its own light ray).
// `fast` halves the steps for secondary uses (reflections, fog and cloud ambient).
void SkyScatter(float3 ray,float3 sun,float3 moon,bool withMoon,float mieScale,bool fast,out float3 sunScatter,out float3 moonScatter) {
    sunScatter=0;moonScatter=0;
    ray.z=max(ray.z,.01);ray=normalize(ray);
    float3 origin=float3(0,0,kSkyPlanet+kSkyEye);
    float2 p=SkySphere(origin,ray,kSkyAtmosphere);
    if(p.x>p.y)return;
    // The eye is inside the atmosphere: march from it, and stop at the ground only when it is ahead.
    float ground=SkySphere(origin,ray,kSkyPlanet).x;
    if(ground>0)p.y=min(p.y,ground);
    p.x=max(p.x,0);
    int viewSteps=fast?8:16,lightSteps=fast?4:8;
    float step=(p.y-p.x)/viewSteps,kM=kSkyMie*mieScale,odR=0,odM=0;
    float3 sunR=0,sunM=0,moonR=0,moonM=0;
    [loop]for(int i=0;i<viewSteps;++i){
        float3 pos=origin+ray*(p.x+(i+.5)*step);
        float h=length(pos)-kSkyPlanet;
        float dR=exp(-h/kSkyRayleighHeight)*step,dM=exp(-h/kSkyMieHeight)*step;
        odR+=dR;odM+=dM;
        float2 l=SkyLightDepth(pos,sun,lightSteps);
        float3 attenuation=exp(-(kM*(odM+l.y)+kSkyRayleigh*(odR+l.x)));
        sunR+=attenuation*dR;sunM+=attenuation*dM;
        [branch]if(withMoon){
            l=SkyLightDepth(pos,moon,lightSteps);
            attenuation=exp(-(kM*(odM+l.y)+kSkyRayleigh*(odR+l.x)));
            moonR+=attenuation*dR;moonM+=attenuation*dM;
        }
    }
    float2 sunPhase=SkyPhases(dot(ray,sun)),moonPhase=SkyPhases(dot(ray,moon));
    sunScatter=sunPhase.x*kSkyRayleigh*sunR+sunPhase.y*kM*sunM;
    moonScatter=moonPhase.x*kSkyRayleigh*moonR+moonPhase.y*kM*moonM;
}
// The moon's surface: dark maria and brighter highlands over the lit sphere (n in the moon's frame).
float MoonAlbedo(float3 n) {
    float maria=SkyNoise(n*2.3+4.7)*.6+SkyNoise(n*4.7-2.1)*.4;
    float detail=SkyNoise(n*11+1.3)*.6+SkyNoise(n*23-5.9)*.4;
    return lerp(.95,.55,smoothstep(.46,.62,maria))*(.82+.36*detail);
}
// The moon's disc over the sky: rgb radiance, a = coverage (hides the stars behind it).
float4 MoonDisc(float3 ray,SkyLights l) {
    float3 m=l.moon;
    if(l.moonBrightness<=0||m.z<-.05||dot(ray,m)<cos(l.moonRadius*1.05))return 0;
    float3 right=cross(m,float3(0,0,1));
    right=dot(right,right)>1e-6?normalize(right):float3(1,0,0);
    float3 up=cross(right,m);
    float2 q=float2(dot(ray,right),dot(ray,up))/tan(max(l.moonRadius,1e-4));
    float r2=dot(q,q);
    if(r2>=1)return 0;
    float3 n=float3(q,-sqrt(1-r2));                         // facing the viewer, in (right, up, m)
    float phase=l.moonPhase*6.2831853;
    float3 toSun=float3(sin(phase),0,cos(phase));           // 0 new (lit from behind), 0.5 full
    float ci=dot(n,toSun),ce=-n.z;
    float lommelSeeliger=ci>0?ci/(ci+ce):0;
    float earthshine=.012;
    float3 radiance=l.moonTint*l.moonBrightness*(lommelSeeliger*2+earthshine)*MoonAlbedo(n);
    float edge=1-smoothstep(.93,1,r2);
    float horizon=smoothstep(-.05,.03,m.z);
    return float4(radiance*edge*horizon,edge*horizon);
}
float3 SkyRadiance(float3 ray,SkyLights l,bool fast=false) {
    float day=smoothstep(-14,5,l.sunElevation);
    float moonGain=l.moonBrightness*(.5-.5*cos(l.moonPhase*6.2831853))*smoothstep(-.02,.05,l.moon.z)*(1-day);
    float3 sunScatter,moonScatter;
    SkyScatter(ray,l.sun,l.moon,moonGain>1e-4,max(l.haze,0)/.4,fast,sunScatter,moonScatter);
    float3 color=kSkyExposure*kSkySun*(sunScatter*l.sunTint+moonScatter*l.moonTint*moonGain*kSkyMoonScale);
    // Airglow and starlight under everything at night (the previous procedural sky's night gradient).
    float h=pow(saturate(ray.z),.4);
    color+=lerp(float3(.018,.022,.05),float3(.002,.004,.018),h)*(1-day);
    float sun=dot(ray,l.sun);
    color+=l.sunlight*l.visibility*smoothstep(.99975,.99993,sun)*2;
    float4 moon=MoonDisc(ray,l);
    color+=moon.rgb;
    float star=pow(saturate((SkyHash(floor(ray*1300))-.995)*200),8);
    color+=star*l.stars*(1-day)*smoothstep(0,.15,ray.z)*(1-moon.a);
    return max(color,0);
}
)HLSL"
