#include "SkyGpuProbe.h"

#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <d3d11.h>
#include <d3d11shader.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <Windows.h>

namespace Filmmaker {

namespace {

using D3DReflectFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);

D3DReflectFn ResolveD3DReflect() {
	static D3DReflectFn result = []() -> D3DReflectFn {
		HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
		if (!module) return nullptr;
		return reinterpret_cast<D3DReflectFn>(GetProcAddress(module, "D3DReflect"));
	}();
	return result;
}

std::atomic<int> s_remaining{ 0 };
std::atomic<int> s_updateRemaining{ 0 };
std::atomic<bool> s_captureArmed{ false };
std::atomic<bool> s_effectsOnly{ false };
std::atomic<bool> s_structuredSampled{ false };
constexpr unsigned kTrackedStages = 2;
constexpr unsigned kTrackedSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
std::atomic<ID3D11Buffer*> s_skyBuffers[kTrackedStages][kTrackedSlots] = {};
std::atomic<unsigned> s_skyBufferBytes[kTrackedStages][kTrackedSlots] = {};
std::atomic<bool> s_skyCaptured[kTrackedStages][kTrackedSlots] = {};

struct ShaderRecord {
	std::vector<unsigned char> bytes;
	bool dumped = false;
};

std::map<void*, ShaderRecord> s_shaders;
size_t s_shaderBytes = 0;
constexpr size_t kShaderByteBudget = 128u * 1024u * 1024u;
std::map<void*, bool> s_reflectionLogged;

struct MappedSkyBuffer {
	ID3D11Resource* resource = nullptr;
	const unsigned char* data = nullptr;
	unsigned stage = 0;
	unsigned slot = 0;
	unsigned bytes = 0;
};

thread_local MappedSkyBuffer s_mappedSkyBuffers[kTrackedStages][kTrackedSlots];

bool ContainsInsensitive(const char* value, const char* needle) {
	if (!value || !needle || !*needle) return false;
	for (const char* start = value; *start; ++start) {
		const char* a = start;
		const char* b = needle;
		while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) {
			++a;
			++b;
		}
		if (!*b) return true;
	}
	return false;
}

bool ShaderName(ID3D11DeviceChild* shader, char* out, unsigned capacity) {
	if (!out || capacity < 2) return false;
	out[0] = '\0';
	if (!shader) return false;
	unsigned size = capacity - 1;
	if (FAILED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &size, out)) || size >= capacity)
		return false;
	out[size] = '\0';
	return true;
}

// The 3D sky is not submitted through SceneSystem's material batches. In CS2 it is a small
// indexed csgo_effects draw that samples the scene cubemap from PS slot 12. Keep this probe
// deliberately narrow: shader name, cubemap shape, draw size, and a full-size scene target all
// have to agree before the render hook is allowed to omit a draw.
bool IsPlayersOnlyGreenSkyDraw(ID3D11DeviceContext* context, unsigned drawArg0, unsigned drawArg1) {
	if (!context || drawArg0 == 0 || drawArg0 > 48 || drawArg1 != 1) return false;

	ID3D11PixelShader* pixelShader = nullptr;
	ID3D11VertexShader* vertexShader = nullptr;
	context->PSGetShader(&pixelShader, nullptr, nullptr);
	context->VSGetShader(&vertexShader, nullptr, nullptr);
	char psName[256] = {};
	char vsName[256] = {};
	ShaderName(pixelShader, psName, sizeof(psName));
	ShaderName(vertexShader, vsName, sizeof(vsName));
	const bool namedEffects = ContainsInsensitive(psName, "csgo_effects")
		|| ContainsInsensitive(vsName, "csgo_effects");
	if (pixelShader) pixelShader->Release();
	if (vertexShader) vertexShader->Release();
	if (!namedEffects) return false;

	ID3D11ShaderResourceView* cubeView = nullptr;
	context->PSGetShaderResources(12, 1, &cubeView);
	if (!cubeView) return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
	cubeView->GetDesc(&viewDesc);
	const bool isCube = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE
		|| viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
	D3D11_TEXTURE2D_DESC cubeDesc = {};
	if (isCube) {
		ID3D11Resource* resource = nullptr;
		cubeView->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture))
			&& texture) {
			texture->GetDesc(&cubeDesc);
		}
		if (texture) texture->Release();
		if (resource) resource->Release();
	}
	cubeView->Release();
	if (!isCube || cubeDesc.Width < 256 || cubeDesc.Height < 256
		|| cubeDesc.ArraySize < 6) return false;

	D3D11_VIEWPORT viewport = {};
	UINT viewportCount = 1;
	context->RSGetViewports(&viewportCount, &viewport);
	if (viewportCount == 0 || viewport.Width < 640.0f || viewport.Height < 480.0f)
		return false;

	ID3D11RenderTargetView* targetView = nullptr;
	context->OMGetRenderTargets(1, &targetView, nullptr);
	D3D11_TEXTURE2D_DESC targetDesc = {};
	if (targetView) {
		ID3D11Resource* resource = nullptr;
		targetView->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture))
			&& texture) {
			texture->GetDesc(&targetDesc);
		}
		if (texture) texture->Release();
		if (resource) resource->Release();
		targetView->Release();
	}
	if (targetDesc.Width < 640 || targetDesc.Height < 480) return false;
	// A reflection/shadow target can also be large. The sky draw covers nearly the entire current
	// scene viewport, so require the viewport to occupy most of the bound render target.
	if (viewport.Width < static_cast<float>(targetDesc.Width) * 0.75f
		|| viewport.Height < static_cast<float>(targetDesc.Height) * 0.75f)
		return false;
	return true;
}

