#pragma once
namespace Filmmaker::Environment {
inline constexpr char IndirectCasterArgumentsShader[]=R"(
ByteAddressBuffer nativeArguments:register(t0);
RWByteAddressBuffer output:register(u0);
cbuffer Settings:register(b0){uint draws,stride,capacity,indexed;}
groupshared uint totals[64],errors[64];
[numthreads(64,1,1)]
void Main(uint lane:SV_GroupIndex){
    uint total=0,bad=0;
    for(uint i=lane;i<draws;i+=64){uint2 a=nativeArguments.Load2(i*stride);
        if(a.y){
            if(a.x%3||a.x>capacity/max(a.y,1)){bad=1;continue;}
            uint n=a.x*a.y;
            if(n>capacity-total){bad=1;continue;}
            total+=n;
        }
    }
    totals[lane]=total;errors[lane]=bad;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint width=32;width;width>>=1){
        if(lane<width){errors[lane]|=errors[lane+width];
            if(totals[lane+width]>capacity-totals[lane])errors[lane]=1;
            else totals[lane]+=totals[lane+width];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0){uint valid=errors[0]==0,n=valid?totals[0]:0;
        // DrawInstancedIndirect: current captured vertices, one instance.
        output.Store4(0,uint4(n,1,0,0));
        output.Store4(16,uint4(0,0,0,valid));
    }
}
)";
}
