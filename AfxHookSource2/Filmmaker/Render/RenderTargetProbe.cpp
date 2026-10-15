#include "RenderTargetProbe.h"
#include "RenderEngineSize.h" // install the size source before startup scene-pool creation
#include "RenderSupersample.h" // scene-target enlargement policy (rides this same detour)
#include "RenderHdrCapture.h"  // HDR scene-target tracking (rides this same detour)
#include "RenderPrivateTextureScope.h"

#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/AfxDetours.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <d3d11.h>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include <psapi.h>

namespace Filmmaker {

namespace {

typedef HRESULT(STDMETHODCALLTYPE* CreateTexture2D_t)(
	ID3D11Device* This,
	const D3D11_TEXTURE2D_DESC* pDesc,
	const D3D11_SUBRESOURCE_DATA* pInitialData,
	ID3D11Texture2D** ppTexture2D);

// ReShade proxies and native devices can coexist. A vtable replacement stays installed
// on both, so dispatch must use the original belonging to THIS object's vtable.
std::mutex s_textureHooksMutex;
std::unordered_map<void**, CreateTexture2D_t> s_textureHooks;
CreateTexture2D_t OriginalCreateTexture2D(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(s_textureHooksMutex);
    auto found = s_textureHooks.find(*(void***)device);
    return found == s_textureHooks.end() ? nullptr : found->second;
}
std::atomic<bool> s_enabled{ false };

// ---- Owned GPU memory: every buffer/texture this DLL creates, by creating call site ----------
// A sentinel interface rides each owned resource as private data; D3D releases it when the
// resource is destroyed, which is what takes the bytes back off its site. Engine creations are
// recognised by their return address and cost one compare.
typedef HRESULT(STDMETHODCALLTYPE* CreateBuffer_t)(ID3D11Device*,const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Buffer**);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture3D_t)(ID3D11Device*,const D3D11_TEXTURE3D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture3D**);
std::unordered_map<void**, CreateBuffer_t> s_bufferHooks;
std::unordered_map<void**, CreateTexture3D_t> s_texture3DHooks;
// {b3c1d27e-5f4a-4e0b-9d61-2a7c0e8f4d15}
const GUID OwnedMemoryTag={0xb3c1d27e,0x5f4a,0x4e0b,{0x9d,0x61,0x2a,0x7c,0x0e,0x8f,0x4d,0x15}};
uintptr_t s_selfBase=0,s_selfEnd=0;
struct OwnedSite{unsigned long long bytes=0;unsigned live=0;};
// Immortal on purpose: D3D11 objects released during static destruction (other TUs, process exit) still
// run OwnedSentinel::Release, which must find a live map and mutex. Ordinary statics crashed there.
std::mutex& s_ownedMutex=*new std::mutex;
std::unordered_map<uintptr_t,OwnedSite>& s_ownedSites=*new std::unordered_map<uintptr_t,OwnedSite>; // keyed by caller RVA in this DLL
std::atomic<unsigned long long> s_ownedBytes{0};
std::atomic<unsigned> s_ownedResources{0};
bool OwnedCaller(void* address){
    if(!s_selfBase){
        HMODULE self=nullptr;
        if(::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)&OwnedCaller,&self)&&self){
            MODULEINFO info{};if(::GetModuleInformation(::GetCurrentProcess(),self,&info,sizeof(info))){
                s_selfEnd=uintptr_t(self)+info.SizeOfImage;s_selfBase=uintptr_t(self);}}
    }
    return uintptr_t(address)>=s_selfBase&&uintptr_t(address)<s_selfEnd;
}
struct OwnedSentinel final:IUnknown{
    std::atomic<ULONG> references{1};uintptr_t site;unsigned long long bytes;
    OwnedSentinel(uintptr_t s,unsigned long long b):site(s),bytes(b){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override{
        if(!out)return E_POINTER;
        if(id==__uuidof(IUnknown)){*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
        *out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{
        const ULONG left=--references;
        if(!left){
            {std::lock_guard<std::mutex> lock(s_ownedMutex);auto& entry=s_ownedSites[site];
                entry.bytes-=(std::min)(entry.bytes,bytes);if(entry.live)--entry.live;}
            s_ownedBytes.fetch_sub(bytes,std::memory_order_relaxed);s_ownedResources.fetch_sub(1,std::memory_order_relaxed);
            delete this;
        }
        return left;
    }
};
void TrackOwned(ID3D11DeviceChild* resource,void* caller,unsigned long long bytes){
    if(!resource||!OwnedCaller(caller))return;
    const uintptr_t site=uintptr_t(caller)-s_selfBase;
    auto* sentinel=new OwnedSentinel(site,bytes);
    {std::lock_guard<std::mutex> lock(s_ownedMutex);auto& entry=s_ownedSites[site];entry.bytes+=bytes;++entry.live;}
    s_ownedBytes.fetch_add(bytes,std::memory_order_relaxed);s_ownedResources.fetch_add(1,std::memory_order_relaxed);
    if(FAILED(resource->SetPrivateDataInterface(OwnedMemoryTag,sentinel))){sentinel->references=1;}
    sentinel->Release(); // the resource holds the only reference now (or this undoes the count)
}
double FormatBytes(DXGI_FORMAT f){
    switch(f){
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R32G32B32A32_UINT:case DXGI_FORMAT_R32G32B32A32_SINT:return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS:case DXGI_FORMAT_R32G32B32_FLOAT:case DXGI_FORMAT_R32G32B32_UINT:case DXGI_FORMAT_R32G32B32_SINT:return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:case DXGI_FORMAT_R32G32_TYPELESS:case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:case DXGI_FORMAT_R32G32_SINT:case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return 8;
    case DXGI_FORMAT_R16G16_TYPELESS:case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_TYPELESS:return f==DXGI_FORMAT_R16G16_TYPELESS||f==DXGI_FORMAT_R16G16_FLOAT?4:2;
    case DXGI_FORMAT_R8_TYPELESS:case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_A8_UNORM:return 1;
    case DXGI_FORMAT_BC1_TYPELESS:case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:return .5;
    case DXGI_FORMAT_BC3_TYPELESS:case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_TYPELESS:case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:return 1;
    default:return 4;
    }
}
unsigned long long MipFactorBytes(double level,UINT mips){return (unsigned long long)(mips==1?level:level*4/3);}
HRESULT STDMETHODCALLTYPE New_CreateBuffer(ID3D11Device* This,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Buffer** out){
    CreateBuffer_t original=nullptr;
    {std::lock_guard<std::mutex> lock(s_textureHooksMutex);auto found=s_bufferHooks.find(*(void***)This);if(found!=s_bufferHooks.end())original=found->second;}
    if(!original){if(out)*out=nullptr;return E_UNEXPECTED;}
    const HRESULT hr=original(This,desc,data,out);
    if(SUCCEEDED(hr)&&desc&&out&&*out&&desc->Usage!=D3D11_USAGE_STAGING)TrackOwned(*out,_ReturnAddress(),desc->ByteWidth);
    return hr;
}
HRESULT STDMETHODCALLTYPE New_CreateTexture3D(ID3D11Device* This,const D3D11_TEXTURE3D_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture3D** out){
    CreateTexture3D_t original=nullptr;
    {std::lock_guard<std::mutex> lock(s_textureHooksMutex);auto found=s_texture3DHooks.find(*(void***)This);if(found!=s_texture3DHooks.end())original=found->second;}
    if(!original){if(out)*out=nullptr;return E_UNEXPECTED;}
    const HRESULT hr=original(This,desc,data,out);
    if(SUCCEEDED(hr)&&desc&&out&&*out&&desc->Usage!=D3D11_USAGE_STAGING)
        TrackOwned(*out,_ReturnAddress(),MipFactorBytes(double(desc->Width)*desc->Height*desc->Depth*FormatBytes(desc->Format),desc->MipLevels));
    return hr;
}
void TrackOwnedTexture2D(HRESULT hr,const D3D11_TEXTURE2D_DESC* desc,ID3D11Texture2D** out,void* caller){
    if(SUCCEEDED(hr)&&desc&&out&&*out&&desc->Usage!=D3D11_USAGE_STAGING)
        TrackOwned(*out,caller,MipFactorBytes(double(desc->Width)*desc->Height*(std::max)(1u,desc->ArraySize)*
            (std::max)(1u,desc->SampleDesc.Count)*FormatBytes(desc->Format),desc->MipLevels));
}


// The interesting creations happen during STARTUP, long before "mvm_debug start" can run, so the
// probe cannot rely on the debug log being open. It records into memory and `rtprobe dump`
// prints the findings afterwards. Bounded so a pathological session cannot grow without limit.
std::mutex s_recordMutex;
std::vector<std::string> s_records;
constexpr size_t kMaxRecords = 400;

void Record(const std::string& line) {
	{
		std::lock_guard<std::mutex> lock(s_recordMutex);
		if (s_records.size() < kMaxRecords)
			s_records.push_back(line);
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.rtcreate", "%s", line.c_str());
}

// Resolve an absolute address to "module+0xoffset", which is the form a disassembler wants.
// GetModuleHandleExA with UNCHANGED_REFCOUNT does not take a reference, so this is safe to call
// from an arbitrary engine thread.
std::string DescribeAddress(void* addr) {
	HMODULE mod = nullptr;
	if (!::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod) || !mod) {
		char buf[32];
		snprintf(buf, sizeof(buf), "?+%p", addr);
		return buf;
	}
	char path[MAX_PATH] = {0};
	::GetModuleFileNameA(mod, path, MAX_PATH);
	const char* name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	char buf[MAX_PATH + 48];
	snprintf(buf, sizeof(buf), "%s+0x%llx", name,
		(unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
	return buf;
}

// The signature of a SCENE colour target: multisampled, bound as both a render target and a
// shader resource. That is what render.capsize showed the recorder captures
// (samples=8, bind=0x28). Shadow maps and the many single-sampled utility targets are excluded
// so the log stays readable.
bool LooksLikeSceneTarget(const D3D11_TEXTURE2D_DESC& d) {
	if (d.SampleDesc.Count < 2) return false;
	if (0 == (d.BindFlags & D3D11_BIND_RENDER_TARGET)) return false;
	if (d.Width < 256 || d.Height < 256) return false;
	return true;
}

HRESULT STDMETHODCALLTYPE New_CreateTexture2D(
	ID3D11Device* This,
	const D3D11_TEXTURE2D_DESC* pDesc,
	const D3D11_SUBRESOURCE_DATA* pInitialData,
	ID3D11Texture2D** ppTexture2D) {
    const auto createTexture = OriginalCreateTexture2D(This);
    if (!createTexture) {
        if (ppTexture2D) *ppTexture2D = nullptr;
        return E_UNEXPECTED;
    }
    if (CreatingPrivateRenderTexture()) {
        const HRESULT privateResult = createTexture(This, pDesc, pInitialData, ppTexture2D);
        TrackOwnedTexture2D(privateResult, pDesc, ppTexture2D, _ReturnAddress());
        return privateResult;
    }

	if (s_enabled.load(std::memory_order_relaxed) && pDesc && LooksLikeSceneTarget(*pDesc)) {
		// De-duplicate on the description: the engine creates a family of same-sized targets and
		// only the distinct shapes are interesting.
		static UINT s_lastSig = 0xFFFFFFFFu;
		const UINT sig = pDesc->Width * 31u + pDesc->Height * 17u
			+ pDesc->SampleDesc.Count * 7u + pDesc->BindFlags + (UINT)pDesc->Format * 3u;
		if (sig != s_lastSig) {
			s_lastSig = sig;

			char head[256];
			snprintf(head, sizeof(head),
				"scene target %ux%u fmt=%d samples=%u bind=0x%x usage=%d mips=%u array=%u",
				pDesc->Width, pDesc->Height, (int)pDesc->Format, pDesc->SampleDesc.Count,
				pDesc->BindFlags, (int)pDesc->Usage, pDesc->MipLevels, pDesc->ArraySize);
			Record(head);

			// The call stack is the actual deliverable: whichever engine function appears here is
			// the one that decided this size, and its module+offset is the disassembly entry point.
			void* frames[24] = {0};
			const USHORT n = ::RtlCaptureStackBackTrace(1, 24, frames, nullptr);
			for (USHORT i = 0; i < n; ++i) {
				char line[MAX_PATH + 64];
				snprintf(line, sizeof(line), "  frame[%02u] %s",
					(unsigned)i, DescribeAddress(frames[i]).c_str());
				Record(line);
			}
		}
	}

	// Scene supersampling rides this same detour: it is the one place where a render target's
	// size can still be changed. The POLICY lives in RenderSupersample; this is only the site.
	// Copy-on-modify so the engine's own descriptor is never mutated behind its back.
	HRESULT hr;
	if (pDesc && RenderSupersample_TextureAdjustmentActive()) {
		D3D11_TEXTURE2D_DESC scaled = *pDesc;
		if (RenderSupersample_AdjustTextureDesc(scaled)) {
			hr = createTexture(This, &scaled, pInitialData, ppTexture2D);
			if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D)
				RenderHdrCapture_OnCreateTexture2D(scaled, *ppTexture2D);
			return hr;
		}
	}

	hr = createTexture(This, pDesc, pInitialData, ppTexture2D);
    TrackOwnedTexture2D(hr, pDesc, ppTexture2D, _ReturnAddress());
    if (FAILED(hr) && MvmDebugLog_Active())
        MvmDebugLog_Linef("render.rtcreate", "failed hr=%08x device=%p original=%p size=%ux%u format=%u bind=%u",
            unsigned(hr), This, createTexture, pDesc ? pDesc->Width : 0, pDesc ? pDesc->Height : 0,
            pDesc ? unsigned(pDesc->Format) : 0, pDesc ? pDesc->BindFlags : 0);

	// HDR capture tracks the pre-tonemap scene colour target from this same sanctioned
	// interception site (policy lives in RenderHdrCapture, mirroring RenderSupersample).
	if (SUCCEEDED(hr) && pDesc && ppTexture2D && *ppTexture2D)
		RenderHdrCapture_OnCreateTexture2D(*pDesc, *ppTexture2D);
	return hr;
}

} // namespace

void RenderTargetProbe_AttachDevice(ID3D11Device* pDevice) {
	if (!pDevice)
		return;
	// The first CreateSwapChain callback precedes allocation of the engine2-owned scene colour/depth
	// targets. The client render-device interface is not ready yet, but this call reads and arms the
	// startup factor so the CreateTexture2D detour enlarges those first physical resources. A client
	// target-builder gateway installs the scoped size query later, before its pool construction.
	// This remains an exact no-op unless MVM_RENDER_ENGINESIZE is explicitly enabled.
	RenderEngineSize_EnsureInstalled();
	// The scene targets are created during STARTUP, before any console command can arm this --
	// so the probe can also be switched on before the process gets that far, via the
	// environment. MVM_RTPROBE=1 in the launcher is what makes the creation stacks catchable.
	{
		static bool s_envChecked = false;
		if (!s_envChecked) {
			s_envChecked = true;
			char buf[8] = {0};
			DWORD n = ::GetEnvironmentVariableA("MVM_RTPROBE", buf, (DWORD)sizeof(buf));
			if (n > 0 && n < sizeof(buf) && buf[0] != '0')
				s_enabled.store(true);
		}
	}
	// ID3D11Device vtable: 0 QueryInterface, 1 AddRef, 2 Release, 3 CreateBuffer,
	// 4 CreateTexture1D, 5 CreateTexture2D.
	void** vtable = *(void***)pDevice;
    std::lock_guard<std::mutex> lock(s_textureHooksMutex);
    // Owned-memory accounting: CreateBuffer (3) and CreateTexture3D (6), same per-vtable dispatch.
    if ((void*)vtable[3] != (void*)New_CreateBuffer) {
        s_bufferHooks[vtable] = (CreateBuffer_t)vtable[3];
        if (!AfxDetourPtr(&(vtable[3]), New_CreateBuffer, nullptr) && vtable[3] != (void*)New_CreateBuffer) s_bufferHooks.erase(vtable);
    }
    if ((void*)vtable[6] != (void*)New_CreateTexture3D) {
        s_texture3DHooks[vtable] = (CreateTexture3D_t)vtable[6];
        if (!AfxDetourPtr(&(vtable[6]), New_CreateTexture3D, nullptr) && vtable[6] != (void*)New_CreateTexture3D) s_texture3DHooks.erase(vtable);
    }
    const auto target = (CreateTexture2D_t)vtable[5];
    if (target == New_CreateTexture2D)
        return; // already patched: do not replace its original with our own callback
    s_textureHooks[vtable] = target; // publish before any thread can enter the replacement
    if (!AfxDetourPtr(&(vtable[5]), New_CreateTexture2D, nullptr)) {
        // VirtualProtect may fail after the pointer was written; preserve dispatch in that case.
        if (vtable[5] != (void*)New_CreateTexture2D) s_textureHooks.erase(vtable);
        return;
    }

	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.rtcreate", "CreateTexture2D detour installed on device %p", (void*)pDevice);
}

bool RenderTargetProbe_Enabled() { return s_enabled.load(); }

void RenderOwnedMemory_Totals(unsigned long long& bytes, unsigned& resources) {
    bytes = s_ownedBytes.load(std::memory_order_relaxed);
    resources = s_ownedResources.load(std::memory_order_relaxed);
}

void RenderOwnedMemory_Print(unsigned top) {
    std::vector<std::pair<uintptr_t, OwnedSite>> sites;
    { std::lock_guard<std::mutex> lock(s_ownedMutex); sites.assign(s_ownedSites.begin(), s_ownedSites.end()); }
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    advancedfx::Message("  hook gpu memory              : %.1f MB in %u resources (estimate; by creating call site)\n",
        s_ownedBytes.load() / 1048576.0, s_ownedResources.load());
    for (unsigned i = 0; i < sites.size() && i < top && sites[i].second.bytes; ++i)
        advancedfx::Message("    AfxHookSource2.dll+0x%llx  %8.1f MB  %u live\n",
            (unsigned long long)sites[i].first, sites[i].second.bytes / 1048576.0, sites[i].second.live);
}

void RenderTargetProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "status";
	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "1")) {
		s_enabled.store(true);
		advancedfx::Message("%s: render-target creation probe ON. Now force the engine to recreate "
			"its targets (change the resolution in CS2 video settings, or load a map/demo) and read "
			"the `render.rtcreate` lines in the mvm_debug log.\n", cmd);
	} else if (0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "0")) {
		s_enabled.store(false);
		advancedfx::Message("%s: render-target creation probe off.\n", cmd);
	} else if (0 == _stricmp(sub, "dump")) {
		std::vector<std::string> copy;
		{
			std::lock_guard<std::mutex> lock(s_recordMutex);
			copy = s_records;
		}
		advancedfx::Message("%s: %zu recorded line(s):\n", cmd, copy.size());
		for (const std::string& s : copy)
			advancedfx::Message("%s\n", s.c_str());
		if (copy.empty())
			advancedfx::Message("  (nothing captured -- the targets are made during startup, so set "
				"MVM_RTPROBE=1 in the environment BEFORE launching)\n");
	} else if (0 == _stricmp(sub, "clear")) {
		std::lock_guard<std::mutex> lock(s_recordMutex);
		s_records.clear();
		advancedfx::Message("%s: cleared.\n", cmd);
	} else {
		size_t n = 0;
        bool installed;
        { std::lock_guard<std::mutex> lock(s_textureHooksMutex); installed = !s_textureHooks.empty(); }
		{ std::lock_guard<std::mutex> lock(s_recordMutex); n = s_records.size(); }
		advancedfx::Message("%s: render-target creation probe is %s (detour %s), %zu line(s) recorded.\n"
			"  %s render rtprobe on|off|dump|clear\n", cmd,
			s_enabled.load() ? "ON" : "off",
			installed ? "installed" : "NOT installed - no device seen yet", n, cmd);
	}
}

} // namespace Filmmaker