const char* ViewDimensionName(D3D11_SRV_DIMENSION dimension) {
	switch (dimension) {
	case D3D11_SRV_DIMENSION_TEXTURECUBE: return "cube";
	case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY: return "cube-array";
	case D3D11_SRV_DIMENSION_TEXTURE2D: return "2d";
	case D3D11_SRV_DIMENSION_TEXTURE2DARRAY: return "2d-array";
	case D3D11_SRV_DIMENSION_BUFFER: return "buffer";
	case D3D11_SRV_DIMENSION_BUFFEREX: return "bufferex";
	default: return "other";
	}
}

void ClearCaptureState() {
	for (unsigned stage = 0; stage < kTrackedStages; ++stage) {
		for (unsigned slot = 0; slot < kTrackedSlots; ++slot) {
			s_skyBuffers[stage][slot].store(nullptr, std::memory_order_relaxed);
			s_skyBufferBytes[stage][slot].store(0, std::memory_order_relaxed);
			s_skyCaptured[stage][slot].store(false, std::memory_order_relaxed);
			s_mappedSkyBuffers[stage][slot] = {};
		}
	}
}

void RememberShader(void* shader, const void* bytecode, SIZE_T bytecodeBytes) {
	if (!shader || !bytecode || bytecodeBytes == 0 || bytecodeBytes > 16u * 1024u * 1024u)
		return;
	// Keep a bounded cache while the game creates shader variants. The sky shader is dumped on its
	// first matching draw, well before this budget is reached.
	while (s_shaderBytes + bytecodeBytes > kShaderByteBudget && !s_shaders.empty()) {
		auto it = s_shaders.begin();
		s_shaderBytes -= it->second.bytes.size();
		s_shaders.erase(it);
	}
	ShaderRecord& record = s_shaders[shader];
	s_shaderBytes -= record.bytes.size();
	record.bytes.assign(static_cast<const unsigned char*>(bytecode),
		static_cast<const unsigned char*>(bytecode) + bytecodeBytes);
	record.dumped = false;
	s_shaderBytes += record.bytes.size();
}

std::string ShaderDebugName(ID3D11DeviceChild* shader) {
	char name[256] = {};
	unsigned size = sizeof(name) - 1;
	if (!shader || FAILED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
		return {};
	name[size < sizeof(name) ? size : sizeof(name) - 1] = '\0';
	return name;
}

std::string DebugObjectName(ID3D11DeviceChild* object) {
	char name[256] = {};
	unsigned size = sizeof(name) - 1;
	if (!object || FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name))) return {};
	name[size < sizeof(name) ? size : sizeof(name) - 1] = '\0';
	return name;
}

