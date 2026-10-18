#pragma once

// The lamp shadow atlas summary behind the lamp filter's exact early-out (NativeLampDisk in
// EnvironmentNativeShadowMath.h). Mip m texel (x, y) holds the nearest and farthest depth of atlas
// texels originX + (x << (m+2)) .. + (4 << m) - 1 by (y << (m+2)) .. + (4 << m) - 1, stored
// bit-inverted: uint2(~asuint(nearest), ~asuint(farthest)). Zero bits (an unbound view or an
// out-of-range read) therefore never pass for real data. A NaN depth makes its block unprovable:
// nearest -1 (no "lit" proof), farthest 2 (no "shadowed" proof).
//
// Only the rectangles written since the last update are rebuilt: one compute dispatch per mip
// covers every changed 8x8 cell of that mip.

#include "EnvironmentShaderCompiler.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::Environment {

inline constexpr char ShadowPyramidShader[]=R"HLSL(
cbuffer Level:register(b0){uint level;uint first;uint originX;uint pad;uint4 size;} // size.xy: this mip, .zw: its source
Texture2D<float> atlas:register(t0);
Texture2D<uint2> previous:register(t1);
Buffer<uint2> cells:register(t2);
RWTexture2D<uint2> output:register(u0);
[numthreads(8,8,1)]
void Main(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
    const uint2 p=cells[first+group.x]*8+thread.xy;
    if(p.x>=size.x||p.y>=size.y)return;
    float nearest=asfloat(0x7f800000),farthest=asfloat(0xff800000);
    if(level==0){
        const uint2 base=uint2(originX+p.x*4,p.y*4);bool invalid=false;
        [unroll]for(uint j=0;j<4;++j)[unroll]for(uint i=0;i<4;++i){
            const float d=atlas.Load(int3(min(base+uint2(i,j),size.zw-1),0));
            invalid=invalid||(asuint(d)&0x7fffffff)>0x7f800000;
            nearest=min(nearest,d);farthest=max(farthest,d);
        }
        if(invalid){nearest=-1;farthest=2;}
    }else{
        const uint2 c=p*2,last=size.zw-1;
        [unroll]for(uint j=0;j<2;++j)[unroll]for(uint i=0;i<2;++i){
            const uint2 v=~previous.Load(int3(min(c+uint2(i,j),last),0));
            nearest=min(nearest,asfloat(v.x));farthest=max(farthest,asfloat(v.y));
        }
    }
    output[p]=~uint2(asuint(nearest),asuint(farthest));
}
)HLSL";

struct ShadowPyramidPass {uint32_t level,first,originX,pad,width,height,sourceWidth,sourceHeight;};

// Mip `level` of a summary of `width` atlas texels.
inline unsigned ShadowPyramidWidth(unsigned width,unsigned level){return (std::max)(1u,((width+3)/4)>>level);}

// Collects the atlas rectangles written since the last update and turns them into each mip's
// changed cells.
class ShadowPyramidPlanner {
public:
    struct Level {unsigned first=0,count=0,width=0,height=0;};
    void Reset(unsigned atlasWidth,unsigned atlasHeight,unsigned originX,unsigned levels){
        m_width=atlasWidth>originX?atlasWidth-originX:0;m_height=atlasHeight;m_origin=originX;m_levels=levels;m_rects.clear();
    }
    // An atlas rectangle that changed. Parts left of originX are not summarized.
    void Add(unsigned x,unsigned y,unsigned width,unsigned height){
        if(x+width<=m_origin||!width||!height)return;
        if(x<m_origin){width-=m_origin-x;x=m_origin;}
        m_rects.push_back({x-m_origin,y,width,height});
    }
    bool Empty()const{return m_rects.empty();}
    // cells: (x, y) pairs of 8x8 cells, level after level. Clears the collected rectangles.
    void Build(std::vector<uint32_t>& cells,std::vector<Level>& levels){
        cells.clear();levels.assign(m_levels,Level{});
        for(unsigned level=0;level<m_levels;++level){
            auto& l=levels[level];l.first=unsigned(cells.size()/2);
            l.width=ShadowPyramidWidth(m_width,level);l.height=ShadowPyramidWidth(m_height,level);
            const unsigned columns=(l.width+7)/8,rows=(l.height+7)/8,shift=level+2;
            m_marks.assign(size_t(columns)*rows,0);
            for(const auto& r:m_rects){
                const unsigned x0=(std::min)(r.x>>shift,l.width-1),x1=(std::min)((r.x+r.width-1)>>shift,l.width-1);
                const unsigned y0=(std::min)(r.y>>shift,l.height-1),y1=(std::min)((r.y+r.height-1)>>shift,l.height-1);
                for(unsigned cy=y0/8;cy<=y1/8;++cy)for(unsigned cx=x0/8;cx<=x1/8;++cx){
                    auto& mark=m_marks[size_t(cy)*columns+cx];if(mark)continue;mark=1;
                    cells.push_back(cx);cells.push_back(cy);
                }
            }
            l.count=unsigned(cells.size()/2)-l.first;
        }
        m_rects.clear();
    }
private:
    struct Rect {unsigned x,y,width,height;};
    std::vector<Rect> m_rects;std::vector<uint8_t> m_marks;
    unsigned m_width=0,m_height=0,m_origin=0,m_levels=0;
};

