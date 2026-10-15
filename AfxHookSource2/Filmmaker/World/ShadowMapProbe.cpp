#include "ShadowMapProbe.h"

#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Filmmaker {
namespace {

std::atomic<bool> g_armed{ false };
std::atomic<unsigned long long> g_events{ 0 };
std::mutex g_seenMutex;
std::unordered_set<std::string> g_seenTargets;
std::unordered_set<std::string> g_seenShaders;
std::unordered_set<std::string> g_seenResources;
std::unordered_set<std::string> g_seenDraws;
std::unordered_set<void*> g_seenUpdates;
thread_local ID3D11Resource* g_mappedResource = nullptr;
thread_local const unsigned char* g_mappedData = nullptr;

bool Remember(std::unordered_set<std::string>& seen, const std::string& key, size_t limit = 2048) {
	std::lock_guard<std::mutex> lock(g_seenMutex);
	if (seen.size() >= limit) return false;
	return seen.insert(key).second;
}

void ClearSeen() {
	std::lock_guard<std::mutex> lock(g_seenMutex);
	g_seenTargets.clear();
	g_seenShaders.clear();
	g_seenResources.clear();
	g_seenDraws.clear();
	g_seenUpdates.clear();
	g_events.store(0, std::memory_order_relaxed);
}

bool ReadTexture2D(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC* out) {
	if (!resource || !out) return false;
	ID3D11Texture2D* texture = nullptr;
	const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
	if (FAILED(hr) || !texture) return false;
	texture->GetDesc(out);
	texture->Release();
	return true;
}

const char* ViewDimensionName(D3D11_SRV_DIMENSION dimension) {
	switch (dimension) {
	case D3D11_SRV_DIMENSION_TEXTURE2D: return "2d";
	case D3D11_SRV_DIMENSION_TEXTURE2DMS: return "2dms";
	case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: return "2d-array";
	case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY: return "2dms-array";
	default: return "other";
	}
}

std::string DebugObjectName(ID3D11DeviceChild* object) {
	char name[256] = {};
	UINT size = sizeof(name) - 1;
	if (!object || FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
		return {};
	name[size < sizeof(name) ? size : sizeof(name) - 1] = '\0';
	return name;
}

void FormatFirstFloats(const void* sourceData, size_t byteCount, char* out, size_t outSize) {
	if (!out || outSize == 0) return;
	out[0] = '\0';
	if (!sourceData) return;
	const size_t count = std::min<size_t>(byteCount / sizeof(float), 16);
	const float* values = static_cast<const float*>(sourceData);
	size_t used = 0;
	for (size_t i = 0; i < count && used < outSize; ++i) {
		const int written = sprintf_s(
			out + used, outSize - used, i == 0 ? "%.6g" : " %.6g", values[i]);
		if (written <= 0) break;
		used += (size_t)written;
	}
}

} // namespace

void ShadowMapProbe_NoteRenderTargets(
	ID3D11DeviceContext* context,
	UINT numViews,
	ID3D11RenderTargetView* const* renderTargetViews,
	ID3D11DepthStencilView* depthStencilView) {
	(void)context;
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active())
		return;

	ID3D11Resource* rtvResource = nullptr;
	ID3D11Resource* dsvResource = nullptr;
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	D3D11_TEXTURE2D_DESC rtvTexture = {};
	D3D11_TEXTURE2D_DESC dsvTexture = {};

	if (numViews > 0 && renderTargetViews && renderTargetViews[0]) {
		renderTargetViews[0]->GetDesc(&rtvDesc);
		renderTargetViews[0]->GetResource(&rtvResource);
		if (rtvResource) ReadTexture2D(rtvResource, &rtvTexture);
	}
	if (depthStencilView) {
		depthStencilView->GetDesc(&dsvDesc);
		depthStencilView->GetResource(&dsvResource);
		if (dsvResource) ReadTexture2D(dsvResource, &dsvTexture);
	}