void LogEffectResources(ID3D11DeviceContext* context, unsigned candidate) {
	if (!context || !s_effectsOnly.load(std::memory_order_acquire)) return;
	// csgo_effects.vfx reads the object and transform SSBOs through VS SRV slots 0/1.  The
	// resource names and structured-buffer sizes let us separate the sun-disc instance from
	// ordinary particle/effect draws without touching any game state.
	ID3D11ShaderResourceView* vsViews[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
	context->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, vsViews);
	for (unsigned slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
		ID3D11ShaderResourceView* view = vsViews[slot];
		if (!view) continue;
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
		view->GetDesc(&viewDesc);
		D3D11_BUFFER_DESC bufferDesc = {};
		if (resource) {
			ID3D11Buffer* buffer = nullptr;
			if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) && buffer) {
				buffer->GetDesc(&bufferDesc);
				buffer->Release();
			}
		}
		const std::string viewName = DebugObjectName(view);
		const std::string resourceName = DebugObjectName(resource);
		MvmDebugLog_LinefAlways("world.skygpu",
			"candidate=%u effect-vs-srv slot=%u view='%s' resource='%s' dim=%s first=%u count=%u bytes=%u stride=%u misc=0x%x",
			candidate, slot, viewName.empty() ? "?" : viewName.c_str(),
			resourceName.empty() ? "?" : resourceName.c_str(),
			ViewDimensionName(viewDesc.ViewDimension),
			viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : 0,
			viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.NumElements : 0,
			bufferDesc.ByteWidth,
			bufferDesc.StructureByteStride, bufferDesc.MiscFlags);
		if (resource) resource->Release();
		view->Release();
	}

	ID3D11Buffer* vertexBuffers[8] = {};
	UINT strides[8] = {}, offsets[8] = {};
	context->IAGetVertexBuffers(0, 8, vertexBuffers, strides, offsets);
	for (unsigned slot = 0; slot < 8; ++slot) {
		ID3D11Buffer* buffer = vertexBuffers[slot];
		if (!buffer) continue;
		D3D11_BUFFER_DESC desc = {};
		buffer->GetDesc(&desc);
		const std::string name = DebugObjectName(buffer);
		MvmDebugLog_LinefAlways("world.skygpu",
			"candidate=%u effect-ia-vb slot=%u name='%s' bytes=%u stride=%u offset=%u misc=0x%x",
			candidate, slot, name.empty() ? "?" : name.c_str(), desc.ByteWidth,
			strides[slot], offsets[slot], desc.MiscFlags);
		buffer->Release();
	}
	ID3D11Buffer* indexBuffer = nullptr;
	DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
	UINT indexOffset = 0;
	context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
	if (indexBuffer) {
		D3D11_BUFFER_DESC desc = {};
		indexBuffer->GetDesc(&desc);
		const std::string name = DebugObjectName(indexBuffer);
		MvmDebugLog_LinefAlways("world.skygpu",
			"candidate=%u effect-ia-ib name='%s' bytes=%u format=%d offset=%u",
			candidate, name.empty() ? "?" : name.c_str(), desc.ByteWidth,
			(int)indexFormat, indexOffset);
		indexBuffer->Release();
	}
}

void SampleStructuredBuffer(ID3D11DeviceContext* context, ID3D11Resource* source,
	const D3D11_SHADER_RESOURCE_VIEW_DESC& viewDesc, unsigned candidate, unsigned slot) {
	if (!context || !source || !MvmDebugLog_Active()) return;
	ID3D11Buffer* sourceBuffer = nullptr;
	if (FAILED(source->QueryInterface(__uuidof(ID3D11Buffer), (void**)&sourceBuffer)) || !sourceBuffer) return;
	D3D11_BUFFER_DESC sourceDesc = {};
	sourceBuffer->GetDesc(&sourceDesc);
	sourceBuffer->Release();
	if (!(sourceDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
		|| sourceDesc.ByteWidth == 0 || sourceDesc.ByteWidth > 16u * 1024u * 1024u) return;
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) return;
	D3D11_BUFFER_DESC stagingDesc = sourceDesc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;
	stagingDesc.StructureByteStride = 0;
	ID3D11Buffer* staging = nullptr;
	const HRESULT createResult = device->CreateBuffer(&stagingDesc, nullptr, &staging);
	device->Release();
	if (FAILED(createResult) || !staging) return;
	context->CopyResource(staging, source);
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	const HRESULT mapResult = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (SUCCEEDED(mapResult) && mapped.pData) {
		const unsigned offset = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER
			? viewDesc.Buffer.FirstElement * sourceDesc.StructureByteStride : 0;
		const unsigned available = sourceDesc.ByteWidth > offset ? sourceDesc.ByteWidth - offset : 0;
		const unsigned sampleBytes = available < 256 ? available : 256;
		const unsigned char* bytes = static_cast<const unsigned char*>(mapped.pData) + offset;
		char hex[769] = {};
		unsigned cursor = 0;
		for (unsigned i = 0; i < sampleBytes && cursor + 3 < sizeof(hex); ++i)
			cursor += (unsigned)_snprintf_s(hex + cursor, sizeof(hex) - cursor, _TRUNCATE, "%02X", bytes[i]);
		MvmDebugLog_LinefAlways("world.skygpu",
			"structured-sample candidate=%u slot=%u source=%p bytes=%u stride=%u firstElement=%u sample=%s",
			candidate, slot, (void*)source, sourceDesc.ByteWidth, sourceDesc.StructureByteStride,
			viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : 0, hex);
		context->Unmap(staging, 0);
	}
	staging->Release();
}