// The summary texture and its update. Record works on immediate and deferred contexts alike.
class ShadowPyramidBuilder {
public:
    // A new summary summarizes the whole atlas on its first Record. False, with `error`, when a
    // resource or the shader cannot be made.
    bool Create(ID3D11Device* device,unsigned atlasWidth,unsigned atlasHeight,unsigned originX,unsigned levels,std::string& error){
        using Microsoft::WRL::ComPtr;
        *this=ShadowPyramidBuilder{};
        m_atlasWidth=atlasWidth;m_atlasHeight=atlasHeight;m_origin=originX;m_planner.Reset(atlasWidth,atlasHeight,originX,levels);
        const unsigned width=atlasWidth>originX?atlasWidth-originX:0;
        D3D11_TEXTURE2D_DESC d{};d.Width=ShadowPyramidWidth(width,0);d.Height=ShadowPyramidWidth(atlasHeight,0);
        d.MipLevels=levels;d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32_UINT;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(!device||!width||!levels||FAILED(device->CreateTexture2D(&d,nullptr,&m_texture))||
           FAILED(device->CreateShaderResourceView(m_texture.Get(),nullptr,&m_view))){error="texture";return Fail();}
        m_levels.resize(levels);m_writes.resize(levels);
        for(UINT level=0;level<levels;++level){
            D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=d.Format;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
            sr.Texture2D.MostDetailedMip=level;sr.Texture2D.MipLevels=1;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ua{};ua.Format=d.Format;ua.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;ua.Texture2D.MipSlice=level;
            if(FAILED(device->CreateShaderResourceView(m_texture.Get(),&sr,&m_levels[level]))||
               FAILED(device->CreateUnorderedAccessView(m_texture.Get(),&ua,&m_writes[level]))){error="views";return Fail();}
        }
        ComPtr<ID3DBlob> code,errors;
        if(FAILED(CompileEnvironmentShader(ShadowPyramidShader,sizeof(ShadowPyramidShader)-1,"EnvironmentShadowPyramid",nullptr,"Main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&code,&errors))||
           FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&m_shader))){
            error=errors?static_cast<const char*>(errors->GetBufferPointer()):"shader";return Fail();}
        D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(ShadowPyramidPass);b.Usage=D3D11_USAGE_DYNAMIC;
        b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(FAILED(device->CreateBuffer(&b,nullptr,&m_pass))){error="constants";return Fail();}
        m_device=device;m_planner.Add(originX,0,width,atlasHeight);
        return true;
    }
    bool Created()const{return m_texture!=nullptr;}
    void Mark(unsigned x,unsigned y,unsigned width,unsigned height){if(m_texture)m_planner.Add(x,y,width,height);}
    // Records the update of every rectangle marked since the last call, reading `atlas`. False on
    // failure: the summary is then stale and must no longer be bound.
    bool Record(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* atlas,std::string& error,unsigned* cellCount=nullptr){
        if(cellCount)*cellCount=0;
        if(!m_texture||!ctx||!atlas){error="not created";return false;}
        if(m_planner.Empty())return true;
        m_planner.Build(m_list,m_plan);
        const UINT entries=UINT(m_list.size()/2);
        if(entries>m_capacity){
            const UINT capacity=(std::max)(entries+entries/2,8192u);
            m_cells.Reset();m_cellView.Reset();
            D3D11_BUFFER_DESC b{};b.ByteWidth=capacity*8;b.Usage=D3D11_USAGE_DYNAMIC;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32G32_UINT;sr.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sr.Buffer.NumElements=capacity;
            if(FAILED(m_device->CreateBuffer(&b,nullptr,&m_cells))||FAILED(m_device->CreateShaderResourceView(m_cells.Get(),&sr,&m_cellView))){
                error="cell list";return Fail();}
            m_capacity=capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(m_cells.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))){error="cell upload";return Fail();}
        std::memcpy(mapped.pData,m_list.data(),size_t(entries)*8);ctx->Unmap(m_cells.Get(),0);
        ctx->CSSetShader(m_shader.Get(),nullptr,0);
        auto pass=m_pass.Get();ctx->CSSetConstantBuffers(0,1,&pass);
        for(unsigned level=0;level<m_plan.size();++level){
            const auto& l=m_plan[level];
            // Reads the level below; only one level is ever bound for writing.
            ID3D11ShaderResourceView* inputs[3]{level?nullptr:atlas,level?m_levels[level-1].Get():nullptr,m_cellView.Get()};
            ID3D11UnorderedAccessView* write=m_writes[level].Get();
            ctx->CSSetShaderResources(0,3,inputs);ctx->CSSetUnorderedAccessViews(0,1,&write,nullptr);
            for(unsigned done=0;done<l.count;){
                const unsigned groups=(std::min)(l.count-done,65535u);
                const ShadowPyramidPass constants{level,l.first+done,m_origin,0,l.width,l.height,
                    level?m_plan[level-1].width:m_atlasWidth,level?m_plan[level-1].height:m_atlasHeight};
                if(FAILED(ctx->Map(m_pass.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))){error="pass upload";return Fail();}
                std::memcpy(mapped.pData,&constants,sizeof(constants));ctx->Unmap(m_pass.Get(),0);
                ctx->Dispatch(groups,1,1);done+=groups;
            }
            ID3D11UnorderedAccessView* none=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&none,nullptr);
        }
        ID3D11ShaderResourceView* none[3]{};ctx->CSSetShaderResources(0,3,none);
        if(cellCount)*cellCount=entries;
        return true;
    }
    ID3D11ShaderResourceView* View()const{return m_view.Get();}
private:
    bool Fail(){m_texture.Reset();m_view.Reset();return false;}
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_view,m_cellView;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_levels;std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> m_writes;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_shader;Microsoft::WRL::ComPtr<ID3D11Buffer> m_cells,m_pass;
    UINT m_capacity=0;unsigned m_atlasWidth=0,m_atlasHeight=0,m_origin=0;
    ShadowPyramidPlanner m_planner;std::vector<uint32_t> m_list;std::vector<ShadowPyramidPlanner::Level> m_plan;
};

} // namespace Filmmaker::Environment
