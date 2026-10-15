#include "SkyGpuRotation.h"

#include "SkyOrient.h"
#include "SkyGpuProbe.h"
#include "SunRotationMath.h"
#include "LightFx.h"
#include "SunFx.h"
#include "../Debug/MvmDebugLog.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <vector>

namespace Filmmaker {

namespace {

constexpr unsigned kSkyUploadBytes = 4096;
// The stock sky shader declares SkyWsToOs as a material matrix. Its D3D11 constant-buffer slot
// is permutation-dependent, so cache all small dynamic constant buffers and identify that matrix
// from the sky draw before applying the orientation override.

struct CachedUpload {
	ID3D11Resource* resource = nullptr;
	unsigned bytesUsed = 0;
	unsigned char bytes[kSkyUploadBytes] = {};
};

struct ActiveMap {
	ID3D11Resource* resource = nullptr;
	const unsigned char* data = nullptr;
};

CachedUpload s_uploads[64] = {};
unsigned s_nextUpload = 0;
thread_local ActiveMap s_activeMap;
ID3D11Buffer* s_rotatedConstants = nullptr;
bool s_loggedSkyBuffers = false;

enum class SunDrawKind { None, Disc, LargeGlow };

struct SunDrawAnchor {
	bool valid = false;
	// Midpoint of the high-boost disc pair (Mirage submits two separate triangles). Glow pairing
	// and bloom scaling both use this shared point so the pieces cannot drift apart.
	SunRotationMath::Vec3 authoredCenter{};
	float camera[3] = {};
	SunRotationMath::Vec3 pairAccum{};
	int pairCount = 0;
	SunRotationMath::Vec3 stableCenter{};
	bool hasStableCenter = false;
};

thread_local SunDrawAnchor s_sunDrawAnchor;

float Distance3(SunRotationMath::Vec3 a, SunRotationMath::Vec3 b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float AabbDiagonal(SunRotationMath::Vec3 minimum, SunRotationMath::Vec3 maximum) {
	return Distance3(minimum, maximum);
}

bool NearDiscAnchor(SunRotationMath::Vec3 point, SunRotationMath::Vec3 minimum,
	SunRotationMath::Vec3 maximum, SunRotationMath::Vec3 anchor) {
	// Runtime evidence 2026-08-05: AABB BoundsContain failed for the companion glow because the
	// disc is two triangle centroids, neither of which sits inside the glow AABB, while their
	// shared midpoint does. Accept anything whose centroid is within the glow's own span of that
	// shared anchor (with a small floor so tiny draws still pair).
	const float span = AabbDiagonal(minimum, maximum);
	const float limit = (std::max)(span * 0.75f, 200.0f);
	return Distance3(point, anchor) <= limit;
}

struct SunVertexCache {
	ID3D11Buffer* source = nullptr;
	ID3D11Buffer* clone = nullptr;
	unsigned bytes = 0;
	unsigned stride = 0;
	std::vector<unsigned char> original;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float glowSize = 1.0f;
	bool mirror = false;
	bool transformed = false;
	float camera[3] = {};
};

SunVertexCache s_sunVertexCaches[64] = {};

bool HasSkyOrientationOverride() {
	return SkyOrient_SkyEnabled() && (std::fabs(SkyOrient_Yaw()) > 0.001f
		|| std::fabs(SkyOrient_Pitch()) > 0.001f
		|| SkyOrient_Mirror());
}

bool HasSunOrientationOverride() {
	return std::fabs(LightFx_SunYaw()) > 0.001f
		|| std::fabs(LightFx_SunPitch()) > 0.001f
		|| std::fabs(SunFx_GlowSize() - 1.0f) > 0.001f;
}

bool ShaderNameContains(ID3D11DeviceChild* shader, const char* needle) {
	char name[256] = {};
	unsigned size = sizeof(name) - 1;
	if (!shader || FAILED(shader->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
		return false;
	name[size < sizeof(name) ? size : sizeof(name) - 1] = '\0';
	if (!needle || !*needle) return false;
	for (char* p = name; *p; ++p) {
		const char* n = needle;
		char* q = p;
		while (*q && *n && tolower((unsigned char)*q) == tolower((unsigned char)*n)) {
			++q;
			++n;
		}
		if (!*n) return true;
	}
	return false;
}

bool IsSkyDraw(ID3D11DeviceContext* context) {
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	context->VSGetShader(&vs, nullptr, nullptr);
	context->PSGetShader(&ps, nullptr, nullptr);
	const bool result = ShaderNameContains(vs, "sky") || ShaderNameContains(ps, "sky");
	if (vs) vs->Release();
	if (ps) ps->Release();
	return result;
}

bool IsSunSpriteCandidate(ID3D11DeviceContext* context, ID3D11Buffer** outVertexBuffer,
	unsigned* outStride, unsigned* outOffset) {
	if (!context) return false;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	context->VSGetShader(&vs, nullptr, nullptr);
	context->PSGetShader(&ps, nullptr, nullptr);
	const bool effect = ShaderNameContains(vs, "effects") || ShaderNameContains(ps, "effects");
	if (vs) vs->Release();
	if (ps) ps->Release();
	if (!effect) return false;
	ID3D11Buffer* vb = nullptr;
	UINT stride = 0, offset = 0;
	context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
	ID3D11Buffer* ib = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT indexOffset = 0;
	context->IAGetIndexBuffer(&ib, &format, &indexOffset);
	D3D11_BUFFER_DESC vbDesc = {}, ibDesc = {};
	if (vb) vb->GetDesc(&vbDesc);
	if (ib) ib->GetDesc(&ibDesc);
	if (ib) ib->Release();
	const unsigned vertexCount = stride ? vbDesc.ByteWidth / stride : 0;
	// The shipped disc is a quad and its broad halo is two triangle draws. Material and measured
	// geometry classification happens after the original vertices have been cached.
	const bool result = vb && stride >= 12 && stride <= 64
		&& vbDesc.ByteWidth % stride == 0 && vertexCount >= 3 && vertexCount <= 6
		&& ibDesc.ByteWidth <= 64 && format != DXGI_FORMAT_UNKNOWN;
	if (!result) {
		if (vb) vb->Release();
		return false;
	}
	if (outVertexBuffer) *outVertexBuffer = vb;
	else vb->Release();
	if (outStride) *outStride = stride;
	if (outOffset) *outOffset = offset;
	return true;
}

const CachedUpload* FindUpload(ID3D11Resource* resource) {
	for (const CachedUpload& upload : s_uploads)
		if (upload.resource == resource) return &upload;
	return nullptr;
}

// g_matSkyWsToOs is already Z-up: the engine's own matrix is a pure rotation about its third axis
// (the map's authored sky yaw; logged on Inferno as [-0.42 -0.91 0][0.91 -0.42 0][0 0 1]). The yaw
// therefore composes about that same axis, so Rotate SPINS the sky about world up and the horizon
// stays level. A fixed "consumer basis" conjugation (X->Z, Y->X, Z->Y) used to sit here: it moved
// the yaw onto a horizontal axis, so the Yaw slider tumbled the sky instead of spinning it.
SunRotationMath::Quat SkyGpuDelta() {
	return SunRotationMath::FromYawPitch(SkyOrient_Yaw(), SkyOrient_Pitch());
}

void RotateSkyMatrix(unsigned char* bytes, unsigned matrixOffset,
	SunRotationMath::Quat delta, bool mirror) {
	float* matrix = reinterpret_cast<float*>(bytes + matrixOffset);
	float r[9] = {};
	SunRotationMath::ToMatrix3(delta, r);

	// Post-multiply the world-to-object axes as row vectors.  Translation stays untouched.  The
	// transpose-looking indexing is intentional: ToMatrix3 is the conventional column-vector
	// form, while this constant buffer is consumed by a row-vector shader path.
	for (unsigned row = 0; row < 3; ++row) {
		const float x = matrix[row * 4 + 0] * (mirror ? -1.0f : 1.0f);
		const float y = matrix[row * 4 + 1];
		const float z = matrix[row * 4 + 2];
		matrix[row * 4 + 0] = x * r[0] + y * r[3] + z * r[6];
		matrix[row * 4 + 1] = x * r[1] + y * r[4] + z * r[7];
		matrix[row * 4 + 2] = x * r[2] + y * r[5] + z * r[8];
	}
}

bool EnsureRotatedConstants(ID3D11DeviceContext* context) {
	if (s_rotatedConstants) return true;
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) return false;
	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = 128;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	const HRESULT result = device->CreateBuffer(&desc, nullptr, &s_rotatedConstants);
	device->Release();
	return SUCCEEDED(result) && s_rotatedConstants;
}

bool ReadCameraPosition(ID3D11DeviceContext* context, float out[3]) {
	if (!context || !out) return false;
	ID3D11Buffer* buffer = nullptr;
	context->VSGetConstantBuffers(1, 1, &buffer);
	const CachedUpload* upload = FindUpload(buffer);
	if (buffer) buffer->Release();
	if (!upload || upload->bytesUsed < 428) return false;
	const float* values = reinterpret_cast<const float*>(upload->bytes + 416);
	if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || !std::isfinite(values[2])) return false;
	out[0] = values[0]; out[1] = values[1]; out[2] = values[2];
	return true;
}

bool ReadBufferToCpu(ID3D11DeviceContext* context, ID3D11Buffer* source,
	std::vector<unsigned char>& out, unsigned bytes) {
	if (!context || !source || bytes == 0 || bytes > 1024u * 1024u) return false;
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) return false;
	D3D11_BUFFER_DESC stagingDesc = {};
	stagingDesc.ByteWidth = bytes;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Buffer* staging = nullptr;
	const HRESULT createResult = device->CreateBuffer(&stagingDesc, nullptr, &staging);
	device->Release();
	if (FAILED(createResult) || !staging) return false;
	context->CopyResource(staging, source);
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	const HRESULT mapResult = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	bool result = false;
	if (SUCCEEDED(mapResult) && mapped.pData) {
		out.resize(bytes);
		std::memcpy(out.data(), mapped.pData, bytes);
		context->Unmap(staging, 0);
		result = true;
	}
	staging->Release();
	return result;
}

float ReadEffectColorBoost(ID3D11DeviceContext* context) {
	if (!context) return NAN;
	ID3D11Buffer* pixelConstants = nullptr;
	context->PSGetConstantBuffers(0, 1, &pixelConstants);
	if (!pixelConstants) return NAN;

	D3D11_BUFFER_DESC desc = {};
	pixelConstants->GetDesc(&desc);
	std::vector<unsigned char> bytes;
	float colorBoost = NAN;
	if (desc.ByteWidth >= 112 && desc.ByteWidth <= 4096
		&& ReadBufferToCpu(context, pixelConstants, bytes, desc.ByteWidth)) {
		// Measured from csgo_effects.vfx_ps: g_flColorBoost is float offset 108. The compact
		// disc uses a very high value; the broad companion glow deliberately uses the normal 1.0
		// value and is therefore identified from its bounds relative to that disc below.
		colorBoost = *reinterpret_cast<const float*>(bytes.data() + 108);
	}
	pixelConstants->Release();
	return colorBoost;
}

SunVertexCache* GetSunVertexCache(ID3D11DeviceContext* context, ID3D11Buffer* source,
	unsigned bytes, unsigned stride) {
	for (SunVertexCache& cache : s_sunVertexCaches)
		if (cache.source == source) return &cache;
	SunVertexCache* selected = nullptr;
	for (SunVertexCache& cache : s_sunVertexCaches) {
		if (!cache.source) { selected = &cache; break; }
	}
	if (!selected) selected = &s_sunVertexCaches[0];
	if (selected->source) selected->source->Release();
	if (selected->clone) selected->clone->Release();
	*selected = {};
	source->AddRef();
	selected->source = source;
	selected->bytes = bytes;
	selected->stride = stride;
	if (!ReadBufferToCpu(context, source, selected->original, bytes)) {
		selected->source->Release();
		selected->source = nullptr;
		return nullptr;
	}
	ID3D11Device* device = nullptr;
	context->GetDevice(&device);
	if (!device) { selected->source->Release(); selected->source = nullptr; return nullptr; }
	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA initial = {};
	initial.pSysMem = selected->original.data();
	const HRESULT result = device->CreateBuffer(&desc, &initial, &selected->clone);
	device->Release();
	if (FAILED(result) || !selected->clone) {
		selected->source->Release();
		selected->source = nullptr;
		selected->original.clear();
		return nullptr;
	}
	return selected;
}

bool ApplySunVertexRotation(ID3D11DeviceContext* context, SkyGpuRotationScope& scope,
	ID3D11Buffer* source, unsigned stride, unsigned offset) {
	if (!context || !source || stride < 12) return false;
	D3D11_BUFFER_DESC sourceDesc = {};
	source->GetDesc(&sourceDesc);
	SunVertexCache* cache = GetSunVertexCache(context, source, sourceDesc.ByteWidth, stride);
	if (!cache || !cache->clone) return false;
	// Always refresh from the live VB. Never write transformed verts back into source: a prior
	// cull-proxy experiment did that and turned the sun into sky-sized white triangles.
	if (!ReadBufferToCpu(context, source, cache->original, sourceDesc.ByteWidth)
		|| cache->original.size() < sourceDesc.ByteWidth) return false;
	const float yaw = LightFx_SunYaw();
	const float pitch = LightFx_SunPitch();
	// The sliders turn about the map's own sun (LightFx.h); before it is known, about this disc.
	float referenceSun[3] = {};
	const bool haveReference = LightFx_SunReference(referenceSun);
	const float glowSize = SunFx_GlowSize();
	const bool mirror = false;
	float camera[3] = {};
	if (!ReadCameraPosition(context, camera)) return false;
	const unsigned count = sourceDesc.ByteWidth / stride;
	SunRotationMath::Vec3 center{};
	SunRotationMath::Vec3 minimum{ INFINITY, INFINITY, INFINITY };
	SunRotationMath::Vec3 maximum{ -INFINITY, -INFINITY, -INFINITY };
	for (unsigned i = 0; i < count; ++i) {
		const float* position = reinterpret_cast<const float*>(cache->original.data() + i * stride);
		center.x += position[0]; center.y += position[1]; center.z += position[2];
		minimum.x = (std::min)(minimum.x, position[0]);
		minimum.y = (std::min)(minimum.y, position[1]);
		minimum.z = (std::min)(minimum.z, position[2]);
		maximum.x = (std::max)(maximum.x, position[0]);
		maximum.y = (std::max)(maximum.y, position[1]);
		maximum.z = (std::max)(maximum.z, position[2]);
	}
	const float invCount = 1.0f / (float)count;
	center.x *= invCount; center.y *= invCount; center.z *= invCount;
	const float colorBoost = ReadEffectColorBoost(context);
	SunDrawKind kind = SunDrawKind::None;
	const bool boostDisc = std::isfinite(colorBoost) && colorBoost > 4.0f;
	const bool boostGlowBand = std::isfinite(colorBoost) && colorBoost >= 0.5f && colorBoost <= 2.0f;
	const bool camMatch = s_sunDrawAnchor.valid
		&& s_sunDrawAnchor.camera[0] == camera[0]
		&& s_sunDrawAnchor.camera[1] == camera[1]
		&& s_sunDrawAnchor.camera[2] == camera[2];
	const SunRotationMath::Vec3 pairAnchor = s_sunDrawAnchor.hasStableCenter
		? s_sunDrawAnchor.stableCenter
		: s_sunDrawAnchor.authoredCenter;
	const bool nearOk = s_sunDrawAnchor.valid
		&& NearDiscAnchor(center, minimum, maximum, pairAnchor);
	if (boostDisc) {
		kind = SunDrawKind::Disc;
		// Mirage submits two high-boost triangles per frame. Reset after each completed pair so the
		// shared midpoint tracks the current frame instead of averaging forever.
		if (!camMatch || s_sunDrawAnchor.pairCount >= 2) {
			s_sunDrawAnchor.pairAccum = center;
			s_sunDrawAnchor.pairCount = 1;
		} else {
			s_sunDrawAnchor.pairAccum.x += center.x;
			s_sunDrawAnchor.pairAccum.y += center.y;
			s_sunDrawAnchor.pairAccum.z += center.z;
			s_sunDrawAnchor.pairCount += 1;
		}
		const float invPair = 1.0f / (float)s_sunDrawAnchor.pairCount;
		s_sunDrawAnchor.authoredCenter = {
			s_sunDrawAnchor.pairAccum.x * invPair,
			s_sunDrawAnchor.pairAccum.y * invPair,
			s_sunDrawAnchor.pairAccum.z * invPair };
		if (s_sunDrawAnchor.pairCount >= 2) {
			s_sunDrawAnchor.stableCenter = s_sunDrawAnchor.authoredCenter;
			s_sunDrawAnchor.hasStableCenter = true;
		}
		s_sunDrawAnchor.valid = true;
		s_sunDrawAnchor.camera[0] = camera[0];
		s_sunDrawAnchor.camera[1] = camera[1];
		s_sunDrawAnchor.camera[2] = camera[2];
	} else if (boostGlowBand && s_sunDrawAnchor.valid && camMatch && nearOk) {
		kind = SunDrawKind::LargeGlow;
	}
	if (kind == SunDrawKind::None) return false;
	// Bloom size must scale both disc triangles around their shared midpoint. Scaling each about
	// its own centroid pulled them apart and read as "two triangles".
	const SunRotationMath::Vec3 scaleCenter = s_sunDrawAnchor.hasStableCenter
		? s_sunDrawAnchor.stableCenter
		: (kind == SunDrawKind::Disc ? s_sunDrawAnchor.authoredCenter : pairAnchor);
	const SunRotationMath::Vec3 reference = haveReference
		? SunRotationMath::Vec3{ referenceSun[0], referenceSun[1], referenceSun[2] }
		: SunRotationMath::Vec3{ center.x - camera[0], center.y - camera[1], center.z - camera[2] };
	std::vector<unsigned char> transformed = cache->original;
	const SunRotationMath::Vec3 movedCenter = SunRotationMath::RotateAroundPoint(center,
		{ camera[0], camera[1], camera[2] }, yaw, pitch, reference);
	for (unsigned i = 0; i < count; ++i) {
		float* position = reinterpret_cast<float*>(transformed.data() + i * stride);
		// Disc pair + companion glow scale around the shared disc midpoint, then every vertex is
		// rigidly rotated around the camera from the engine's fresh view-aligned snapshot.
		const SunRotationMath::Vec3 scaled{
			scaleCenter.x + (position[0] - scaleCenter.x) * glowSize,
			scaleCenter.y + (position[1] - scaleCenter.y) * glowSize,
			scaleCenter.z + (position[2] - scaleCenter.z) * glowSize };
		const SunRotationMath::Vec3 rotated = SunRotationMath::RotateAroundPoint(scaled,
			{ camera[0], camera[1], camera[2] }, yaw, pitch, reference);
		position[0] = rotated.x;
		position[1] = rotated.y;
		position[2] = rotated.z;
	}
	context->UpdateSubresource(cache->clone, 0, nullptr, transformed.data(), 0, 0);
	const bool loggedChange = !cache->transformed || cache->yaw != yaw || cache->pitch != pitch
		|| cache->glowSize != glowSize || cache->mirror != mirror
		|| cache->camera[0] != camera[0] || cache->camera[1] != camera[1]
		|| cache->camera[2] != camera[2];
	cache->yaw = yaw;
	cache->pitch = pitch;
	cache->glowSize = glowSize;
	cache->mirror = mirror;
	cache->camera[0] = camera[0]; cache->camera[1] = camera[1]; cache->camera[2] = camera[2];
	cache->transformed = true;
	if (loggedChange && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("world.skygpu.rotation",
			"sun billboard transform kind=%s source=%p clone=%p boost=%.3f yaw=%.3f pitch=%.3f size=%.3f source=scene-lights camera=(%.1f %.1f %.1f) center=(%.1f %.1f %.1f)->(%.1f %.1f %.1f) scaleAnchor=(%.1f %.1f %.1f)",
			kind == SunDrawKind::Disc ? "disc" : "large-glow",
			(void*)source, (void*)cache->clone, colorBoost, yaw, pitch, glowSize,
			camera[0], camera[1], camera[2], center.x, center.y, center.z,
			movedCenter.x, movedCenter.y, movedCenter.z,
			scaleCenter.x, scaleCenter.y, scaleCenter.z);
	UINT strides = stride, offsets = offset;
	context->IASetVertexBuffers(0, 1, &cache->clone, &strides, &offsets);
	scope.originalSunVertexBuffer = source;
	scope.sunVertexStride = stride;
	scope.sunVertexOffset = offset;
	return true;
}

} // namespace

void SkyGpuRotation_NoteMap(ID3D11Resource* resource, void* data) {
	if ((!HasSkyOrientationOverride() && !HasSunOrientationOverride()) || !resource || !data) return;
	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer) return;
	D3D11_BUFFER_DESC desc = {};
	buffer->GetDesc(&desc);
	buffer->Release();
	if (desc.ByteWidth == 0 || desc.ByteWidth > kSkyUploadBytes || desc.Usage != D3D11_USAGE_DYNAMIC
		|| !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
	s_activeMap = { resource, static_cast<const unsigned char*>(data) };
}

void SkyGpuRotation_NoteUnmap(ID3D11Resource* resource) {
	if (!resource || s_activeMap.resource != resource || !s_activeMap.data) return;
	CachedUpload& upload = s_uploads[s_nextUpload++ % (sizeof(s_uploads) / sizeof(s_uploads[0]))];
	upload.resource = resource;
	ID3D11Buffer* buffer = nullptr;
	if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buffer)) || !buffer) {
		s_activeMap = {};
		return;
	}
	D3D11_BUFFER_DESC desc = {};
	buffer->GetDesc(&desc);
	buffer->Release();
	if (desc.ByteWidth == 0 || desc.ByteWidth > kSkyUploadBytes) {
		s_activeMap = {};
		return;
	}
	upload.bytesUsed = desc.ByteWidth;
	std::memcpy(upload.bytes, s_activeMap.data, upload.bytesUsed);
	s_activeMap = {};
}

