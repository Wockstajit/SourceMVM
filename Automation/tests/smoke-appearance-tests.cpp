#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/World/SmokeAppearance.h"
#include "../../Shared/AfxConsole.h"
#include "../../AfxHookSource2/Filmmaker/World/SmokeVolumeMath.h"
namespace Filmmaker::Environment {std::shared_ptr<const OcclusionMesh> CollisionGeometrySnapshot(){return {};}}
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <fstream>
#include <limits>
#include <cstdarg>
namespace Filmmaker {bool MvmDebugLog_Active(){return false;}void MvmDebugLog_LinefAlways(const char*,const char*,...){} }
namespace advancedfx {void NoConsole(const char*,...){}void (*Message)(const char*,...)=NoConsole;void (*Warning)(const char*,...)=NoConsole;}
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::SmokeAppearance;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
ComPtr<ID3DBlob> Compile(const std::string& source,const char* profile,bool optimized=false){ComPtr<ID3DBlob> code,error;
    const auto hr=D3DCompile(source.data(),source.size(),"SmokeAppearanceTest",nullptr,nullptr,"main",profile,
        (optimized?D3DCOMPILE_OPTIMIZATION_LEVEL3:D3DCOMPILE_SKIP_OPTIMIZATION)|D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"compile");return code;}