void SampleEffectStructuredBuffers(ID3D11DeviceContext* context, unsigned candidate) {
	if (!context || !s_effectsOnly.load(std::memory_order_acquire)
		|| s_structuredSampled.exchange(true, std::memory_order_acq_rel)) return;
	ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
	context->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
	for (unsigned slot : { 12u, 13u }) {
		ID3D11ShaderResourceView* view = views[slot];
		if (!view) continue;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
		view->GetDesc(&desc);
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		SampleStructuredBuffer(context, resource, desc, candidate, slot);
		if (resource) resource->Release();
		view->Release();
	}
}

void SampleEffectVertexBuffer(ID3D11DeviceContext* context, unsigned candidate) {
	if (!context || !s_effectsOnly.load(std::memory_order_acquire)) return;
	ID3D11Buffer* buffers[3] = {};
	UINT strides[3] = {}, offsets[3] = {};
	context->IAGetVertexBuffers(0, 3, buffers, strides, offsets);
	ID3D11Buffer* source = buffers[0];
	for (unsigned i = 1; i < 3; ++i) if (buffers[i]) buffers[i]->Release();
	if (!source || s_structuredSampled.load(std::memory_order_acquire) == false) {
		if (source) source->Release();
		return;
	}
	D3D11_BUFFER_DESC sourceDesc = {};
	source->GetDesc(&sourceDesc);
	if (sourceDesc.ByteWidth == 0 || sourceDesc.ByteWidth > 1024u * 1024u) {
		source->Release();
		return;
	}
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) { source->Release(); return; }
	D3D11_BUFFER_DESC stagingDesc = sourceDesc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;
	stagingDesc.StructureByteStride = 0;
	ID3D11Buffer* staging = nullptr;
	const HRESULT createResult = device->CreateBuffer(&stagingDesc, nullptr, &staging);
	device->Release();
	if (FAILED(createResult) || !staging) { source->Release(); return; }
	context->CopyResource(staging, source);
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
		const unsigned start = offsets[0] < sourceDesc.ByteWidth ? offsets[0] : 0;
		const unsigned available = sourceDesc.ByteWidth - start;
		const unsigned sampleBytes = available < 256 ? available : 256;
		const unsigned char* bytes = static_cast<const unsigned char*>(mapped.pData) + start;
		char hex[769] = {};
		unsigned cursor = 0;
		for (unsigned i = 0; i < sampleBytes && cursor + 3 < sizeof(hex); ++i)
			cursor += (unsigned)_snprintf_s(hex + cursor, sizeof(hex) - cursor, _TRUNCATE, "%02X", bytes[i]);
		MvmDebugLog_LinefAlways("world.skygpu",
			"vertex-sample candidate=%u source=%p bytes=%u stride=%u offset=%u sample=%s",
			candidate, (void*)source, sourceDesc.ByteWidth, strides[0], start, hex);
		context->Unmap(staging, 0);
	}
	staging->Release();
	source->Release();
}

void SampleEffectConstantBuffer(ID3D11DeviceContext* context, ID3D11Buffer* source,
	const char* stage, unsigned slot, unsigned candidate) {
	if (!context || !source || !stage || !s_effectsOnly.load(std::memory_order_acquire)) return;
	D3D11_BUFFER_DESC sourceDesc = {};
	source->GetDesc(&sourceDesc);
	if (sourceDesc.ByteWidth == 0 || sourceDesc.ByteWidth > 4096) return;
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) return;
	D3D11_BUFFER_DESC stagingDesc = {};
	stagingDesc.ByteWidth = sourceDesc.ByteWidth;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Buffer* staging = nullptr;
	const HRESULT createResult = device->CreateBuffer(&stagingDesc, nullptr, &staging);
	device->Release();
	if (FAILED(createResult) || !staging) return;
	context->CopyResource(staging, source);
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
		const unsigned count = sourceDesc.ByteWidth / sizeof(float) < 64
			? sourceDesc.ByteWidth / sizeof(float) : 64;
		const float* values = static_cast<const float*>(mapped.pData);
		char text[2048] = {};
		unsigned used = 0;
		for (unsigned i = 0; i < count && used + 32 < sizeof(text); ++i)
			used += (unsigned)_snprintf_s(text + used, sizeof(text) - used, _TRUNCATE,
				" %u=%.7g", i * 4, values[i]);
		MvmDebugLog_LinefAlways("world.skygpu",
			"effect-cb candidate=%u stage=%s slot=%u bytes=%u floats:%s",
			candidate, stage, slot, sourceDesc.ByteWidth, text);
		context->Unmap(staging, 0);
	}
	staging->Release();
}


