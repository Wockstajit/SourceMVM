#define NOMINMAX
#include "EnvironmentRayBackend.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <stdexcept>
#include "EnvironmentRayQueryBytecode.h"
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
namespace {
void Require(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(uint32_t(hr)));
}
D3D12_RESOURCE_BARRIER Uav(ID3D12Resource *resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    return b;
}
} // namespace
struct RayBackend::Impl {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> bottom, top;
    HANDLE event = nullptr;
    UINT64 serial = 0;
    bool failed = false;
    ~Impl() {
        if (event)
            CloseHandle(event);
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                  const void *data = nullptr) {
        D3D12_HEAP_PROPERTIES h{};
        h.Type = heap;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        Require(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                IID_PPV_ARGS(&resource)),
                "Create ray buffer");
        if (data) {
            void *p = nullptr;
            D3D12_RANGE empty{};
            Require(resource->Map(0, &empty, &p), "Map ray upload");
            memcpy(p, data, size_t(bytes));
            resource->Unmap(0, nullptr);
        }
        return resource;
    }
    void Reset() {
        if (failed)
            throw std::runtime_error("Ray device requires reinitialization after submission failure");
        Require(allocator->Reset(), "Reset ray allocator");
        Require(list->Reset(allocator.Get(), nullptr), "Reset ray list");
    }
    void Submit() {
        Require(list->Close(), "Close ray list");
        ID3D12CommandList *lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        failed = true;
        Require(queue->Signal(fence.Get(), ++serial), "Signal ray fence");
        if (fence->GetCompletedValue() < serial) {
            Require(fence->SetEventOnCompletion(serial, event), "Set ray fence event");
            // Resources must remain alive until the queue finishes. This wait runs only on the dedicated
            // worker.
            while (WaitForSingleObject(event, 1000) == WAIT_TIMEOUT)
                Require(device->GetDeviceRemovedReason(), "Ray device removed");
        }
        Require(device->GetDeviceRemovedReason(), "Ray execution");
        failed = false;
    }
};
RayBackend::RayBackend() : impl(new Impl) {}
RayBackend::~RayBackend() = default;
bool RayBackend::Begin(IDXGIAdapter *adapter, std::string &error) {
    try {
        auto next = std::make_unique<Impl>();
        auto &s = *next;
        Require(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s.device)),
                "Create DXR device");
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
        Require(s.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)),
                "Query DXR support");
        if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
            throw std::runtime_error("DXR 1.1 is unavailable on this adapter");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Require(s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue)), "Create ray queue");
        Require(s.device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&s.allocator)), "Create ray allocator");
        Require(s.device->CreateCommandList(0, q.Type, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list)),
                "Create ray list");
        Require(s.list->Close(), "Close initial list");
        Require(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)), "Create ray fence");
        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s.event)
            throw std::runtime_error("Create ray event failed");
        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1].Descriptor.ShaderRegister = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[2].Descriptor.ShaderRegister = 0;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[3].Constants.Num32BitValues = 1;
        D3D12_ROOT_SIGNATURE_DESC r{};
        r.NumParameters = 4;
        r.pParameters = p;
        ComPtr<ID3DBlob> blob, errors;
        Require(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
                "Serialize ray root");
        Require(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&s.root)),
                "Create ray root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = s.root.Get();
        ps.CS = {g_EnvironmentRayQuery, sizeof(g_EnvironmentRayQuery)};
        Require(s.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&s.pipeline)),
                "Create ray query pipeline");
        impl = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool RayBackend::Build(const OcclusionMesh &mesh, std::string &error) {
    try {
        auto &s = *impl;
        if (!s.device)
            throw std::runtime_error("Ray backend is not initialized");
        if (mesh.positions.size() < 3 || mesh.indices.empty() || mesh.indices.size() % 3 ||
            mesh.positions.size() > 32000000 || mesh.indices.size() > 96000000)
            throw std::runtime_error("Invalid ray geometry size");
        for (auto p : mesh.positions)
            if (!Finite(p))
                throw std::runtime_error("Nonfinite ray vertex");
        for (auto i : mesh.indices)
            if (i >= mesh.positions.size())
                throw std::runtime_error("Ray index out of bounds");
        auto vertices =
            s.Buffer(mesh.positions.size() * sizeof(Vec3), D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mesh.positions.data());
        auto indices =
            s.Buffer(mesh.indices.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                     D3D12_RESOURCE_FLAG_NONE, mesh.indices.data());
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        auto &t = geometry.Triangles;
        t.VertexBuffer = {vertices->GetGPUVirtualAddress(), sizeof(Vec3)};
        t.VertexCount = UINT(mesh.positions.size());
        t.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        t.IndexBuffer = indices->GetGPUVirtualAddress();
        t.IndexCount = UINT(mesh.indices.size());
        t.IndexFormat = DXGI_FORMAT_R32_UINT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &geometry;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        s.device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        if (!info.ResultDataMaxSizeInBytes)
            throw std::runtime_error("Invalid bottom-level prebuild size");
        auto bottom = s.Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto scratch =
            s.Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        s.Reset();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs = in;
        build.DestAccelerationStructureData = bottom->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        s.list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        auto barrier = Uav(bottom.Get());
        s.list->ResourceBarrier(1, &barrier);
        s.Submit();
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        instance.InstanceMask = 255;
        instance.AccelerationStructure = bottom->GetGPUVirtualAddress();
        auto instances = s.Buffer(sizeof(instance), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                  D3D12_RESOURCE_FLAG_NONE, &instance);
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.InstanceDescs = instances->GetGPUVirtualAddress();
        s.device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        if (!info.ResultDataMaxSizeInBytes)
            throw std::runtime_error("Invalid top-level prebuild size");
        auto top = s.Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scratch = s.Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        s.Reset();
        build.Inputs = in;
        build.DestAccelerationStructureData = top->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        s.list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        barrier = Uav(top.Get());
        s.list->ResourceBarrier(1, &barrier);
        s.Submit();
        s.bottom = std::move(bottom);
        s.top = std::move(top);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool RayBackend::Trace(const std::vector<HardwareRay> &rays, std::vector<HardwareHit> &hits,
                       std::string &error) {
    try {
        auto &s = *impl;
        if (!s.top)
            throw std::runtime_error("Ray geometry is not built");
        if (rays.empty()) {
            hits.clear();
            error.clear();
            return true;
        }
        if (rays.size() > 1048576)
            throw std::runtime_error("Ray batch exceeds budget");
        for (auto &r : rays)
            if (!Finite(r.origin) || !Finite(r.direction) || !std::isfinite(r.minimum) ||
                !std::isfinite(r.maximum) || r.minimum < 0 || r.maximum <= r.minimum ||
                std::abs(Dot(r.direction, r.direction) - 1) > .001f)
                throw std::runtime_error("Invalid ray interval or direction");
        auto input = s.Buffer(rays.size() * sizeof(HardwareRay), D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, rays.data());
        UINT64 bytes = rays.size() * sizeof(HardwareHit);
        auto output = s.Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = s.Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        s.Reset();
        s.list->SetPipelineState(s.pipeline.Get());
        s.list->SetComputeRootSignature(s.root.Get());
        s.list->SetComputeRootShaderResourceView(0, s.top->GetGPUVirtualAddress());
        s.list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress());
        s.list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
        s.list->SetComputeRoot32BitConstant(3, UINT(rays.size()), 0);
        s.list->Dispatch(UINT((rays.size() + 63) / 64), 1, 1);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        s.list->ResourceBarrier(1, &b);
        s.list->CopyResource(readback.Get(), output.Get());
        s.Submit();
        void *p = nullptr;
        D3D12_RANGE range{0, SIZE_T(bytes)};
        Require(readback->Map(0, &range, &p), "Read ray hits");
        hits.resize(rays.size());
        memcpy(hits.data(), p, size_t(bytes));
        D3D12_RANGE empty{};
        readback->Unmap(0, &empty);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
static_assert(sizeof(HardwareRay) == 32 && sizeof(HardwareHit) == 16, "Ray shader ABI");
} // namespace Filmmaker::Environment