	char key[256] = {};
	sprintf_s(key, "%p/%p/%u/%u/%u/%u/%u/%u",
		(void*)rtvResource, (void*)dsvResource,
		rtvTexture.Width, rtvTexture.Height, (unsigned)rtvTexture.Format,
		dsvTexture.Width, dsvTexture.Height, (unsigned)dsvTexture.Format);
	if (Remember(g_seenTargets, key)) {
		const bool depthOnly = numViews == 0 && dsvResource != nullptr;
		MvmDebugLog_LinefAlways("world.shadowmap.probe",
			"target event=%llu kind=%s rtvCount=%u rtv=%p rtvViewFmt=%u "
			"rtvTex=%ux%u fmt=%u samples=%u dsv=%p dsvViewFmt=%u "
			"dsvTex=%ux%u fmt=%u samples=%u",
			g_events.fetch_add(1, std::memory_order_relaxed) + 1,
			depthOnly ? "depth-only" : (dsvResource ? "color-depth" : "color"),
			numViews, (void*)rtvResource, (unsigned)rtvDesc.Format,
			rtvTexture.Width, rtvTexture.Height, (unsigned)rtvTexture.Format,
			rtvTexture.SampleDesc.Count, (void*)dsvResource, (unsigned)dsvDesc.Format,
			dsvTexture.Width, dsvTexture.Height, (unsigned)dsvTexture.Format,
			dsvTexture.SampleDesc.Count);
	}

	if (rtvResource) rtvResource->Release();
	if (dsvResource) dsvResource->Release();
}

void ShadowMapProbe_NotePixelShader(ID3D11PixelShader* shader, const char* name) {
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active())
		return;

	char key[512] = {};
	sprintf_s(key, "%p/%s", (void*)shader, name ? name : "");
	if (Remember(g_seenShaders, key)) {
		MvmDebugLog_LinefAlways("world.shadowmap.probe",
			"pixel-shader shader=%p name='%s'",
			(void*)shader, name ? name : "");
	}
}

void ShadowMapProbe_NoteShaderResources(
	ID3D11DeviceContext* context,
	UINT startSlot,
	UINT numViews,
	ID3D11ShaderResourceView* const* shaderResourceViews) {
	(void)context;
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active()
		|| !shaderResourceViews)
		return;

	for (UINT i = 0; i < numViews; ++i) {
		ID3D11ShaderResourceView* view = shaderResourceViews[i];
		if (!view) continue;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		view->GetDesc(&srvDesc);
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		D3D11_TEXTURE2D_DESC textureDesc = {};
		if (resource) ReadTexture2D(resource, &textureDesc);

		char key[256] = {};
		sprintf_s(key, "%u/%p/%u/%u/%u/%u",
			startSlot + i, (void*)resource, (unsigned)srvDesc.ViewDimension,
			textureDesc.Width, textureDesc.Height, (unsigned)textureDesc.Format);
		if (Remember(g_seenResources, key)) {
			MvmDebugLog_LinefAlways("world.shadowmap.probe",
				"resource slot=%u view=%p resource=%p dim=%s viewFmt=%u "
				"tex=%ux%u fmt=%u samples=%u array=%u",
				startSlot + i, (void*)view, (void*)resource,
				ViewDimensionName(srvDesc.ViewDimension), (unsigned)srvDesc.Format,
				textureDesc.Width, textureDesc.Height, (unsigned)textureDesc.Format,
				textureDesc.SampleDesc.Count, textureDesc.ArraySize);
		}
		if (resource) resource->Release();
	}
}

void ShadowMapProbe_NoteUpdateSubresource(
	ID3D11Resource* resource,
	const void* sourceData,
	UINT sourceRowPitch,
	UINT sourceDepthPitch) {
	(void)sourceData;
	(void)sourceRowPitch;
	(void)sourceDepthPitch;
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active()
		|| !resource)
		return;

	D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
	resource->GetType(&dimension);
	if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

	D3D11_BUFFER_DESC desc = {};
	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer)
		return;
	buffer->GetDesc(&desc);
	buffer->Release();

	bool first = false;
	{
		std::lock_guard<std::mutex> lock(g_seenMutex);
		first = g_seenUpdates.insert((void*)resource).second;
	}
	if (first) {
		char values[768] = {};
		size_t valueCount = 0;
		if (sourceData) {
			const size_t maxValues = std::min<size_t>(desc.ByteWidth / sizeof(float), 16);
			float floats[16] = {};
			std::memcpy(floats, sourceData, maxValues * sizeof(float));
			for (size_t i = 0; i < maxValues && valueCount < sizeof(values); ++i) {
				const int written = sprintf_s(
					values + valueCount, sizeof(values) - valueCount,
					i == 0 ? "%.6g" : " %.6g", floats[i]);
				if (written <= 0) break;
				valueCount += (size_t)written;
			}
		}
		MvmDebugLog_LinefAlways("world.shadowmap.probe",
			"constant-update resource=%p source=%p bytes=%u rowPitch=%u depthPitch=%u "
			"bind=0x%x usage=%u cpu=0x%x misc=0x%x firstFloats='%s'",
			(void*)resource, sourceData, desc.ByteWidth, sourceRowPitch, sourceDepthPitch,
			desc.BindFlags, (unsigned)desc.Usage, desc.CPUAccessFlags, desc.MiscFlags,
			values);
	}
}