void LogFloatMatrix(unsigned snapshot, unsigned stage, unsigned slot, const void* buffer, unsigned bytes,
	const float* values, unsigned matrixIndex) {
	const char* stageName = stage == 0 ? "VS" : "PS";
	const float* m = values + matrixIndex * 16;
	MvmDebugLog_LinefAlways("world.skygpu",
		"snapshot=%u cb=%s slot=%u buffer=%p bytes=%u matrix=%u "
		"[%.7g %.7g %.7g %.7g] [%.7g %.7g %.7g %.7g] "
		"[%.7g %.7g %.7g %.7g] [%.7g %.7g %.7g %.7g]",
		snapshot, stageName, slot, buffer, bytes, matrixIndex,
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
		m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

} // namespace

void SkyGpuProbe_Arm(int candidateLimit) {
	if (candidateLimit < 0) candidateLimit = 0;
	if (candidateLimit > 32) candidateLimit = 32;
	s_remaining.store(candidateLimit, std::memory_order_release);
	s_updateRemaining.store(0, std::memory_order_release);
	s_effectsOnly.store(false, std::memory_order_release);
	s_structuredSampled.store(false, std::memory_order_release);
	s_captureArmed.store(candidateLimit != 0, std::memory_order_release);
	ClearCaptureState();
	advancedfx::Message(
		"Sky GPU probe %s%s. Start mvm_debug first, then let the game draw a frame and read "
		"channel world.skygpu.\n",
		candidateLimit ? "armed for " : "disarmed", candidateLimit ? std::to_string(candidateLimit).c_str() : "");
}

void SkyGpuProbe_ArmEffects(int candidateLimit) {
	if (candidateLimit < 0) candidateLimit = 0;
	if (candidateLimit > 64) candidateLimit = 64;
	s_remaining.store(candidateLimit, std::memory_order_release);
	s_updateRemaining.store(candidateLimit > 0 ? candidateLimit * 8 : 0, std::memory_order_release);
	s_effectsOnly.store(candidateLimit != 0, std::memory_order_release);
	s_structuredSampled.store(false, std::memory_order_release);
	s_captureArmed.store(candidateLimit != 0, std::memory_order_release);
	ClearCaptureState();
	advancedfx::Message(
		"Sky/effect GPU probe %s%s (effect shaders only). Start mvm_debug first, then let the "
		"game draw a frame and read channel world.skygpu.\n",
		candidateLimit ? "armed for " : "disarmed", candidateLimit ? std::to_string(candidateLimit).c_str() : "");
}

bool SkyGpuProbe_IsPlayersOnlyGreenSkyDraw(ID3D11DeviceContext* context,
	unsigned drawArg0, unsigned drawArg1) {
	return IsPlayersOnlyGreenSkyDraw(context, drawArg0, drawArg1);
}

void SkyGpuProbe_NoteVertexShaderCreated(ID3D11VertexShader* shader, const void* bytecode,
	std::size_t bytecodeBytes) {
	RememberShader(shader, bytecode, bytecodeBytes);
}

void SkyGpuProbe_NotePixelShaderCreated(ID3D11PixelShader* shader, const void* bytecode,
	std::size_t bytecodeBytes) {
	RememberShader(shader, bytecode, bytecodeBytes);
}

void SkyGpuProbe_DumpBoundShader(ID3D11DeviceChild* shader, const char* stage) {
	if (!shader || !stage || !MvmDebugLog_Active()) return;
	auto it = s_shaders.find(shader);
	if (it == s_shaders.end() || it->second.dumped || it->second.bytes.empty()) return;
	const std::string name = ShaderDebugName(shader);
	char appData[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableA("APPDATA", appData, sizeof(appData));
	if (n == 0 || n >= sizeof(appData)) return;
	char path[MAX_PATH * 2] = {};
	_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\HLAE\\debuglogs\\sky_shader_%s_%p.bin",
		appData, stage, (void*)shader);
	FILE* file = nullptr;
	if (fopen_s(&file, path, "wb") != 0 || !file) return;
	fwrite(it->second.bytes.data(), 1, it->second.bytes.size(), file);
	fclose(file);
	it->second.dumped = true;
	MvmDebugLog_LinefAlways("world.skygpu",
		"shaderDump stage=%s name='%s' bytes=%zu path='%s'", stage,
		name.empty() ? "?" : name.c_str(), it->second.bytes.size(), path);
}

void SkyGpuProbe_LogShaderReflection(ID3D11DeviceChild* shader, const char* stage) {
	if (!shader || !stage || !MvmDebugLog_Active()) return;
	auto it = s_shaders.find(shader);
	if (it == s_shaders.end() || it->second.bytes.empty()) {
		MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s unavailable shader=%p recorded=%d",
			stage, (void*)shader, it == s_shaders.end() ? 0 : 1);
		return;
	}
	D3DReflectFn reflect = ResolveD3DReflect();
	if (!reflect) {
		MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s unavailable d3dcompiler", stage);
		return;
	}
	ID3D11ShaderReflection* reflection = nullptr;
	const HRESULT result = reflect(it->second.bytes.data(), it->second.bytes.size(),
		__uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&reflection));
	if (FAILED(result) || !reflection) {
		MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s failed hr=0x%08x bytes=%zu",
			stage, (unsigned)result, it->second.bytes.size());
		return;
	}
	D3D11_SHADER_DESC shaderDesc = {};
	reflection->GetDesc(&shaderDesc);
	MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s cbuffers=%u resources=%u",
		stage, shaderDesc.ConstantBuffers, shaderDesc.BoundResources);
	for (unsigned i = 0; i < shaderDesc.ConstantBuffers; ++i) {
		ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByIndex(i);
		D3D11_SHADER_BUFFER_DESC cbDesc = {};
		if (!cb || FAILED(cb->GetDesc(&cbDesc))) continue;
		MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s cbuffer=%u name='%s' bytes=%u vars=%u",
			stage, i, cbDesc.Name ? cbDesc.Name : "?", cbDesc.Size, cbDesc.Variables);
		for (unsigned var = 0; var < cbDesc.Variables; ++var) {
			ID3D11ShaderReflectionVariable* variable = cb->GetVariableByIndex(var);
			D3D11_SHADER_VARIABLE_DESC variableDesc = {};
			if (!variable || FAILED(variable->GetDesc(&variableDesc))) continue;
			MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s cbuffer=%u var=%u name='%s' offset=%u bytes=%u",
				stage, i, var, variableDesc.Name ? variableDesc.Name : "?",
				variableDesc.StartOffset, variableDesc.Size);
		}
	}
	for (unsigned i = 0; i < shaderDesc.BoundResources; ++i) {
		D3D11_SHADER_INPUT_BIND_DESC binding = {};
		if (FAILED(reflection->GetResourceBindingDesc(i, &binding))) continue;
		MvmDebugLog_LinefAlways("world.skygpu", "reflection stage=%s binding=%u name='%s' type=%d slot=%u count=%u",
			stage, i, binding.Name ? binding.Name : "?", (int)binding.Type,
			binding.BindPoint, binding.BindCount);
	}
	reflection->Release();
}

