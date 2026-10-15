// Shared by the native sky draw and the atmospheric/reflection passes.
R"HLSL(
float SkyHash(float3 p) {
    p=frac(p*.1031); p+=dot(p,p.yzx+33.33); return frac((p.x+p.y)*p.z);
}
float3 SkyRadiance(float3 ray,float3 direction,float3 sunlight,float elevation,
                   float haze,float stars,float visibility) {
    float day=smoothstep(-14,5,elevation),h=pow(saturate(ray.z),.4);
    float3 horizon=lerp(float3(.018,.022,.05),lerp(float3(.9,.45,.2),float3(.56,.7,.85),saturate(elevation/25)),day);
    float3 zenith=lerp(float3(.002,.004,.018),float3(.075,.26,.57),day);
    float3 color=lerp(horizon,zenith,h/(1+haze*.25));
    float sun=dot(ray,direction);
    color+=sunlight*visibility*(smoothstep(.99975,.99993,sun)*2+pow(saturate(sun),24)*.14*haze);
    float star=pow(saturate((SkyHash(floor(ray*1300))-.995)*200),8);
    return max(color+star*stars*(1-day)*smoothstep(0,.15,ray.z),0);
}
)HLSL"