std::string Shader(bool compute,bool moments=false,bool geometry=false){return std::string(R"(
cbuffer Material:register(b0){float4 m[9];}
cbuffer View:register(b1){float4 view[29];}
)")+(compute?"cbuffer Volume:register(b4){float4 volume[156];} Texture3D<float4> tex:register(t13); RWTexture2D<float4> outImage:register(u0);":moments?"cbuffer Volume:register(b4){float4 volume[156];} Texture3D<float4> tex:register(t14);":"cbuffer Volume:register(b5){float4 volume[156];} Texture3D<float4> tex:register(t15);")+R"(
SamplerState samp:register(s0);
float4 value(){float extra=m[1].x+m[2].w+m[3].x+m[1].y+m[1].z+m[0].z+m[5].y+m[5].x;
  float4 appearance=float4(m[0].w,m[8].x,m[4].x+extra,1)+tex.SampleLevel(samp,(view[28].xyz+volume[155].xyz),0);
)"+(geometry?R"(
  uint id=(uint)view[28].w;
  float3 delta=float3(1041,2021,3021)-volume[id+32].xyz;
  float3 grid=delta*float3(.05,.05,.05)+float3(16,16,16);
  float3 bounds=volume[id+16].xyz-volume[id].xyz;
  return float4(bounds.x,grid.xy,1)+appearance*.000001;
}
)":"return appearance;}")+(compute?"[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){outImage[id.xy]=value();}":"float4 main():SV_Target{return value();}");}
// A native-like two-branch integrator, attached to the smoke ABI fixture.
// Its output is premultiplied colour: thinning must preserve RGB/alpha ratios.
std::vector<uint8_t> DensityFixture(ID3DBlob* source,unsigned branches=2){
    using namespace Filmmaker::Relight::IrradianceDxbc;
    const auto* src=static_cast<const uint8_t*>(source->GetBufferPointer());
    auto word=[&](size_t at){uint32_t n;std::memcpy(&n,src+at,4);return n;};
    const unsigned count=word(28);std::vector<uint8_t> result(src,src+32+count*4);
    auto put=[&](size_t at,uint32_t n){std::memcpy(result.data()+at,&n,4);};
    for(unsigned c=0;c<count;++c){size_t chunk=word(32+c*4),dest=result.size(),bytes=word(chunk+4);
        put(32+c*4,unsigned(dest));result.insert(result.end(),src+chunk,src+chunk+8);
        if(std::memcmp(src+chunk,"SHEX",4)&&std::memcmp(src+chunk,"SHDR",4))result.insert(result.end(),src+chunk+8,src+chunk+8+bytes);
        else {Words original(bytes/4);std::memcpy(original.data(),src+chunk+8,bytes);Words code{original[0],0};unsigned base=0;
            for(size_t at=2;at<original.size();){const unsigned op=original[at]&2047;const size_t length=(original[at]>>24)&127;
                if(op==104){base=original[at+1];code.insert(code.end(),{0x02000068u,base+2});}
                else if(op==62){
                    for(unsigned branch=0;branch<branches;++branch){unsigned r=base+branch;
                        Emit(code,56,{Dest(r,1),Literal(1,1,1),Cb(0,0,255)});
                        Emit(code,14,{Dest(r,1),Literal(.2f,.2f,.2f),Temp(r,0)});
                        Emit(code,14,{Dest(r,1),Literal(1,1,1),Temp(r,0)});
                        Emit(code,0x2000|56,{Dest(r,1),Temp(r,0),Literal(.1f,.1f,.1f)});
                        Emit(code,50,{Dest(r,2),Temp(r,0),Literal(-2,-2,-2),Literal(3,3,3)});
                        Emit(code,56,{Dest(r,1),Temp(r,0),Temp(r,0)});
                        Emit(code,56,{Dest(r,8),Temp(r,0),Temp(r,85)});
                        Emit(code,56,{Dest(r,4),Literal(.1f,.1f,.1f),Cb(0,6,255)});
                    }
                    Emit(code,0,{Dest(base,8),Temp(base,255),Temp(base,170)});
                    Words out{0x001020f2u,0};
                    Emit(code,56,{out,Temp(base,255),Literal(.3f,.5f,.8f,1)});Emit(code,62,{});
                }else code.insert(code.end(),original.begin()+at,original.begin()+at+length);
                at+=length;
            }code[1]=unsigned(code.size());put(dest+4,unsigned(code.size()*4));size_t end=result.size();result.resize(end+code.size()*4);std::memcpy(result.data()+end,code.data(),code.size()*4);
        }
    }put(24,unsigned(result.size()));Filmmaker::Environment::DxbcHash::ComputeHashRetail(result.data()+20,unsigned(result.size()-20),result.data()+4);return result;
}
// Use native-shaped dynamic CB operands. HLSL's debug compiler instead folds the
// array offset into a temporary, which deliberately falls outside this matcher.
std::vector<uint8_t> GridFixture(ID3DBlob* source){
    using namespace Filmmaker::Relight::IrradianceDxbc;
    const auto* src=static_cast<const uint8_t*>(source->GetBufferPointer());auto word=[&](size_t at){uint32_t n;std::memcpy(&n,src+at,4);return n;};
    unsigned count=word(28);std::vector<uint8_t> out(src,src+32+count*4);auto put=[&](size_t at,uint32_t n){std::memcpy(out.data()+at,&n,4);};
    for(unsigned c=0;c<count;++c){size_t chunk=word(32+c*4),dest=out.size(),bytes=word(chunk+4);put(32+c*4,unsigned(dest));out.insert(out.end(),src+chunk,src+chunk+8);
        if(std::memcmp(src+chunk,"SHEX",4)&&std::memcmp(src+chunk,"SHDR",4))out.insert(out.end(),src+chunk+8,src+chunk+8+bytes);
        else{Words original(bytes/4);std::memcpy(original.data(),src+chunk+8,bytes);Words code{original[0],0};unsigned first=0,table=5;Words target,coordinates;const bool compute=original[0]==0x50050;
            for(size_t at=2;at<original.size();){unsigned op=original[at]&2047;size_t length=(original[at]>>24)&127;
                if(op==89&&original[at+3]==156)table=original[at+2];
                if(op==104){first=original[at+1];code.insert(code.end(),{0x02000068u,first+3});}
                else if(op==164){size_t cursor=at+1;Operand a;Check(ReadOperand(original,cursor,at+length,a),"fixture UAV");target=Words(original.begin()+a.start,original.begin()+a.end);Check(ReadOperand(original,cursor,at+length,a),"fixture coordinates");coordinates=Words(original.begin()+a.start,original.begin()+a.end);}
                else if(op==62){auto cb=[&](unsigned row,bool negative=false){Words v{0x06208e46u,table,row,0x0010000au,first};if(negative){v[0]|=0x80000000u;v.insert(v.begin()+1,0x41u);}return v;};
                    Emit(code,54,{Dest(first,1),{0x00004001u,0}});
                    Emit(code,0,{Dest(first+1),Literal(1041,2021,3021),cb(32,true)});
                    Emit(code,50,{Dest(first+1),Temp(first+1),Literal(.05f,.05f,.05f),Literal(16,16,16)});
                    Emit(code,0,{Dest(first+2),cb(16),cb(0,true)});
                    Emit(code,54,{compute?Dest(first+2,1):Words{0x00102012u,0},Temp(first+2,0)});
                    Emit(code,54,{compute?Dest(first+2,6):Words{0x00102062u,0},Temp(first+1,0x90)});
                    auto neg=[](Words v){v[0]|=0x80000000u;v.insert(v.begin()+1,0x41u);return v;};
                    Emit(code,0,{compute?Dest(first+2,8):Words{0x00102082u,0},Cb(table,97,0),neg(Cb(table,96,0))});
                    if(compute){Check(!target.empty(),"fixture store opcode");Emit(code,164,{target,coordinates,Temp(first+2)});}Emit(code,62,{});
                }else code.insert(code.end(),original.begin()+at,original.begin()+at+length);at+=length;
            }code[1]=unsigned(code.size());put(dest+4,unsigned(code.size()*4));size_t start=out.size();out.resize(start+code.size()*4);std::memcpy(out.data()+start,code.data(),code.size()*4);
        }
    }put(24,unsigned(out.size()));Filmmaker::Environment::DxbcHash::ComputeHashRetail(out.data()+20,unsigned(out.size()-20),out.data()+4);return out;
}
void CheckGpu(bool compute,bool moments=false,bool densityFixture=false,bool geometryFixture=false,bool candidateFixture=false){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    auto hlsl=Shader(compute,moments);
    if(candidateFixture){hlsl="Texture2D<float4> candidates:register(t18); "+hlsl;hlsl.replace(hlsl.find("main():SV_Target{return value();}"),strlen("main():SV_Target{return value();}"),"main(float4 position:SV_Position,float3 ray:TEXCOORD0):SV_Target{return value()+candidates.Load(int3(ray.xy,0))+(position.x+ray.z)*.000001;}");}
    const auto source=Compile(hlsl,compute?"cs_5_0":"ps_5_0");
    std::vector<uint8_t> fixture;if(densityFixture)fixture=DensityFixture(source.Get());
    if(geometryFixture)fixture=GridFixture(source.Get());
    const void* bytecode=fixture.empty()?source->GetBufferPointer():fixture.data();size_t byteCount=fixture.empty()?source->GetBufferSize():fixture.size();
    std::vector<uint8_t> patched;unsigned reads=0;
    Check(Program(bytecode,byteCount,patched,&reads)&&reads>=10,"recognize smoke ABI and all controls");
    std::vector<uint8_t> rejected;
    Check(!Program(patched.data(),patched.size(),rejected),"reject already patched programs");
    auto occupied=Shader(compute,moments);
    occupied.insert(0,"cbuffer Owned:register(b11){float4 own;} ");
    occupied.replace(occupied.find("m[0].w,m[8].x"),strlen("m[0].w,m[8].x"),"m[0].w+own.x,m[8].x");
    const auto conflict=Compile(occupied,compute?"cs_5_0":"ps_5_0");
    Check(!Program(conflict->GetBufferPointer(),conflict->GetBufferSize(),rejected),"reject occupied appearance binding");
    ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11ComputeShader> cs;
    if(compute){Check(SUCCEEDED(device->CreateComputeShader(bytecode,byteCount,nullptr,&cs)),"native CS");NoteComputeShader(cs.Get(),bytecode,byteCount);ctx->CSSetShader(cs.Get(),nullptr,0);}
    else{Check(SUCCEEDED(device->CreatePixelShader(bytecode,byteCount,nullptr,&ps)),"native PS");NotePixelShader(ps.Get(),bytecode,byteCount);ctx->PSSetShader(ps.Get(),nullptr,0);}
    auto native=Constants(Parameters{});native[16]=.25f;native[27]=1;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=144;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{native.data(),0,0};ComPtr<ID3D11Buffer> material,sentinel;
    Check(SUCCEEDED(device->CreateBuffer(&bd,&init,&material))&&SUCCEEDED(device->CreateBuffer(&bd,&init,&sentinel)),"native buffers");
    ID3D11Buffer* b=material.Get();auto* saved=sentinel.Get();
    if(compute){ctx->CSSetConstantBuffers(0,1,&b);ctx->CSSetConstantBuffers(ConstantSlot,1,&saved);}
    else{ctx->PSSetConstantBuffers(0,1,&b);ctx->PSSetConstantBuffers(ConstantSlot,1,&saved);}
    ComPtr<ID3D11Buffer> grid;
    if(geometryFixture||candidateFixture){std::array<float,156*4> data{};
        for(unsigned axis=0;axis<3;++axis){data[axis]=float((axis+1)*1000)-320;data[16*4+axis]=float((axis+1)*1000)+320;data[32*4+axis]=float((axis+1)*1000);data[96*4+axis]=data[axis];data[97*4+axis]=data[16*4+axis];}
        if(candidateFixture){data.fill(0);data[3]=2;data[0]=data[1]=-1;data[2]=10;data[16*4]=data[16*4+1]=1;data[16*4+2]=20;
            data[4]=data[5]=data[6]=10;data[17*4]=data[17*4+1]=data[17*4+2]=11;}
        bd.ByteWidth=sizeof(data);D3D11_SUBRESOURCE_DATA volumeData{data.data(),0,0};Check(SUCCEEDED(device->CreateBuffer(&bd,&volumeData,&grid)),"fixture volume table");
        auto* volume=grid.Get();if(compute)ctx->CSSetConstantBuffers(4,1,&volume);else ctx->PSSetConstantBuffers(moments?4:5,1,&volume);
    }
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=compute?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target)),"target");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&staging)),"staging");
    ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
    if(compute){device->CreateUnorderedAccessView(target.Get(),nullptr,&uav);auto* v=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&v,nullptr);}
    else{device->CreateRenderTargetView(target.Get(),nullptr,&rtv);auto* v=rtv.Get();ctx->OMSetRenderTargets(1,&v,nullptr);
        auto vs=Compile("struct V{float4 position:SV_Position;float3 ray:TEXCOORD0;};V main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};V o;o.position=float4(p[id],0,1);o.ray=float3(0,0,1);return o;}","vs_5_0");
        ComPtr<ID3D11VertexShader> vertex;device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex);ctx->VSSetShader(vertex.Get(),nullptr,0);
        if(geometryFixture){const char name[]="smoke_volume.vfx_vs";vertex->SetPrivateData(WKPDID_D3DDebugObjectName,sizeof(name)-1,name);D3D11_BUFFER_DESC vd{};vd.ByteWidth=27*16;vd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;float view[27*4]{};view[4*4]=view[5*4+1]=view[6*4+2]=view[7*4+3]=1;D3D11_SUBRESOURCE_DATA initialView{view,0,0};ComPtr<ID3D11Buffer> viewBuffer;device->CreateBuffer(&vd,&initialView,&viewBuffer);auto* v=viewBuffer.Get();ctx->VSSetConstantBuffers(0,1,&v);ctx->GSSetConstantBuffers(0,1,&saved);}
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,1,1,0,1};ctx->RSSetViewports(1,&vp);}
    auto render=[&](){if(compute)ctx->Dispatch(1,1,1);else ctx->Draw(3,0);ctx->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"readback");
        std::array<float,4> pixel{};std::memcpy(pixel.data(),mapped.pData,sizeof(pixel));ctx->Unmap(staging.Get(),0);return pixel;};
    auto restored=[&](){ComPtr<ID3D11Buffer> actual,nativeMaterial;ComPtr<ID3D11DeviceChild> shader;
        if(compute){ctx->CSGetConstantBuffers(ConstantSlot,1,&actual);ctx->CSGetConstantBuffers(0,1,&nativeMaterial);ID3D11ComputeShader* p=nullptr;ctx->CSGetShader(&p,nullptr,nullptr);shader.Attach(p);}
        else{ctx->PSGetConstantBuffers(ConstantSlot,1,&actual);ctx->PSGetConstantBuffers(0,1,&nativeMaterial);ID3D11PixelShader* p=nullptr;ctx->PSGetShader(&p,nullptr,nullptr);shader.Attach(p);}
        Check(actual.Get()==sentinel.Get()&&nativeMaterial.Get()==material.Get(),"restore original CB11 and preserve CB0");
        Check(shader.Get()==(compute?static_cast<ID3D11DeviceChild*>(cs.Get()):static_cast<ID3D11DeviceChild*>(ps.Get())),"restore native shader");};
    Reset();const auto baseline=render();
    if(geometryFixture){
        if(std::abs(baseline[0]-640)>=.001f||std::abs(baseline[1]-18.05f)>=.001f)std::cerr<<"Native GPU output: "<<baseline[0]<<", "<<baseline[1]<<", "<<baseline[2]<<'\n';
        Check(std::abs(baseline[0]-640)<.001f&&std::abs(baseline[1]-18.05f)<.001f&&std::abs(baseline[3]-640)<.001f,"native volume coordinates and union bounds");
        Parameters p;Configure(true,p);NextFrame();{Scope scope(ctx.Get(),true,compute);Check(render()==baseline,"default volume override preserves native grid");}restored();
        p.spread=.5f;Configure(true,p);NextFrame();{Scope scope(ctx.Get(),true,compute);auto pixel=render();if(std::abs(pixel[0]-320)>=.001f||std::abs(pixel[1]-20.1f)>=.001f)std::cerr<<"Scaled GPU output: "<<pixel[0]<<", "<<pixel[1]<<", "<<pixel[2]<<'\n';Check(std::abs(pixel[0]-320)<.001f&&std::abs(pixel[1]-20.1f)<.001f,"bounds and density coordinates scale together");}restored();
        p.spread=2;p.resolution=8;
        ComPtr<ID3D11RasterizerState> clipped;
        if(!compute){D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;
            Check(SUCCEEDED(device->CreateRasterizerState(&rd,&clipped)),"scissor fixture");ctx->RSSetState(clipped.Get());D3D11_RECT rect{};ctx->RSSetScissorRects(1,&rect);}
        ComPtr<ID3D11DepthStencilState> clippedDepth;ComPtr<ID3D11DepthStencilView> stencilView;
        if(!compute){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
            ComPtr<ID3D11Texture2D> t;device->CreateTexture2D(&d,nullptr,&t);device->CreateDepthStencilView(t.Get(),nullptr,&stencilView);auto* color=rtv.Get();ctx->OMSetRenderTargets(1,&color,stencilView.Get());ctx->ClearDepthStencilView(stencilView.Get(),D3D11_CLEAR_STENCIL,1,0);
            D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.StencilEnable=TRUE;ds.StencilReadMask=ds.StencilWriteMask=255;ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;ds.BackFace.StencilFunc=D3D11_COMPARISON_NEVER;
            ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace.StencilFailOp=ds.BackFace.StencilDepthFailOp=ds.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
            Check(SUCCEEDED(device->CreateDepthStencilState(&ds,&clippedDepth)),"native stencil clipping fixture");ctx->OMSetDepthStencilState(clippedDepth.Get(),128);}
        if(!compute){ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);const float empty[4]{};ctx->ClearRenderTargetView(rtv.Get(),empty);Check(render()==std::array<float,4>{},"original occupancy stencil blocks enlarged drawing");}
        Configure(true,p);NextFrame();{Scope scope(ctx.Get(),true,compute);if(!compute){ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);Check(!gs,"grown main pass preserves native geometry stage");ComPtr<ID3D11DepthStencilState> actualDepth;UINT ref=0;ctx->OMGetDepthStencilState(&actualDepth,&ref);D3D11_DEPTH_STENCIL_DESC desc{};actualDepth->GetDesc(&desc);Check(!desc.StencilEnable&&ref==128,"enlarged smoke bypasses original occupancy stencil");
            ComPtr<ID3D11PixelShader> appearance,probe;ctx->PSGetShader(&appearance,nullptr,nullptr);auto rayCode=Compile("float4 main(float4 position:SV_Position,float3 ray:TEXCOORD0):SV_Target{return float4(ray+position.xxx*.000001,1);}","ps_5_0");device->CreatePixelShader(rayCode->GetBufferPointer(),rayCode->GetBufferSize(),nullptr,&probe);ctx->PSSetShader(probe.Get(),nullptr,0);const auto ray=render();
            Check(std::isfinite(ray[2])&&std::abs(ray[2]-1.f)<.0001f,"volume override preserves native vertex rays");ctx->PSSetShader(appearance.Get(),nullptr,0);}auto pixel=render();Check(std::abs(pixel[0]-1280)<.001f&&std::abs(pixel[1]-17.025f)<.001f&&std::abs(pixel[2]-16.525f)<.001f&&std::abs(pixel[3]-1280)<.001f,"grown union and volume keeps continuous local coordinates");}restored();
        if(!compute){ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);ComPtr<ID3D11Buffer> gsBuffer;ctx->GSGetConstantBuffers(0,1,&gsBuffer);Check(!gs&&gsBuffer.Get()==sentinel.Get(),"preserve native geometry stage and buffer");ComPtr<ID3D11DepthStencilState> actualDepth;UINT ref=0;ctx->OMGetDepthStencilState(&actualDepth,&ref);Check(actualDepth.Get()==clippedDepth.Get()&&ref==128,"restore original stencil state and reference");ctx->OMSetDepthStencilState(nullptr,0);auto* color=rtv.Get();ctx->OMSetRenderTargets(1,&color,nullptr);UINT n=1;D3D11_RECT rect{};ctx->RSGetScissorRects(&n,&rect);Check(n==1&&rect.right==0&&rect.bottom==0,"restore native scissor after grown smoke draw");ctx->RSSetState(nullptr);}
        p.spread=1;p.resolution=64;Configure(true,p);NextFrame();{Scope scope(ctx.Get(),true,compute);auto pixel=render();Check(std::abs(pixel[0]-640)<.001f&&std::abs(pixel[1]-18.05f)<.001f,"finer grid does not snap ray coordinates");}restored();
        Reset();{Scope scope(ctx.Get(),true,compute);Check(render()==baseline,"reset restores native volume");}restored();return;
    }
    if(candidateFixture){Parameters p;p.spread=2;Configure(true,p);NextFrame();{Scope scope(ctx.Get());const auto pixel=render();Check(std::abs(pixel[0]-baseline[0]-1)<.0001f,"grown ray mask excludes the off-ray volume");}restored();Reset();return;}
    if(densityFixture){
        for(float density:{.85f,.05f,0.f}){Parameters thin;thin.density=density;Configure(true,thin);NextFrame();Scope scope(ctx.Get(),true,false);const auto pixel=render();
            for(unsigned lane=0;lane<4;++lane)Check(std::isfinite(pixel[lane])&&std::abs(pixel[lane]-baseline[lane]*density/.85f)<.0001f,"thin density preserves premultiplied colour/opacity, including zero");
        }restored();Reset();return;
    }
    Check(std::abs(baseline[0]-.85f)<.001f,"native density output");
    Parameters settings;settings.density=2.4f;settings.wispy=.7f;settings.speed=0;Configure(true,settings);NextFrame();
    {Scope scope(ctx.Get(),true,compute);const auto pixel=render();Check(std::abs(pixel[0]-2.4f)<.001f&&std::abs(pixel[1]-.7f)<.001f,"density/wisp overrides reach GPU");
        Check(std::abs(pixel[2]-(baseline[2]-1.5f))<.001f,"speed override and untouched material lane");}
    restored();Check(render()==baseline,"native result restored after scoped draw");
    Configure(false,settings);NextFrame();{Scope scope(ctx.Get(),true,compute);Check(render()==baseline,"off preserves native rendering");}restored();
    Configure(true,settings);NextFrame();{Scope scope(ctx.Get(),false,compute);Check(render()==baseline,"excluded draw stays native");}restored();
    {Scope scope(ctx.Get(),true,compute);Check(std::abs(render()[0]-2.4f)<.001f,"Publish smoke settings for this frame");}restored();
    settings.density=1.3f;Configure(true,settings);
    {Scope scope(ctx.Get(),true,compute);Check(std::abs(render()[0]-2.4f)<.001f,"A same-frame edit preserves the published smoke settings");}restored();
    NextFrame();{Scope scope(ctx.Get(),true,compute);Check(std::abs(render()[0]-1.3f)<.001f,"updates reused constants");}restored();Reset();
}
void CheckGrowth(){
    using namespace Filmmaker::Environment;
    OcclusionMesh wall;wall.positions={{20,-1000,-1000},{20,1000,-1000},{20,1000,1000},{20,-1000,1000}};wall.indices={0,1,2,0,2,3};
    SurfaceRayGrid grid;Check(BuildSurfaceRayGrid(wall,grid),"build wall acceleration");
    // A camera-culling table is a view of the active atlas, not a new smoke simulation.
    std::vector<GrowthVolume> known{{{0,0,0},2,0},{{100,0,0},2,1}};
    const auto completed=known;
    MergeGrowthVolumes(known,{{{100.05f,0,0},2,1}});
    Check(known.size()==2&&GrowthReadyMask(completed,known)==3,"camera culling and rounded centers retain both collision fields");
    MergeGrowthVolumes(known,{{{0,0,0},2,0},{{100,0,0},2,1}});
    Check(GrowthReadyMask(completed,known)==3,"volume table reordering does not invalidate completed growth");
    MergeGrowthVolumes(known,{{{200,0,0},2,2}});
    Check(GrowthReadyMask(completed,known)==3&&GrowthWantedMask(known)==7,"new smoke keeps existing slots ready while its worker runs");
    MergeGrowthVolumes(known,{{{300,0,0},2,1}});
    Check(GrowthReadyMask(completed,known)==1,"reused atlas slot never samples an old grenade's collision field");
    for(auto& item:known)item.spread=1.5f;
    Check(GrowthReadyMask(completed,known)==0,"changed spread cannot reuse a differently scaled collision field");
    const GrowthVolume volume{{0,0,0},2,0};const int side=16;
    const auto sealed=GrowVolume(wall,grid,volume,side);
    auto at=[&](const std::vector<WarpCell>& cells,int x,int y,int z)->const WarpCell&{return cells[(z*side+y)*side+x];};
    Check(at(sealed,7,8,8).coverage==1,"source room remains reachable");
    Check(at(sealed,8,8,8).coverage==0&&at(sealed,10,8,8).coverage==0,"thin wall blocks neighbour growth and seeding");
    // A doorway in the same wall must let growth travel through and around its corner.
    OcclusionMesh doorway;
    auto quad=[&](float y0,float y1){unsigned start=unsigned(doorway.positions.size());
        doorway.positions.insert(doorway.positions.end(),{{20,y0,-1000},{20,y1,-1000},{20,y1,1000},{20,y0,1000}});
        doorway.indices.insert(doorway.indices.end(),{start,start+1,start+2,start,start+2,start+3});};
    quad(-1000,100);quad(260,1000);Check(BuildSurfaceRayGrid(doorway,grid),"doorway acceleration");
    const auto open=GrowVolume(doorway,grid,volume,side);
    Check(at(open,9,8,8).coverage==1,"growth reaches room through doorway");
    Check(at(open,9,8,8).x!=at(sealed,9,8,8).x||at(open,9,8,8).y!=at(sealed,9,8,8).y,"occluded cell follows collision-free path instead of stretching through wall");
    OcclusionMesh empty;SurfaceRayGrid absent;const auto free=GrowVolume(empty,absent,volume,side);
    const auto sample=at(free,9,8,8);Check(std::abs(sample.x*542/32-.59375f)<.0001f&&std::abs(sample.y-.53125f)<.0001f,"open-space mapping retains exact native scale");
    std::vector<WarpCell> packed;IdentityWarp(packed);
    auto cells=GrowVolume(empty,absent,GrowthVolume{{0,0,0},2,15});StoreWarp(packed,15,cells);
    Check(packed.size()==size_t(WarpWidth)*WarpSide*WarpSide&&std::isfinite(packed.back().x),"last packed volume and guards stay in bounds");
}
void CheckFilteredGpu(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"filter WARP");
    auto hlsl=std::string("Texture2D<float4> candidates:register(t18);Texture3D<float4> light:register(t14); ")+Shader(true);hlsl.replace(hlsl.find("return appearance;"),strlen("return appearance;"),"return appearance*.000001+tex.SampleLevel(samp,volume[154].xyz,0)+candidates.Load(int3(0,0,0))+light.SampleLevel(samp,volume[154].xyz,0)*volume[153].w;");
    const auto source=Compile(hlsl,"cs_5_0");std::vector<uint8_t> patched;ProgramInfo info;
    Check(Program(source->GetBufferPointer(),source->GetBufferSize(),patched,nullptr,&info)&&info.samples==3,"recognize actual atlas sampling");
    ComPtr<ID3D11ComputeShader> shader;Check(SUCCEEDED(device->CreateComputeShader(patched.data(),patched.size(),nullptr,&shader)),"filtered shader accepted");ctx->CSSetShader(shader.Get(),nullptr,0);
    std::vector<float> texels(542*32*32);
    for(unsigned z=0;z<32;++z)for(unsigned y=0;y<32;++y)for(unsigned x=0;x<542;++x)texels[(z*32+y)*542+x]=(x+.5f)*(x+.5f)/1024;
    D3D11_TEXTURE3D_DESC td{};td.Width=542;td.Height=td.Depth=32;td.MipLevels=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{texels.data(),542*sizeof(float),542*32*sizeof(float)};ComPtr<ID3D11Texture3D> texture;
    Check(SUCCEEDED(device->CreateTexture3D(&td,&initial,&texture)),"quadratic atlas");ComPtr<ID3D11ShaderResourceView> srv;device->CreateShaderResourceView(texture.Get(),nullptr,&srv);
    auto* view=srv.Get();ctx->CSSetShaderResources(13,1,&view);ctx->CSSetShaderResources(14,1,&view);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;device->CreateSamplerState(&sd,&sampler);auto* smp=sampler.Get();ctx->CSSetSamplers(0,1,&smp);
    D3D11_BUFFER_DESC bd{};bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.ByteWidth=156*16;ComPtr<ID3D11Buffer> table;
    device->CreateBuffer(&bd,nullptr,&table);auto* buffer=table.Get();ctx->CSSetConstantBuffers(4,1,&buffer);
    bd.ByteWidth=176;ComPtr<ID3D11Buffer> params;device->CreateBuffer(&bd,nullptr,&params);buffer=params.Get();ctx->CSSetConstantBuffers(ConstantSlot,1,&buffer);
    D3D11_TEXTURE2D_DESC out{};out.Width=out.Height=out.MipLevels=out.ArraySize=out.SampleDesc.Count=1;out.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;out.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> candidateTexture;auto candidateDesc=out;candidateDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;float candidateZero[4]{};D3D11_SUBRESOURCE_DATA candidateData{candidateZero,16,0};
    Check(SUCCEEDED(device->CreateTexture2D(&candidateDesc,&candidateData,&candidateTexture)),"candidate mask fixture");ComPtr<ID3D11ShaderResourceView> candidateView;device->CreateShaderResourceView(candidateTexture.Get(),nullptr,&candidateView);auto* candidateResource=candidateView.Get();ctx->CSSetShaderResources(18,1,&candidateResource);
    ComPtr<ID3D11Texture2D> target,staging;device->CreateTexture2D(&out,nullptr,&target);ComPtr<ID3D11UnorderedAccessView> uav;device->CreateUnorderedAccessView(target.Get(),nullptr,&uav);auto* output=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&output,nullptr);
    out.BindFlags=0;out.Usage=D3D11_USAGE_STAGING;out.CPUAccessFlags=D3D11_CPU_ACCESS_READ;device->CreateTexture2D(&out,nullptr,&staging);
    std::vector<WarpCell> identity;IdentityWarp(identity);
    D3D11_TEXTURE3D_DESC wd{};wd.Width=WarpWidth;wd.Height=wd.Depth=WarpSide;wd.MipLevels=1;wd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;wd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wi{identity.data(),WarpWidth*sizeof(WarpCell),WarpWidth*WarpSide*sizeof(WarpCell)};ComPtr<ID3D11Texture3D> warpTexture;Check(SUCCEEDED(device->CreateTexture3D(&wd,&wi,&warpTexture)),"identity warp texture");
    ComPtr<ID3D11ShaderResourceView> warpSrv;device->CreateShaderResourceView(warpTexture.Get(),nullptr,&warpSrv);auto* warpView=warpSrv.Get();ctx->CSSetShaderResources(WarpTextureSlot,1,&warpView);ctx->CSSetSamplers(WarpSamplerSlot,1,&smp);
    auto render=[&](float coordinate,unsigned resolution,bool useWarp=false,float spread=1.f,bool lighting=false){std::array<float,624> data{};data[3]=2.f;data[153*4+3]=lighting?1.f:0.f;data[154*4]=coordinate/542;data[154*4+1]=data[154*4+2]=.5f;
        Parameters p;p.spread=spread;p.resolution=float(resolution);auto constants=Constants(p);constants[42]=useWarp?1.f:0.f;constants[43]=useWarp?65535.f:0.f;ctx->UpdateSubresource(table.Get(),0,nullptr,data.data(),0,0);ctx->UpdateSubresource(params.Get(),0,nullptr,constants.data(),0,0);
        ctx->Dispatch(1,1,1);ctx->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE read{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&read)),"filtered readback");float value=*static_cast<float*>(read.pData);ctx->Unmap(staging.Get(),0);return value;};
    Check(std::abs(render(17,32,false,2)-render(17,32))<.0001f,"preserve native per-ray candidate masks");
    Check(std::abs(render(17,32,true)-render(17,32))<.0001f,"identity collision lookup preserves native density");
    Check(std::abs(render(17,8)-292.25f/1024)<.0001f,"coarse samples interpolate neighbouring centres");
    const float left=render(16-.001f,8),right=render(16+.001f,8);
    Check(right>left&&right-left<.0002f,"crossing former cell boundary stays continuous");
    Check(std::abs(render(17,64)-render(17,32))<.0001f,"fine resolution preserves smooth native field");
    const float nativeDensity=render(17,32),nativeLit=render(17,32,false,1,true);
    for(auto& cell:identity)cell.coverage=.5f;ctx->UpdateSubresource(warpTexture.Get(),0,nullptr,identity.data(),WarpWidth*sizeof(WarpCell),WarpWidth*WarpSide*sizeof(WarpCell));
    Check(std::abs(render(17,32,true)-nativeDensity*.5f)<.0001f,"collision boundary fades density frames");
    Check(std::abs(render(17,32,true,1,true)-(nativeLit-nativeDensity*.5f))<.0001f,"collision boundary retains lighting intensity");

    Check(std::isfinite(render(.01f,4))&&std::isfinite(render(541.99f,4)),"atlas guard edges stay finite");
}
int main(int argc,char** argv){CheckGrowth();CheckFilteredGpu();Parameters p;const auto old=p.density;
    // Panorama value tiles append quiet. Exercise the actual command parser so
    // accepted console edits cannot conceal rejected UI edits.
    for(const auto& control:Controls){
        advancedfx::CFakeCommandArgs args("mirv_filmmaker");
        for(const char* part:{"world","smoke","appearance",control.name,"0.4","quiet"})args.AddArg(part);
        Reset();RunCommand(args.ArgC(),&args);const auto state=Snapshot();
        const float expected=control.member==&Parameters::resolution?std::round(std::clamp(.4f,control.minimum,control.maximum)):std::clamp(.4f,control.minimum,control.maximum);
        Check(state.enabled&&std::abs(state.parameters.*(control.member)-expected)<.0001f,"Panorama quiet edits enable and update smoke");
    }
    Reset();
    Parameters cells;Check(VoxelSize(cells)==20&&VoxelCount(cells)==32768,"native grid size and count");
    Check(SetVoxelSize(cells,80)&&cells.resolution==8&&VoxelCount(cells)==512,"voxel size and count are linked");
    Check(!SetVoxelSize(cells,0)&&!SetVoxelSize(cells,std::numeric_limits<float>::infinity()),"invalid cell size leaves grid intact");
    advancedfx::CFakeCommandArgs voxelArgs("mirv_filmmaker");for(const char* part:{"world","smoke","appearance","voxel_size","40","quiet"})voxelArgs.AddArg(part);
    RunCommand(voxelArgs.ArgC(),&voxelArgs);Check(Snapshot().enabled&&Snapshot().parameters.resolution==16,"live voxel-size UI command");Reset();
    Check(!Set(p,Controls[0],std::numeric_limits<float>::quiet_NaN())&&p.density==old,"reject NaN without mutation");
    Check(Set(p,Controls[0],100)&&p.density==20,"bound finite parameters");
    Check(Set(p,Controls[0],0)&&Constants(p)[0]==0&&Constants(p)[3]==.85f,"zero density has finite native threshold and zero opacity factor");
    Check(Set(p,Controls[3],0)&&p.scaleLow==.05f,"prevent noise-scale division by zero");
    std::vector<uint8_t> output;Check(!Program(nullptr,0,output),"reject malformed container");
    const auto unrelated=Compile("float4 main():SV_Target{return 1;}","ps_5_0");
    Check(!Program(unrelated->GetBufferPointer(),unrelated->GetBufferSize(),output),"reject unrelated shaders");
    const auto nativeFixture=Compile(Shader(false),"ps_5_0");const auto partial=DensityFixture(nativeFixture.Get(),1);
    Check(!Program(partial.data(),partial.size(),output),"reject a partial native opacity/coverage match");
    CheckGpu(false,false,false,false,true);CheckGpu(false);CheckGpu(false,true);CheckGpu(true);CheckGpu(false,false,true);
    CheckGpu(false,false,false,true);CheckGpu(false,true,false,true);CheckGpu(true,false,false,true);
    for(int i=1;i<argc;++i){std::ifstream file(argv[i],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});unsigned reads=0;
        ProgramInfo extracted;const bool changed=Program(bytes.data(),bytes.size(),output,&reads,&extracted);Check(changed||extracted.composite,"recognize extracted native smoke shader");if(extracted.composite)output=bytes;
        ComPtr<ID3D11Device> device;D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr);
        ComPtr<ID3D11DeviceChild> shader;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11ComputeShader> cs;
        HRESULT hr=device->CreatePixelShader(output.data(),output.size(),nullptr,&ps);
        if(FAILED(hr))hr=device->CreateComputeShader(output.data(),output.size(),nullptr,&cs);
        ComPtr<ID3D11VertexShader> vs;if(FAILED(hr))hr=device->CreateVertexShader(output.data(),output.size(),nullptr,&vs);
        Check(SUCCEEDED(hr),"D3D11 accepts patched native bytecode");std::cout<<argv[i]<<": "<<reads<<" redirected reads\n";}
    std::cout<<"Smoke appearance GPU and restoration checks passed\n";
}