void SkyGpuProbe_NoteDraw(ID3D11DeviceContext* context, const char* operation,
	unsigned arg0, unsigned arg1, unsigned arg2, unsigned arg3, void* caller) {
	if (!context || s_remaining.load(std::memory_order_relaxed) <= 0 || !MvmDebugLog_Active()) return;

	ID3D11PixelShader* pixelShader = nullptr;
	ID3D11VertexShader* vertexShader = nullptr;
	context->PSGetShader(&pixelShader, nullptr, nullptr);
	context->VSGetShader(&vertexShader, nullptr, nullptr);
	char psName[256] = {};
	char vsName[256] = {};
	ShaderName(pixelShader, psName, sizeof(psName));
	ShaderName(vertexShader, vsName, sizeof(vsName));

	ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
	context->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
	struct CubeBinding { unsigned slot; D3D11_TEXTURE2D_DESC texture; D3D11_SRV_DIMENSION dimension; };
	CubeBinding cubes[8] = {};
	unsigned cubeCount = 0;
	for (unsigned slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
		ID3D11ShaderResourceView* view = views[slot];
		if (!view) continue;
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
		view->GetDesc(&viewDesc);
		if (viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE
			|| viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) {
			ID3D11Resource* resource = nullptr;
			view->GetResource(&resource);
			ID3D11Texture2D* texture = nullptr;
			if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture
				&& cubeCount < sizeof(cubes) / sizeof(cubes[0])) {
				cubes[cubeCount].slot = slot;
				cubes[cubeCount].dimension = viewDesc.ViewDimension;
				texture->GetDesc(&cubes[cubeCount].texture);
				++cubeCount;
			}
			if (texture) texture->Release();
			if (resource) resource->Release();
		}
		view->Release();
	}

	const bool namedSky = ContainsInsensitive(psName, "sky") || ContainsInsensitive(vsName, "sky");
	// The sun disc is not part of sky.vfx. It is a csgo_effects sprite rendered by the same
	// scene pass, so keep a bounded trace of that shader too. This is intentionally opt-in via
	// `world skyorient gputrace`; the normal hook still does no work unless the probe is armed.
	const bool namedEffect = ContainsInsensitive(psName, "effect") || ContainsInsensitive(vsName, "effect");
	// Some maps submit the sky-camera layer through a world shader rather than the named effect
	// variant. A bounded gputrace still needs to expose those cubemap draws so the green policy can
	// distinguish them from smoke/post-processing; normal frames remain a no-op while disarmed.
	if (!namedSky && !namedEffect) {
		if (pixelShader) pixelShader->Release();
		if (vertexShader) vertexShader->Release();
		return;
	}
	if (s_effectsOnly.load(std::memory_order_acquire) && !namedEffect) {
		if (pixelShader) pixelShader->Release();
		if (vertexShader) vertexShader->Release();
		return;
	}
	SkyGpuProbe_DumpBoundShader(vertexShader, "vs");
	SkyGpuProbe_DumpBoundShader(pixelShader, "ps");
	// The effect path (sun disc, glow, particles) is driven by structured buffers rather than
	// the constant buffers below.  Dump reflection once per shader so an opt-in trace also tells
	// us which SRV binding carries the object/transform data we can safely override.
	if (namedEffect) {
		if (vertexShader && !s_reflectionLogged[(void*)vertexShader]) {
			s_reflectionLogged[(void*)vertexShader] = true;
			SkyGpuProbe_LogShaderReflection(vertexShader, "VS-effect");
		}
		if (pixelShader && !s_reflectionLogged[(void*)pixelShader]) {
			s_reflectionLogged[(void*)pixelShader] = true;
			SkyGpuProbe_LogShaderReflection(pixelShader, "PS-effect");
		}
	}

	const int before = s_remaining.fetch_sub(1, std::memory_order_acq_rel);
	if (before <= 0) {
		s_remaining.store(0, std::memory_order_release);
		if (pixelShader) pixelShader->Release();
		if (vertexShader) vertexShader->Release();
		return;
	}

	D3D11_VIEWPORT viewport = {};
	unsigned viewportCount = 1;
	context->RSGetViewports(&viewportCount, &viewport);
	ID3D11RenderTargetView* targetView = nullptr;
	context->OMGetRenderTargets(1, &targetView, nullptr);
	D3D11_TEXTURE2D_DESC target = {};
	if (targetView) {
		ID3D11Resource* resource = nullptr;
		targetView->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture) {
			texture->GetDesc(&target);
			texture->Release();
		}
		if (resource) resource->Release();
		targetView->Release();
	}

	MvmDebugLog_LinefAlways("world.skygpu",
		"candidate=%d op=%s args=%u,%u,%u,%u caller=%p ps='%s' vs='%s' namedSky=%d namedEffect=%d cubes=%u "
		"target=%ux%u fmt=%d samples=%u vp=%.1f,%.1f %.1fx%.1f",
		before, operation ? operation : "?", arg0, arg1, arg2, arg3, caller,
		psName[0] ? psName : "?", vsName[0] ? vsName : "?", namedSky ? 1 : 0,
		namedEffect ? 1 : 0, cubeCount,
		target.Width, target.Height, (int)target.Format, target.SampleDesc.Count,
		viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height);
	for (unsigned i = 0; i < cubeCount; ++i) {
		const D3D11_TEXTURE2D_DESC& d = cubes[i].texture;
		MvmDebugLog_LinefAlways("world.skygpu",
			"candidate=%d cube slot=%u dim=%s texture=%ux%u array=%u mips=%u fmt=%d misc=0x%x",
			before, cubes[i].slot, ViewDimensionName(cubes[i].dimension), d.Width, d.Height,
			d.ArraySize, d.MipLevels, (int)d.Format, d.MiscFlags);
	}

	ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
	context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
	for (unsigned slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
		if (!buffers[slot]) continue;
		D3D11_BUFFER_DESC d = {};
		buffers[slot]->GetDesc(&d);
		s_skyBuffers[0][slot].store(buffers[slot], std::memory_order_release);
		s_skyBufferBytes[0][slot].store(d.ByteWidth, std::memory_order_release);
		MvmDebugLog_LinefAlways("world.skygpu", "candidate=%d cb stage=VS slot=%u buffer=%p bytes=%u usage=%d cpu=0x%x",
			before, slot, (void*)buffers[slot], d.ByteWidth, (int)d.Usage, d.CPUAccessFlags);
		if (namedEffect && slot == 0) SampleEffectConstantBuffer(context, buffers[slot], "VS", slot, static_cast<unsigned>(before));
		buffers[slot]->Release();
	}
	context->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
	for (unsigned slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
		if (!buffers[slot]) continue;
		D3D11_BUFFER_DESC d = {};
		buffers[slot]->GetDesc(&d);
		s_skyBuffers[1][slot].store(buffers[slot], std::memory_order_release);
		s_skyBufferBytes[1][slot].store(d.ByteWidth, std::memory_order_release);
		MvmDebugLog_LinefAlways("world.skygpu", "candidate=%d cb stage=PS slot=%u buffer=%p bytes=%u usage=%d cpu=0x%x",
			before, slot, (void*)buffers[slot], d.ByteWidth, (int)d.Usage, d.CPUAccessFlags);
		if (namedEffect && slot == 0) SampleEffectConstantBuffer(context, buffers[slot], "PS", slot, static_cast<unsigned>(before));
		buffers[slot]->Release();
	}
	LogEffectResources(context, static_cast<unsigned>(before));
	SampleEffectStructuredBuffers(context, static_cast<unsigned>(before));
	SampleEffectVertexBuffer(context, static_cast<unsigned>(before));

	if (pixelShader) pixelShader->Release();
	if (vertexShader) vertexShader->Release();
	if (before == 1) MvmDebugLog_LinefAlways("world.skygpu", "probe complete");
}