bool ShadowMapProbe_Armed() {
	return g_armed.load(std::memory_order_acquire);
}

void ShadowMapProbe_NoteMap(ID3D11Resource* resource, void* mappedData) {
	g_mappedResource = nullptr;
	g_mappedData = nullptr;
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active() || !resource || !mappedData)
		return;

	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer)
		return;
	D3D11_BUFFER_DESC desc = {};
	buffer->GetDesc(&desc);
	buffer->Release();
	if (desc.ByteWidth == 0 || desc.ByteWidth > 64u * 1024u
		|| desc.Usage != D3D11_USAGE_DYNAMIC
		|| !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE))
		return;

	g_mappedResource = resource;
	g_mappedData = static_cast<const unsigned char*>(mappedData);
}

void ShadowMapProbe_NoteUnmap(ID3D11Resource* resource) {
	if (g_mappedResource != resource || !g_mappedData) return;
	const unsigned char* data = g_mappedData;
	g_mappedResource = nullptr;
	g_mappedData = nullptr;
	if (!g_armed.load(std::memory_order_relaxed) || !MvmDebugLog_Active() || !resource)
		return;

	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer)
		return;
	D3D11_BUFFER_DESC desc = {};
	buffer->GetDesc(&desc);
	buffer->Release();

	bool first = false;
	{
		std::lock_guard<std::mutex> lock(g_seenMutex);
		first = g_seenUpdates.insert((void*)resource).second;
	}
	if (!first) return;

	char values[768] = {};
	FormatFirstFloats(data, desc.ByteWidth, values, sizeof(values));
	MvmDebugLog_LinefAlways("world.shadowmap.probe",
		"mapped-update resource=%p bytes=%u usage=%u cpu=0x%x firstFloats='%s'",
		(void*)resource, desc.ByteWidth, (unsigned)desc.Usage,
		desc.CPUAccessFlags, values);
}

