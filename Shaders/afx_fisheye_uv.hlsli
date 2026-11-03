// Shared D3D11/D3D9 mapping: each corner anchors itself, with a slight centre zoom.
float2 AfxFisheyeUv(float2 uv,float4 params) {
    float2 c=uv-.5;
    c.x*=params.z;
    float r2=dot(c,c),corner=.25*(params.z*params.z+1);
    c*=(1+r2*(params.x+params.y*r2))/(1+corner*(params.x+params.y*corner));
    c.x/=params.z;
    return c+.5;
}