void SkyGpuProbe_NoteMap(ID3D11Resource* resource, void* data) {
	if (!resource || !data || !s_captureArmed.load(std::memory_order_acquire)) return;
	for (unsigned stage = 0; stage < kTrackedStages; ++stage) {
		for (unsigned slot = 0; slot < kTrackedSlots; ++slot) {
			if (resource != s_skyBuffers[stage][slot].load(std::memory_order_acquire)) continue;
			s_mappedSkyBuffers[stage][slot] = { resource, static_cast<const unsigned char*>(data), stage, slot,
				s_skyBufferBytes[stage][slot].load(std::memory_order_acquire) };
		}
	}
}

void SkyGpuProbe_NoteUnmap(ID3D11Resource* resource) {
	if (!resource || !s_captureArmed.load(std::memory_order_acquire) || !MvmDebugLog_Active()) return;
	for (unsigned stage = 0; stage < kTrackedStages; ++stage) {
		for (unsigned slot = 0; slot < kTrackedSlots; ++slot) {
			MappedSkyBuffer& mapped = s_mappedSkyBuffers[stage][slot];
			if (mapped.resource != resource || !mapped.data || mapped.bytes < 64) continue;
			mapped.resource = nullptr;
			if (s_skyCaptured[stage][slot].exchange(true, std::memory_order_acq_rel)) continue;

			const unsigned matrixCount = (mapped.bytes / 64) < 16 ? (mapped.bytes / 64) : 16;
			const unsigned snapshot = stage * kTrackedSlots + slot + 1;
			for (unsigned matrix = 0; matrix < matrixCount; ++matrix)
				LogFloatMatrix(snapshot, stage, slot, resource, mapped.bytes,
					reinterpret_cast<const float*>(mapped.data), matrix);
		}
	}

	// Effect-only tracing is interested in a draw, not in the sky constant-buffer uploads. No
	// sky buffer is expected to be mapped in that mode, so the normal upload-complete test would
	// disarm it on the first unrelated Map/Unmap before the sun sprite is submitted.
	if (!s_effectsOnly.load(std::memory_order_acquire)) {
		bool complete = true;
		for (unsigned stage = 0; stage < kTrackedStages; ++stage) {
			for (unsigned slot = 0; slot < kTrackedSlots; ++slot) {
				if (s_skyBuffers[stage][slot].load(std::memory_order_acquire)
					&& !s_skyCaptured[stage][slot].load(std::memory_order_acquire)) complete = false;
			}
		}
		if (complete) {
			s_captureArmed.store(false, std::memory_order_release);
			MvmDebugLog_LinefAlways("world.skygpu", "constant-buffer upload capture complete");
		}
	}
}