void ShadowMapProbe_NoteDraw(
	ID3D11DeviceContext* context,
	const char* operation,
	UINT arg0,
	UINT arg1,
	UINT arg2,
	UINT arg3,
	void* caller) {
	if (!g_armed.load(std::memory_order_relaxed)
		|| !MvmDebugLog_Active() || !context
		|| context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		return;

	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11DepthStencilView* dsv = nullptr;
	context->OMGetRenderTargets(1, &rtv, &dsv);
	if (!dsv) {
		if (rtv) rtv->Release();
		return;
	}

	ID3D11Resource* dsvResource = nullptr;
	dsv->GetResource(&dsvResource);
	D3D11_TEXTURE2D_DESC dsvTexture = {};
	if (dsvResource) ReadTexture2D(dsvResource, &dsvTexture);
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsv->GetDesc(&dsvDesc);

	ID3D11Resource* rtvResource = nullptr;
	D3D11_TEXTURE2D_DESC rtvTexture = {};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	if (rtv) {
		rtv->GetResource(&rtvResource);
		rtv->GetDesc(&rtvDesc);
		if (rtvResource) ReadTexture2D(rtvResource, &rtvTexture);
	}

	D3D11_VIEWPORT viewport = {};
	UINT viewportCount = 1;
	context->RSGetViewports(&viewportCount, &viewport);

	ID3D11VertexShader* vertexShader = nullptr;
	ID3D11PixelShader* pixelShader = nullptr;
	context->VSGetShader(&vertexShader, nullptr, nullptr);
	context->PSGetShader(&pixelShader, nullptr, nullptr);
	const std::string vsName = DebugObjectName(vertexShader);
	const std::string psName = DebugObjectName(pixelShader);

	char key[768] = {};
	sprintf_s(key,
		"%p/%p/%u/%u/%u/%u/%u/%u/%.2f/%.2f/%.2f/%.2f/%s/%s",
		(void*)dsvResource, (void*)rtvResource,
		dsvTexture.Width, dsvTexture.Height, (unsigned)dsvTexture.Format,
		rtvTexture.Width, rtvTexture.Height, (unsigned)rtvTexture.Format,
		viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
		vsName.c_str(), psName.c_str());
	if (Remember(g_seenDraws, key)) {
		MvmDebugLog_LinefAlways("world.shadowmap.probe",
			"draw event=%llu op=%s args=%u,%u,%u,%u caller=%p "
			"rtv=%p rtvTex=%ux%u fmt=%u samples=%u dsv=%p dsvTex=%ux%u fmt=%u samples=%u "
			"vp=%.1f,%.1f %.1fx%.1f vs='%s' ps='%s'",
			g_events.fetch_add(1, std::memory_order_relaxed) + 1,
			operation ? operation : "?", arg0, arg1, arg2, arg3, caller,
			(void*)rtvResource, rtvTexture.Width, rtvTexture.Height,
			(unsigned)rtvTexture.Format, rtvTexture.SampleDesc.Count,
			(void*)dsvResource, dsvTexture.Width, dsvTexture.Height,
			(unsigned)dsvTexture.Format, dsvTexture.SampleDesc.Count,
			viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
			vsName.empty() ? "?" : vsName.c_str(),
			psName.empty() ? "?" : psName.c_str());

		ID3D11Buffer* constantBuffers[8] = {};
		context->VSGetConstantBuffers(0, 8, constantBuffers);
		for (UINT slot = 0; slot < 8; ++slot) {
			if (!constantBuffers[slot]) continue;
			D3D11_BUFFER_DESC desc = {};
			constantBuffers[slot]->GetDesc(&desc);
			MvmDebugLog_LinefAlways("world.shadowmap.probe",
				"draw-cbuffer slot=%u buffer=%p bytes=%u usage=%u cpu=0x%x",
				slot, (void*)constantBuffers[slot], desc.ByteWidth,
				(unsigned)desc.Usage, desc.CPUAccessFlags);
			constantBuffers[slot]->Release();
		}

		ID3D11ShaderResourceView* resources[16] = {};
		context->PSGetShaderResources(0, 16, resources);
		for (UINT slot = 0; slot < 16; ++slot) {
			if (!resources[slot]) continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
			resources[slot]->GetDesc(&viewDesc);
			ID3D11Resource* resource = nullptr;
			resources[slot]->GetResource(&resource);
			D3D11_TEXTURE2D_DESC texture = {};
			if (resource) ReadTexture2D(resource, &texture);
			MvmDebugLog_LinefAlways("world.shadowmap.probe",
				"draw-ps-resource slot=%u view=%p resource=%p dim=%s viewFmt=%u "
				"tex=%ux%u fmt=%u samples=%u array=%u",
				slot, (void*)resources[slot], (void*)resource,
				ViewDimensionName(viewDesc.ViewDimension), (unsigned)viewDesc.Format,
				texture.Width, texture.Height, (unsigned)texture.Format,
				texture.SampleDesc.Count, texture.ArraySize);
			if (resource) resource->Release();
			resources[slot]->Release();
		}
	}

	if (vertexShader) vertexShader->Release();
	if (pixelShader) pixelShader->Release();
	if (rtvResource) rtvResource->Release();
	if (dsvResource) dsvResource->Release();
	if (rtv) rtv->Release();
	if (dsv) dsv->Release();
	(void)dsvDesc;
	(void)rtvDesc;
}

void ShadowMapProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "probe";
	const char* mode = argc >= 5 ? args->ArgV(4) : "status";

	if (0 != _stricmp(op, "probe")) {
		advancedfx::Message(
			"Usage: %s world shadowmap probe on|off|clear|status\n", cmd);
		return;
	}

	if (0 == _stricmp(mode, "on") || 0 == _stricmp(mode, "arm")) {
		ClearSeen();
		g_armed.store(true, std::memory_order_release);
		advancedfx::Message(
			"shadow-map resource probe armed; start mvm_debug and render a view.\n");
		return;
	}
	if (0 == _stricmp(mode, "off") || 0 == _stricmp(mode, "disarm")) {
		g_armed.store(false, std::memory_order_release);
		advancedfx::Message("shadow-map resource probe disarmed.\n");
		return;
	}
	if (0 == _stricmp(mode, "clear")) {
		ClearSeen();
		advancedfx::Message("shadow-map resource probe observations cleared.\n");
		return;
	}

	advancedfx::Message("shadow-map resource probe: %s events=%llu\n",
		g_armed.load(std::memory_order_acquire) ? "armed" : "off",
		g_events.load(std::memory_order_relaxed));
}

} // namespace Filmmaker
