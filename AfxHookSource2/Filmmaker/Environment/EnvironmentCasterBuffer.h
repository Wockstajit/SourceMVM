#pragma once
#include "EnvironmentCasterBufferMath.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <utility>
namespace Filmmaker::Environment {
// Render-thread storage for native stream-output captures. Growing retains the
// captured prefix, including the preceding frame's independently published pose.
inline bool EnsureCasterBuffer(ID3D11Device* device,ID3D11DeviceContext* context,
    Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,uint32_t required,uint32_t retained,
    uint32_t stride,uint32_t limit) {
    const auto capacity=CasterBufferCapacity(required,limit);
    if(!device||!capacity||stride<12||stride%4||uint64_t(capacity)*stride>UINT32_MAX)return false;
    D3D11_BUFFER_DESC old{};if(buffer)buffer->GetDesc(&old);
    if(retained>required||uint64_t(retained)*stride>old.ByteWidth)return false;
    if(old.ByteWidth>=uint64_t(required)*stride)return true;
    if(retained&&!context)return false;
    D3D11_BUFFER_DESC d{};d.ByteWidth=capacity*stride;
    d.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    Microsoft::WRL::ComPtr<ID3D11Buffer> grown;
    if(FAILED(device->CreateBuffer(&d,nullptr,&grown)))return false;
    if(retained){const D3D11_BOX box{0,0,0,retained*stride,1,1};
        context->CopySubresourceRegion(grown.Get(),0,0,0,0,buffer.Get(),0,&box);}
    buffer=std::move(grown);return true;
}
}