void SkyGpuProbe_NoteUpdateSubresource(ID3D11Resource* resource, const void* data,
	unsigned rowPitch, unsigned depthPitch) {
	if (!resource || !data || !s_effectsOnly.load(std::memory_order_acquire)
		|| !s_captureArmed.load(std::memory_order_acquire) || !MvmDebugLog_Active()) return;
	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer) return;
	D3D11_BUFFER_DESC desc = {};
	buffer->GetDesc(&desc);
	buffer->Release();
	if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
		|| (desc.StructureByteStride != 32 && desc.StructureByteStride != 48)
		|| desc.ByteWidth == 0 || rowPitch == 0) return;
	const int before = s_updateRemaining.fetch_sub(1, std::memory_order_acq_rel);
	if (before <= 0) return;
	const unsigned byteCount = rowPitch < desc.ByteWidth ? rowPitch : desc.ByteWidth;
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	const unsigned dumpBytes = byteCount < 64 ? byteCount : 64;
	char hex[193] = {};
	unsigned cursor = 0;
	for (unsigned i = 0; i < dumpBytes && cursor + 3 < sizeof(hex); ++i)
		cursor += (unsigned)_snprintf_s(hex + cursor, sizeof(hex) - cursor, _TRUNCATE, "%02X", bytes[i]);
	MvmDebugLog_LinefAlways("world.skygpu",
		"structured-update=%d resource=%p bytes=%u stride=%u rowPitch=%u depthPitch=%u first64=%s",
		before, (void*)resource, desc.ByteWidth, desc.StructureByteStride, rowPitch, depthPitch, hex);
}

} // namespace Filmmaker
