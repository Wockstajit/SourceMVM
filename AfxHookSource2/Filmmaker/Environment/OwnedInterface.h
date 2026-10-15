#pragma once
// Private-data interfaces that survive RenderDoc.
//
// The lighting and shadow features attach replacement shaders to the engine's shaders with
// SetPrivateDataInterface and read them back with GetPrivateData. D3D11 releases such an interface
// while it destroys the engine shader, under its own lock. When the replacement was created through
// RenderDoc's wrapped device, that Release takes RenderDoc's lock, while another thread inside a
// wrapped Create* call holds RenderDoc's lock and waits for D3D11's: demo load deadlocks.
//
// Without RenderDoc these helpers are the plain D3D11 calls. With RenderDoc resident, D3D11 holds a
// small mod-owned holder instead; when it is destroyed the wrapped object goes on a queue that the
// render thread drains (FlushDeferredOwnedInterfaces), so no RenderDoc lock is taken inside D3D11's
// destruction path. Callers never see the holder: Get returns the stored interface itself.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace Filmmaker {
namespace OwnedInterfaceDetail {

inline bool UnderRenderDoc() {
    static const bool underRenderDoc = nullptr != ::GetModuleHandleW(L"renderdoc.dll");
    return underRenderDoc;
}

struct DeferredQueue {
    std::mutex lock;
    std::vector<IUnknown*> items;
};
// Immortal: holders can be destroyed during process exit, after ordinary statics are gone.
inline DeferredQueue& Queue() {
    static DeferredQueue* queue = new DeferredQueue;
    return *queue;
}

struct Holder final : IUnknown {
    std::atomic<ULONG> references{1};
    IUnknown* held;
    explicit Holder(IUnknown* object) : held(object) { held->AddRef(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id == __uuidof(IUnknown)) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references;
        if (!left) {
            {
                DeferredQueue& queue = Queue();
                std::lock_guard<std::mutex> guard(queue.lock);
                queue.items.push_back(held);
            }
            delete this;
        }
        return left;
    }
};

} // namespace OwnedInterfaceDetail

// Accepts a raw pointer or a ComPtr at the call sites.
inline ID3D11DeviceChild* RawOwner(ID3D11DeviceChild* owner) { return owner; }
template <class T> ID3D11DeviceChild* RawOwner(const Microsoft::WRL::ComPtr<T>& owner) { return owner.Get(); }

inline HRESULT SetOwnedInterface(ID3D11DeviceChild* owner, REFGUID tag, IUnknown* object) {
    if (!OwnedInterfaceDetail::UnderRenderDoc()) return owner->SetPrivateDataInterface(tag, object);
    auto* holder = new OwnedInterfaceDetail::Holder(object);
    const HRESULT result = owner->SetPrivateDataInterface(tag, holder);
    holder->Release(); // D3D11 holds its own reference on success
    return result;
}

// Same contract as owner->GetPrivateData(tag, &size, &pointer) for an interface stored by SetOwnedInterface:
// *out receives the stored interface with one added reference.
inline HRESULT GetOwnedInterface(ID3D11DeviceChild* owner, REFGUID tag, UINT* size, void** out) {
    if (!OwnedInterfaceDetail::UnderRenderDoc()) return owner->GetPrivateData(tag, size, out);
    IUnknown* stored = nullptr;
    UINT bytes = sizeof(stored);
    const HRESULT result = owner->GetPrivateData(tag, &bytes, &stored);
    if (FAILED(result) || !stored) { if (out) *out = nullptr; return FAILED(result) ? result : E_FAIL; }
    auto* holder = static_cast<OwnedInterfaceDetail::Holder*>(stored);
    IUnknown* object = holder->held;
    object->AddRef();
    if (out) *out = object;
    if (size) *size = sizeof(void*);
    stored->Release();
    return S_OK;
}

// Call once per frame from the render thread.
inline void FlushDeferredOwnedInterfaces() {
    std::vector<IUnknown*> pending;
    {
        OwnedInterfaceDetail::DeferredQueue& queue = OwnedInterfaceDetail::Queue();
        std::lock_guard<std::mutex> guard(queue.lock);
        pending.swap(queue.items);
    }
    for (IUnknown* object : pending) object->Release();
}

} // namespace Filmmaker