SkyGpuRotationScope SkyGpuRotation_BeginDraw(ID3D11DeviceContext* context) {
	SkyGpuRotationScope scope;
	if (!context || (!HasSkyOrientationOverride() && !HasSunOrientationOverride())) return scope;
	if (!IsSkyDraw(context)) {
		ID3D11Buffer* sunVertexBuffer = nullptr;
		unsigned sunStride = 0, sunOffset = 0;
		if (IsSunSpriteCandidate(context, &sunVertexBuffer, &sunStride, &sunOffset)) {
			if (!ApplySunVertexRotation(context, scope, sunVertexBuffer, sunStride, sunOffset))
				sunVertexBuffer->Release();
		}
		return scope;
	}
	if (!HasSkyOrientationOverride()) return scope;

	ID3D11Buffer* originalVs = nullptr;
	context->VSGetConstantBuffers(0, 1, &originalVs);
	const CachedUpload* vsUpload = FindUpload(originalVs);
	if (!s_loggedSkyBuffers && MvmDebugLog_Active()) {
		s_loggedSkyBuffers = true;
		ID3D11VertexShader* shaderVs = nullptr;
		ID3D11PixelShader* shaderPs = nullptr;
		context->VSGetShader(&shaderVs, nullptr, nullptr);
		context->PSGetShader(&shaderPs, nullptr, nullptr);
		SkyGpuProbe_LogShaderReflection(shaderVs, "VS");
		SkyGpuProbe_LogShaderReflection(shaderPs, "PS");
		if (shaderVs) shaderVs->Release();
		if (shaderPs) shaderPs->Release();
		ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
		for (unsigned stage = 0; stage < 2; ++stage) {
			if (stage == 0) context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			else context->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers);
			for (unsigned slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot) {
				if (!buffers[slot]) continue;
				const CachedUpload* upload = FindUpload(buffers[slot]);
				D3D11_BUFFER_DESC desc = {};
				buffers[slot]->GetDesc(&desc);
				MvmDebugLog_LinefAlways("world.skygpu.rotation", "candidate stage=%s slot=%u buffer=%p bytes=%u cached=%d",
					stage == 0 ? "VS" : "PS", slot, (void*)buffers[slot], desc.ByteWidth, upload ? 1 : 0);
				if (upload && upload->bytesUsed >= 64) {
					const unsigned matrixCount = (upload->bytesUsed / 64) < 16 ? upload->bytesUsed / 64 : 16;
					for (unsigned matrix = 0; matrix < matrixCount; ++matrix) {
						const float* m = reinterpret_cast<const float*>(upload->bytes + matrix * 64);
						MvmDebugLog_LinefAlways("world.skygpu.rotation", "candidate stage=%s slot=%u matrix=%u [%.7g %.7g %.7g %.7g] [%.7g %.7g %.7g %.7g] [%.7g %.7g %.7g %.7g] [%.7g %.7g %.7g %.7g]",
							stage == 0 ? "VS" : "PS", slot, matrix,
							m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
							m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
					}
				}
				buffers[slot]->Release();
			}
		}
	}
	if (!originalVs || !vsUpload || vsUpload->bytesUsed < 64) {
		if (originalVs) originalVs->Release();
		return scope;
	}
	if (!EnsureRotatedConstants(context)) {
		if (originalVs) originalVs->Release();
		return scope;
	}

	unsigned char rotated[128] = {};
	std::memcpy(rotated, vsUpload->bytes, sizeof(rotated));
	// sky_pc_50_vs declares g_matSkyWsToOs in $Globals at cb0 offset 0. Compose the requested
	// orientation into that matrix while leaving PerViewConstantBuffer_t (cb1) untouched, so the
	// sky cube remains full-screen and only the cubemap lookup direction moves.
	RotateSkyMatrix(rotated, 0, SkyGpuDelta(), SkyOrient_Mirror());
	context->UpdateSubresource(s_rotatedConstants, 0, nullptr, rotated, 0, 0);
	context->VSSetConstantBuffers(0, 1, &s_rotatedConstants);
	scope.originalVsSlot0 = originalVs;
	originalVs = nullptr;
	if (originalVs) originalVs->Release();
	// This hook runs once per sky draw, not once per frame.  Keep the probe useful without
	// flushing an identical line for every draw call (mvm_debug is intentionally an evidence
	// channel, not a frame logger).
	static bool s_loggedApplied = false;
	static float s_lastYaw = 0.0f, s_lastPitch = 0.0f;
	static bool s_lastMirror = false;
	if (MvmDebugLog_Active()) {
		const float yaw = SkyOrient_Yaw();
		const float pitch = SkyOrient_Pitch();
		const bool mirror = SkyOrient_Mirror();
		if (!s_loggedApplied || yaw != s_lastYaw || pitch != s_lastPitch || mirror != s_lastMirror) {
			const SunRotationMath::Quat q = SkyGpuDelta();
			MvmDebugLog_Linef("world.skygpu.rotation",
				"applied yaw=%.3f pitch=%.3f mirror=%d gpuQuat=(%.5f %.5f %.5f %.5f) vsSource=%p vsClone=%p target=VS.cb0.g_matSkyWsToOs",
				yaw, pitch, mirror ? 1 : 0,
				q.x, q.y, q.z, q.w,
				(void*)scope.originalVsSlot0, (void*)s_rotatedConstants);
			s_lastYaw = yaw;
			s_lastPitch = pitch;
			s_lastMirror = mirror;
			s_loggedApplied = true;
		}
	} else {
		s_loggedApplied = false;
	}
	return scope;
}

void SkyGpuRotation_EndDraw(ID3D11DeviceContext* context, SkyGpuRotationScope& scope) {
	if (!context) return;
	if (scope.originalSunVertexBuffer) {
		UINT stride = scope.sunVertexStride, offset = scope.sunVertexOffset;
		context->IASetVertexBuffers(0, 1, &scope.originalSunVertexBuffer, &stride, &offset);
		scope.originalSunVertexBuffer->Release();
		scope.originalSunVertexBuffer = nullptr;
	}
	if (scope.originalVsSlot0) {
		context->VSSetConstantBuffers(0, 1, &scope.originalVsSlot0);
		scope.originalVsSlot0->Release();
		scope.originalVsSlot0 = nullptr;
	}
}

} // namespace Filmmaker
