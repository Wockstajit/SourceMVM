#define NOMINMAX
#include "WorldWarpRender.h"
#include "WorldWarp.h"
#include "WorldWarpDxbcMath.h"

#include "../Debug/GpuPassProfiler.h"
#include "../Debug/MvmDebugLog.h"
#include "../Environment/EnvironmentMath.h"
#include "../Environment/EnvironmentShaderCompiler.h"
#include "../Render/RenderPrivateTextureScope.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Filmmaker::WorldWarp {
using Microsoft::WRL::ComPtr;
namespace {

constexpr UINT kGridCols = 480;
constexpr UINT kGridRows = 270;
constexpr UINT kCubeSize = 1024;

constexpr char kShader[] = R"HLSL(
cbuffer WarpConstants : register(b0) {
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float4 dims;         // target w, h; depth w, h
    float4 depthInfo;    // minDepth, maxDepth, reversed, mode
    float4 cameraTime;   // cam xyz, demoTime
    float4 originFlags;  // origin xyz, useSceneOrigin
    float4 planetA;      // scale, spinRad, tiltRad, conformal
    float4 planetB;      // rimSoftness, cubeReady, tanHalfX, tanHalfY
    float4 horizonParams;// bend, flatRadius, curveExp, 0
    float4 waveParams;   // height, wavelength, speed, dirRad
    float4 waveExtra;    // radial, flatRadius, 0, 0
    float4 foldParams;   // angleRad, axisYawRad, offset, softness
    float4 foldExtra;    // flipSide, 0, 0, 0
};

Texture2D<float4> sceneColor : register(t0);
#if DEPTH_MS
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2DArray<float4> cubeFaces : register(t2);
SamplerState linearClamp : register(s0);

struct ScreenOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

ScreenOut ScreenVS(uint id : SV_VertexID) {
    ScreenOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}

bool Reversed() { return depthInfo.z > 0.5; }

float SampleRawDepth(float2 uv) {
    int2 p = clamp(int2(uv * dims.zw), int2(0, 0), int2(dims.zw) - 1);
#if DEPTH_MS
    return sceneDepth.Load(p, 0);
#else
    return sceneDepth.Load(int3(p, 0));
#endif
}

bool UnprojectWorld(float2 uv, out float3 world, out bool isSky) {
    world = cameraTime.xyz;
    isSky = false;
    float d = SampleRawDepth(uv);
    if (Reversed() ? d < 1e-7 : d > 0.9999999) { isSky = true; return false; }
    if (!Reversed() && d <= 0.100001) return false;
    float span = max(depthInfo.y - depthInfo.x, 1e-12);
    float n = (d - depthInfo.x) / span;
    if (n < 0.0 || n > 1.0) { isSky = true; return false; }
    float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, n, 1.0));
    if (abs(h.w) < 1e-10) { isSky = true; return false; }
    world = h.xyz / h.w;
    return true;
}

float3 DeformWorld(float3 p) {
    int mode = int(depthInfo.w + 0.5);
    float3 anchor = originFlags.w > 0.5 ? originFlags.xyz : cameraTime.xyz;
    if (mode == 2 || mode == 3) {
        // Inception & Rolling Horizon
        float d = length(p.xy - anchor.xy);
        float flat = max(0.0, horizonParams.y);
        if (d <= flat) return p;
        float excess = (d - flat) * 0.001;
        float lift = horizonParams.x * 480.0 * pow(excess, clamp(horizonParams.z, 1.1, 3.5));
        return float3(p.x, p.y, p.z + lift);
    }
    if (mode == 4) {
        // World Wave
        float2 rel = p.xy - anchor.xy;
        float2 dir = float2(cos(waveParams.w), sin(waveParams.w));
        float coord = waveExtra.x > 0.5 ? length(rel) : dot(rel, dir);
        float wl = max(waveParams.y, 32.0);
        float phase = (6.2831853 * coord / wl) - waveParams.z * cameraTime.w;
        float camD = length(p.xy - cameraTime.xy);
        float ramp = waveExtra.y > 1.0 ? smoothstep(0.0, waveExtra.y, camD) : 1.0;
        return float3(p.x, p.y, p.z + waveParams.x * sin(phase) * ramp);
    }
    if (mode == 5) {
        // World Fold
        float tx = cos(foldParams.y), ty = sin(foldParams.y);
        float sideSign = foldExtra.x > 0.5 ? -1.0 : 1.0;
        float2 n = float2(-ty * sideSign, tx * sideSign);
        float2 hinge = anchor.xy + n * foldParams.z;
        float u = dot(p.xy - hinge, n);
        if (u <= 0.0) return p;
        float blend = foldParams.w > 1e-3 ? smoothstep(0.0, foldParams.w, u) : 1.0;
        float a = foldParams.x * blend;
        float ca = cos(a), sa = sin(a);
        float dz = p.z - anchor.z;
        float uNew = u * ca - dz * sa;
        float zNew = anchor.z + u * sa + dz * ca;
        float2 xyNew = p.xy + n * (uNew - u);
        return float3(xyNew.x, xyNew.y, zNew);
    }
    return p;
}

// Samples our 6-face Source-2 cubemap (0=Down -Z, 1=Fwd +X, 2=Left +Y, 3=Back -X, 4=Right -Y, 5=Up +Z)
// using the exact projection half-tangents recorded during capture so faces stitch seamlessly.
float4 SamplePlanetCube(float3 dir) {
    float ax = abs(dir.x), ay = abs(dir.y), az = abs(dir.z);
    float tanX = max(planetB.z, 0.2);
    float tanY = max(planetB.w, 0.2);
    int face = 0;
    float u = 0.0, v = 0.0;
    if (az >= ax && az >= ay) {
        if (dir.z <= 0.0) {
            // Face 0: Down (pitch +90, yaw 0 -> camera right = -Y, camera up = +X)
            face = 0;
            u = (-dir.y / max(az, 1e-5)) / tanX;
            v = (-dir.x / max(az, 1e-5)) / tanY;
        } else {
            // Face 5: Up (pitch -90, yaw 0 -> camera right = -Y, camera up = -X)
            face = 5;
            u = (-dir.y / max(az, 1e-5)) / tanX;
            v = ( dir.x / max(az, 1e-5)) / tanY;
        }
    } else if (ax >= ay) {
        if (dir.x >= 0.0) {
            // Face 1: Forward (+X -> camera right = -Y, camera up = +Z)
            face = 1;
            u = (-dir.y / max(ax, 1e-5)) / tanX;
            v = (-dir.z / max(ax, 1e-5)) / tanY;
        } else {
            // Face 3: Back (-X -> camera right = +Y, camera up = +Z)
            face = 3;
            u = ( dir.y / max(ax, 1e-5)) / tanX;
            v = (-dir.z / max(ax, 1e-5)) / tanY;
        }
    } else {
        if (dir.y >= 0.0) {
            // Face 2: Left (+Y -> camera right = +X, camera up = +Z)
            face = 2;
            u = ( dir.x / max(ay, 1e-5)) / tanX;
            v = (-dir.z / max(ay, 1e-5)) / tanY;
        } else {
            // Face 4: Right (-Y -> camera right = -X, camera up = +Z)
            face = 4;
            u = (-dir.x / max(ay, 1e-5)) / tanX;
            v = (-dir.z / max(ay, 1e-5)) / tanY;
        }
    }
    float2 uv = float2(u * 0.5 + 0.5, v * 0.5 + 0.5);
    return cubeFaces.SampleLevel(linearClamp, float3(saturate(uv), float(face)), 0);
}

// Little Planet pixel shader: conformal stereographic projection of the 360 cubemap.
float4 LittlePlanetPS(ScreenOut i) : SV_Target {
    float aspect = dims.x / max(dims.y, 1.0);
    float2 p = (i.uv - 0.5) * 2.0;
    p.x *= aspect;
    float r = length(p);
    float scale = max(planetA.x, 0.15);
    float phi = atan2(p.y, p.x) + planetA.y;

    float thetaStereo = 2.0 * atan(r / scale);
    float thetaEqui = clamp((r / scale) * 1.5707963, 0.0, 3.14159265);
    float theta = lerp(thetaEqui, thetaStereo, saturate(planetA.w));

    float sinT = sin(theta), cosT = cos(theta);
    float3 d = float3(sinT * cos(phi), sinT * sin(phi), -cosT);
    if (abs(planetA.z) > 1e-4) {
        float ct = cos(planetA.z), st = sin(planetA.z);
        d = float3(d.x * ct + d.z * st, d.y, -d.x * st + d.z * ct);
    }
    d = normalize(d);

    if (planetB.y > 0.5) {
        float4 col = SamplePlanetCube(d);
        // Subtle atmospheric sky softness around the outer rim (matches the reference's dreamy sky rim)
        float rim = smoothstep(1.85, 2.85, theta) * saturate(planetB.x);
        if (rim > 0.01) {
            float3 s1 = SamplePlanetCube(normalize(d + float3(0.03, 0.02, 0.0))).rgb;
            float3 s2 = SamplePlanetCube(normalize(d - float3(0.03, 0.02, 0.0))).rgb;
            float3 s3 = SamplePlanetCube(normalize(d + float3(-0.02, 0.03, 0.0))).rgb;
            float3 s4 = SamplePlanetCube(normalize(d - float3(-0.02, 0.03, 0.0))).rgb;
            col.rgb = lerp(col.rgb, (col.rgb + s1 + s2 + s3 + s4) * 0.2, rim * 0.75);
        }
        return float4(col.rgb, 1.0);
    }

    // Fallback before the 6 cubemap faces complete their first capture: stereographic radial warp.
    float2 fallbackUv = float2(0.5 + (p.x / max(aspect, 1e-4)) * 0.45 / (1.0 + 0.35 * r),
                               0.5 + p.y * 0.45 / (1.0 + 0.35 * r));
    return float4(sceneColor.SampleLevel(linearClamp, saturate(fallbackUv), 0).rgb, 1.0);
}

// Background sky-fill pass run before the 3D deformed mesh so areas revealed behind folded or
// lifted buildings show natural sky/backdrop instead of black holes.
float4 BackdropPS(ScreenOut i) : SV_Target {
    return float4(sceneColor.SampleLevel(linearClamp, i.uv, 0).rgb, 1.0);
}

// 3D Depth-Displaced Mesh Reprojection for Inception, Rolling Horizon, World Wave, World Fold.
struct MeshOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float  dist  : TEXCOORD1;
    float  valid : TEXCOORD2;
};

MeshOut DeformMeshVS(uint vid : SV_VertexID) {
    const uint cols = 480;
    const uint rows = 270;
    uint quad = vid / 6;
    uint corner = vid % 6;
    uint gx = quad % cols;
    uint gy = quad / cols;
    uint ox = (corner == 1 || corner == 2 || corner == 4) ? 1 : 0;
    uint oy = (corner == 2 || corner == 4 || corner == 5) ? 1 : 0;
    float2 uv = float2(float(gx + ox) / float(cols), float(gy + oy) / float(rows));

    MeshOut o;
    o.uv = uv;
    float3 w;
    bool isSky;
    if (!UnprojectWorld(uv, w, isSky)) {
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.9999, 1.0);
        o.dist = 30000.0;
        o.valid = isSky ? 0.0 : 1.0;
        return o;
    }
    float3 wDef = DeformWorld(w);
    float4 clip = mul(viewProj, float4(wDef, 1.0));
    if (clip.w <= 0.05) {
        o.pos = float4(0, 0, 2, 1);
        o.dist = 0;
        o.valid = 0;
        return o;
    }
    // Map depth to standard [0, 1] forward depth for our local D32F Z-buffer (near = 0, far = 1).
    float camDist = length(wDef - cameraTime.xyz);
    float zNorm = saturate(camDist / 18000.0);
    o.pos = float4(clip.xy / clip.w, zNorm, 1.0);
    o.dist = camDist;
    o.valid = 1.0;
    return o;
}

float4 DeformMeshPS(MeshOut i) : SV_Target {
    if (i.valid < 0.95) discard;
    // Discard stretched rubber-band triangles spanning sharp foreground/background depth jumps.
    float grad = abs(ddx(i.dist)) + abs(ddy(i.dist));
    if (grad > max(32.0, i.dist * 0.14)) discard;
    return float4(sceneColor.SampleLevel(linearClamp, i.uv, 0).rgb, 1.0);
}
)HLSL";

struct Constants {
	float viewProj[16];
	float invViewProj[16];
	float dims[4];
	float depthInfo[4];
	float cameraTime[4];
	float originFlags[4];
	float planetA[4];
	float planetB[4];
	float horizonParams[4];
	float waveParams[4];
	float waveExtra[4];
	float foldParams[4];
	float foldExtra[4];
};
static_assert(sizeof(Constants) % 16 == 0, "cb alignment");

struct VsWarpConstants {
	float modeTime[4];       // x=mode, y=demoTime, z=useOrigin, w=activeFlag
	float originPos[4];      // xyz=origin
	float horizonParams[4];  // x=bend, y=flatRadius, z=curve
	float waveParams[4];     // x=height, y=wavelength, z=speed, w=dirRad
	float waveExtra[4];      // x=radial, y=flatRadius
	float foldParams[4];     // x=angleRad, y=axisYawRad, z=offset, w=softness
	float foldExtra[4];      // x=flipSide
};
static_assert(sizeof(VsWarpConstants) == WorldWarpDxbc::kWarpConstantRows * 16, "cb13 size");

// Standalone SM5 vertex-shader snippet compiled once and spliced into CS2's 3D world vertex shaders
// right before `ret` via WorldWarpDxbc::PatchVertexShaderDxbc.
constexpr char kWarpSnippetHlsl[] = R"HLSL(
cbuffer NativeView : register(b12) {
    column_major float4x4 g_worldToProj : packoffset(c0);
    column_major float4x4 g_projToWorld : packoffset(c4);
};
cbuffer WorldWarpCB : register(b13) {
    float4 modeTime;
    float4 originPos;
    float4 horizonParams;
    float4 waveParams;
    float4 waveExtra;
    float4 foldParams;
    float4 foldExtra;
};
float4 WarpSnippetVS(float4 clipIn : POSITION) : SV_Position {
    float4 outPos = clipIn;
    float ndcZ = clipIn.z / max(abs(clipIn.w), 1e-5);
    bool isSky = (clipIn.w > 1.0) && ((originPos.w > 0.5) ? (ndcZ < 1e-5) : (ndcZ > 0.9999));
    if (modeTime.w > 0.5 && !isSky) {
        float4 wp4 = mul(g_projToWorld, clipIn);
        if (abs(wp4.w) > 1e-5) {
            float3 p = wp4.xyz / wp4.w;
            float4 eye4 = mul(g_projToWorld, float4(0.0, 0.0, 1.0, 0.0));
            float3 cam = (abs(eye4.w) > 1e-5) ? (eye4.xyz / eye4.w) : p;
            float camD = length(p - cam);
            if (camD < 8000.0) {
                float3 anchor = (modeTime.z > 0.5) ? originPos.xyz : cam;
                int mode = int(modeTime.x + 0.5);
                float3 pDef = p;
                if (mode == 2 || mode == 3) {
                    float d = length(p.xy - anchor.xy);
                    float flat = max(0.0, horizonParams.y);
                    if (d > flat) {
                        float excess = (d - flat) / 1000.0;
                        pDef.z += horizonParams.x * 480.0 * pow(max(excess, 0.0), clamp(horizonParams.z, 1.1, 3.5));
                    }
                } else if (mode == 4) {
                    float2 rel = p.xy - anchor.xy;
                    float2 dir = float2(cos(waveParams.w), sin(waveParams.w));
                    float coord = (waveExtra.x > 0.5) ? length(rel) : dot(rel, dir);
                    float wl = max(waveParams.y, 32.0);
                    float phase = (6.2831853 * coord / wl) - waveParams.z * modeTime.y;
                    float camXY = length(p.xy - cam.xy);
                    float ramp = (waveExtra.y > 1.0) ? smoothstep(0.0, waveExtra.y, camXY) : 1.0;
                    pDef.z += waveParams.x * sin(phase) * ramp;
                } else if (mode == 5) {
                    float tx = cos(foldParams.y), ty = sin(foldParams.y);
                    float sideSign = (foldExtra.x > 0.5) ? -1.0 : 1.0;
                    float2 n = float2(-ty * sideSign, tx * sideSign);
                    float2 hinge = anchor.xy + n * foldParams.z;
                    float u = dot(p.xy - hinge, n);
                    if (u > 0.0) {
                        float blend = (foldParams.w > 1e-3) ? smoothstep(0.0, foldParams.w, u) : 1.0;
                        float a = foldParams.x * blend;
                        float ca = cos(a), sa = sin(a);
                        float dz = p.z - anchor.z;
                        float uNew = u * ca - dz * sa;
                        float zNew = anchor.z + u * sa + dz * ca;
                        pDef = float3(p.xy + n * (uNew - u), zNew);
                    }
                }
                float3 delta = pDef - p;
                outPos = clipIn + wp4.w * mul(g_worldToProj, float4(delta, 0.0));
            }
        }
    }
    return outPos;
}
)HLSL";

// Custom GUID for attaching TrackedVs metadata to CS2's ID3D11VertexShader objects.
constexpr GUID kWarpVsGuid = { 0x9b4f21a3, 0x7e48, 0x4c19, { 0xa2, 0x6f, 0x51, 0x0d, 0x88, 0x3b, 0x19, 0xe4 } };

struct TrackedVs final : public IUnknown {
	std::atomic<ULONG> refs{ 1 };
	std::mutex mx;
	std::vector<uint8_t> bytecode;
	unsigned perViewSlot = 0;
	ComPtr<ID3D11VertexShader> patched;
	bool tried = false;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
		if (!ppv) return E_POINTER;
		if (riid == __uuidof(IUnknown)) {
			*ppv = static_cast<IUnknown*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG r = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (r == 0) delete this;
		return r;
	}
};

thread_local bool g_creatingWarpShader = false;
std::atomic<uint32_t> g_vsTrackedCount{ 0 };
std::atomic<uint32_t> g_vsPatchedCount{ 0 };
std::atomic<uint32_t> g_vsDrawsThisFrame{ 0 };
std::atomic<uint32_t> g_vsDrawsLastFrame{ 0 };
std::atomic<UINT> g_sceneW{ 0 };
std::atomic<UINT> g_sceneH{ 0 };
std::atomic<bool> g_reversedZ{ false };
std::atomic<uint32_t> g_uploadedGen{ 0 };
std::atomic<double> g_uploadedDemoTime{ -1.0 };

bool HasBytes(const void* buf, size_t len, const char* needle) {
	const char* first = static_cast<const char*>(buf);
	const size_t nlen = std::strlen(needle);
	return buf && len >= nlen && std::search(first, first + len, needle, needle + nlen) != first + len;
}

struct Blobs {
	ComPtr<ID3DBlob> screenVS, planetPS, backdropPS[2], meshVS[2], meshPS;
	WorldWarpDxbc::WarpSnippet warpSnippet;
	bool ok = false;
};
std::mutex g_blobLock;
std::shared_ptr<const Blobs> g_blobs;
std::atomic<bool> g_compileStarted{ false };
std::thread g_compiler;

struct Gpu {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11VertexShader> screenVS, meshVS[2];
	ComPtr<ID3D11PixelShader> planetPS, backdropPS[2], meshPS;
	ComPtr<ID3D11Buffer> cb, vsWarpCb;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11RasterizerState> raster;
	ComPtr<ID3D11DepthStencilState> dsOff, dsTestWrite;
	ComPtr<ID3D11BlendState> blendOpaque;

	ComPtr<ID3D11Texture2D> colorCopy, depthCopy, meshZ;
	ComPtr<ID3D11ShaderResourceView> colorSrv, depthSrv;
	ComPtr<ID3D11DepthStencilView> meshZView;
	UINT colorW = 0, colorH = 0;
	DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
	D3D11_TEXTURE2D_DESC depthDesc{};

	ComPtr<ID3D11Texture2D> cubeArray;
	ComPtr<ID3D11ShaderResourceView> cubeSrv;
	UINT cubeW = 0, cubeH = 0;
	DXGI_FORMAT cubeFmt = DXGI_FORMAT_UNKNOWN;
	// The six faces are captured into cubeArray and copied as a whole into cubeShow once all six
	// share one demo time (WorldWarp_ConsumeCapturedFace), so the planet never mixes two moments.
	ComPtr<ID3D11Texture2D> cubeShow;
	ComPtr<ID3D11ShaderResourceView> cubeShowSrv;
	bool cubeShown = false;
	float tanHalfX = 1.0f, tanHalfY = 1.0f;
	bool failed = false;
} g_gpu;

std::mutex g_stateCacheLock;
std::unordered_map<ID3D11DepthStencilState*, ComPtr<ID3D11DepthStencilState>> g_dssRelaxedCache[4];

ID3D11DepthStencilState* GetRelaxedDepthStencil(ID3D11DepthStencilState* orig, const D3D11_DEPTH_STENCIL_DESC& desc, bool reversedZ, bool forceWrite) {
	if (!g_gpu.device || !orig) return nullptr;
	const int idx = (reversedZ ? 1 : 0) | (forceWrite ? 2 : 0);
	std::lock_guard<std::mutex> lk(g_stateCacheLock);
	auto& cache = g_dssRelaxedCache[idx];
	auto it = cache.find(orig);
	if (it != cache.end()) return it->second.Get();
	D3D11_DEPTH_STENCIL_DESC mod = desc;
	mod.DepthFunc = reversedZ ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
	if (forceWrite) mod.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	ComPtr<ID3D11DepthStencilState> created;
	{
		RenderPrivateTextureScope priv;
		g_gpu.device->CreateDepthStencilState(&mod, &created);
	}
	cache.emplace(orig, created);
	return created.Get();
}

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT DepthTypelessFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

void CompileAll() {
	auto b = std::make_shared<Blobs>();
	ComPtr<ID3DBlob> err;
	const D3D_SHADER_MACRO s0[] = { { "DEPTH_MS", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO s1[] = { { "DEPTH_MS", "1" }, { nullptr, nullptr } };
	const size_t n = sizeof(kShader) - 1;
	b->ok = SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s0, "ScreenVS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->screenVS, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s0, "LittlePlanetPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->planetPS, &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s0, "BackdropPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->backdropPS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s1, "BackdropPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->backdropPS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s0, "DeformMeshVS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->meshVS[0], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s1, "DeformMeshVS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->meshVS[1], &err))
		&& SUCCEEDED(Environment::CompileEnvironmentShader(kShader, n, "WorldWarp", s0, "DeformMeshPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &b->meshPS, &err));
	if (!b->ok)
		MvmDebugLog_LinefAlways("worldwarp", "shader compile failed: %s",
			err ? static_cast<const char*>(err->GetBufferPointer()) : "compiler unavailable");
	ComPtr<ID3DBlob> snipBlob, snipErr;
	if (SUCCEEDED(Environment::CompileEnvironmentShader(kWarpSnippetHlsl, sizeof(kWarpSnippetHlsl) - 1,
		"WorldWarpSnippet", nullptr, "WarpSnippetVS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, &snipBlob, &snipErr))
		&& snipBlob) {
		if (!WorldWarpDxbc::ExtractWarpSnippet(snipBlob->GetBufferPointer(), snipBlob->GetBufferSize(), b->warpSnippet))
			MvmDebugLog_LinefAlways("worldwarp", "DXBC warp snippet extraction failed");
	} else {
		MvmDebugLog_LinefAlways("worldwarp", "DXBC warp snippet compile failed: %s",
			snipErr ? static_cast<const char*>(snipErr->GetBufferPointer()) : "compiler unavailable");
	}
	std::lock_guard<std::mutex> lock(g_blobLock);
	g_blobs = b;
}

bool EnsureShaders() {
	if (g_gpu.screenVS) return true;
	if (!g_gpu.device || g_gpu.failed) return false;
	std::shared_ptr<const Blobs> b;
	{ std::lock_guard<std::mutex> lock(g_blobLock); b = g_blobs; }
	if (!b) return false;
	if (!b->ok) { g_gpu.failed = true; return false; }

	RenderPrivateTextureScope priv;
	auto* d = g_gpu.device.Get();
	const bool ok = SUCCEEDED(d->CreateVertexShader(b->screenVS->GetBufferPointer(), b->screenVS->GetBufferSize(), nullptr, &g_gpu.screenVS))
		&& SUCCEEDED(d->CreatePixelShader(b->planetPS->GetBufferPointer(), b->planetPS->GetBufferSize(), nullptr, &g_gpu.planetPS))
		&& SUCCEEDED(d->CreatePixelShader(b->backdropPS[0]->GetBufferPointer(), b->backdropPS[0]->GetBufferSize(), nullptr, &g_gpu.backdropPS[0]))
		&& SUCCEEDED(d->CreatePixelShader(b->backdropPS[1]->GetBufferPointer(), b->backdropPS[1]->GetBufferSize(), nullptr, &g_gpu.backdropPS[1]))
		&& SUCCEEDED(d->CreateVertexShader(b->meshVS[0]->GetBufferPointer(), b->meshVS[0]->GetBufferSize(), nullptr, &g_gpu.meshVS[0]))
		&& SUCCEEDED(d->CreateVertexShader(b->meshVS[1]->GetBufferPointer(), b->meshVS[1]->GetBufferSize(), nullptr, &g_gpu.meshVS[1]))
		&& SUCCEEDED(d->CreatePixelShader(b->meshPS->GetBufferPointer(), b->meshPS->GetBufferSize(), nullptr, &g_gpu.meshPS));
	if (!ok) { g_gpu.failed = true; return false; }

	D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(Constants); cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d->CreateBuffer(&cbd, nullptr, &g_gpu.cb);
	D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	d->CreateSamplerState(&sd, &g_gpu.sampler);
	D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	d->CreateRasterizerState(&rd, &g_gpu.raster);
	D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	d->CreateDepthStencilState(&dd, &g_gpu.dsOff);
	dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	d->CreateDepthStencilState(&dd, &g_gpu.dsTestWrite);
	D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	d->CreateBlendState(&bd, &g_gpu.blendOpaque);
	return g_gpu.cb && g_gpu.sampler && g_gpu.raster && g_gpu.dsOff && g_gpu.dsTestWrite && g_gpu.blendOpaque;
}

bool EnsureColorCopy(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, DXGI_FORMAT viewFmt) {
	D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
	const DXGI_FORMAT typed = TypedColorFormat(viewFmt != DXGI_FORMAT_UNKNOWN ? viewFmt : d.Format);
	if (!g_gpu.colorCopy || g_gpu.colorW != d.Width || g_gpu.colorH != d.Height || g_gpu.colorFmt != typed) {
		RenderPrivateTextureScope priv;
		g_gpu.colorCopy.Reset(); g_gpu.colorSrv.Reset(); g_gpu.meshZ.Reset(); g_gpu.meshZView.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.MipLevels = 1; td.ArraySize = 1; td.Format = typed; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
		td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.colorCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.colorCopy.Get(), nullptr, &g_gpu.colorSrv)))
			return false;
		D3D11_TEXTURE2D_DESC zd = td;
		zd.Format = DXGI_FORMAT_D32_FLOAT; zd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (FAILED(g_gpu.device->CreateTexture2D(&zd, nullptr, &g_gpu.meshZ))
			|| FAILED(g_gpu.device->CreateDepthStencilView(g_gpu.meshZ.Get(), nullptr, &g_gpu.meshZView)))
			return false;
		g_gpu.colorW = d.Width; g_gpu.colorH = d.Height; g_gpu.colorFmt = typed;
	}
	if (d.SampleDesc.Count > 1) ctx->ResolveSubresource(g_gpu.colorCopy.Get(), 0, src, 0, typed);
	else ctx->CopyResource(g_gpu.colorCopy.Get(), src);
	return true;
}

bool EnsureDepthCopy(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, bool& isMsaa, UINT& w, UINT& h) {
	if (!dsv) return false;
	ComPtr<ID3D11Resource> res; dsv->GetResource(&res);
	ComPtr<ID3D11Texture2D> tex; if (!res || FAILED(res.As(&tex))) return false;
	D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
	const DXGI_FORMAT typeless = DepthTypelessFormat(d.Format);
	const DXGI_FORMAT srvFmt = DepthSrvFormat(d.Format);
	if (typeless == DXGI_FORMAT_UNKNOWN || srvFmt == DXGI_FORMAT_UNKNOWN) return false;
	isMsaa = d.SampleDesc.Count > 1;
	w = d.Width; h = d.Height;
	if (!g_gpu.depthCopy || g_gpu.depthDesc.Width != d.Width || g_gpu.depthDesc.Height != d.Height
		|| g_gpu.depthDesc.Format != typeless || g_gpu.depthDesc.SampleDesc.Count != d.SampleDesc.Count) {
		RenderPrivateTextureScope priv;
		g_gpu.depthCopy.Reset(); g_gpu.depthSrv.Reset();
		D3D11_TEXTURE2D_DESC td = d;
		td.Format = typeless; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.MiscFlags = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = srvFmt;
		sd.ViewDimension = isMsaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
		if (!isMsaa) sd.Texture2D.MipLevels = 1;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.depthCopy))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.depthCopy.Get(), &sd, &g_gpu.depthSrv)))
			return false;
		g_gpu.depthDesc = td;
	}
	ctx->CopyResource(g_gpu.depthCopy.Get(), tex.Get());
	return true;
}

void UploadVsWarpConstants(ID3D11DeviceContext* ctx, double demoTime) {
	if (!ctx || !g_gpu.vsWarpCb) return;
	const Settings s = WorldWarp_Get();
	VsWarpConstants vcb{};
	vcb.modeTime[0] = (float)(int)s.mode;
	vcb.modeTime[1] = (float)demoTime;
	vcb.modeTime[2] = s.useSceneOrigin ? 1.0f : 0.0f;
	vcb.modeTime[3] = WorldWarp_WantsVertexWarp() ? 1.0f : 0.0f;
	vcb.originPos[0] = s.origin[0]; vcb.originPos[1] = s.origin[1]; vcb.originPos[2] = s.origin[2];
	vcb.originPos[3] = g_reversedZ.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
	vcb.horizonParams[0] = s.horizonBend; vcb.horizonParams[1] = s.horizonFlatRadius; vcb.horizonParams[2] = s.horizonCurve;
	vcb.waveParams[0] = s.waveHeight; vcb.waveParams[1] = s.waveLength;
	vcb.waveParams[2] = s.waveSpeed; vcb.waveParams[3] = s.waveDirection * kDegToRad;
	vcb.waveExtra[0] = s.waveRadial ? 1.0f : 0.0f; vcb.waveExtra[1] = s.waveFlatRadius;
	vcb.foldParams[0] = s.foldAngle * kDegToRad; vcb.foldParams[1] = s.foldAxisYaw * kDegToRad;
	vcb.foldParams[2] = s.foldOffset; vcb.foldParams[3] = s.foldSoftness;
	vcb.foldExtra[0] = s.foldFlipSide ? 1.0f : 0.0f;
	ctx->UpdateSubresource(g_gpu.vsWarpCb.Get(), 0, nullptr, &vcb, 0, 0);
	g_uploadedGen.store(WorldWarp_SettingsGeneration(), std::memory_order_release);
	g_uploadedDemoTime.store(demoTime, std::memory_order_release);
}

} // namespace

bool CreatingShader() {
	return g_creatingWarpShader;
}

void NoteVertexShader(ID3D11VertexShader* shader, const void* bytecode, size_t bytes) {
	if (!shader || !bytecode || bytes < 64 || g_creatingWarpShader)
		return;
	// Sub-microsecond reject for 2D UI, post-process, and non-world shaders.
	if (!HasBytes(bytecode, bytes, "PerViewConstantBuffer_t") || !HasBytes(bytecode, bytes, "g_matWorldToProjection"))
		return;
	static HMODULE compiler = Environment::EnvironmentShaderCompiler();
	using ReflectFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
	auto reflect = compiler ? reinterpret_cast<ReflectFn>(GetProcAddress(compiler, "D3DReflect")) : nullptr;
	if (!reflect) return;

	ComPtr<ID3D11ShaderReflection> r;
	if (FAILED(reflect(bytecode, bytes, __uuidof(ID3D11ShaderReflection), &r)) || !r)
		return;
	const char* cbName = "PerViewConstantBuffer_t";
	auto* cb = r->GetConstantBufferByName(cbName);
	D3D11_SHADER_INPUT_BIND_DESC bind{};
	if (!cb || FAILED(r->GetResourceBindingDescByName(cbName, &bind))
		|| bind.Type != D3D_SIT_CBUFFER || bind.BindPoint >= WorldWarpDxbc::kWarpConstantSlot)
		return;
	D3D11_SHADER_VARIABLE_DESC w2p{};
	if (FAILED(cb->GetVariableByName("g_matWorldToProjection")->GetDesc(&w2p))
		|| w2p.StartOffset != 0 || w2p.Size != 64)
		return;
	D3D11_SHADER_VARIABLE_DESC p2w{};
	if (SUCCEEDED(cb->GetVariableByName("g_matProjectionToWorld")->GetDesc(&p2w))
		&& (p2w.StartOffset != 64 || p2w.Size != 64))
		return;

	auto* tracker = new TrackedVs();
	tracker->perViewSlot = bind.BindPoint;
	tracker->bytecode.assign(static_cast<const uint8_t*>(bytecode), static_cast<const uint8_t*>(bytecode) + bytes);
	if (SUCCEEDED(shader->SetPrivateDataInterface(kWarpVsGuid, tracker)))
		g_vsTrackedCount.fetch_add(1, std::memory_order_relaxed);
	tracker->Release();
}

void UpdateFrameConstants(ID3D11DeviceContext* immediate, double demoTime, UINT sceneW, UINT sceneH) {
	if (sceneW >= 256 && sceneH >= 256) {
		g_sceneW.store(sceneW, std::memory_order_relaxed);
		g_sceneH.store(sceneH, std::memory_order_relaxed);
	}
	if (WorldWarp_WantsVertexWarp() && immediate && g_gpu.vsWarpCb) {
		UploadVsWarpConstants(immediate, demoTime);
	}
}

void GetVertexWarpStats(uint32_t& outTracked, uint32_t& outPatched, uint32_t& outLastFrameDraws) {
	outTracked = g_vsTrackedCount.load(std::memory_order_relaxed);
	outPatched = g_vsPatchedCount.load(std::memory_order_relaxed);
	outLastFrameDraws = g_vsDrawsLastFrame.load(std::memory_order_relaxed);
}

void DrawScope::Enter(ID3D11DeviceContext* ctx) noexcept {
	if (!ctx || !g_gpu.device || !g_gpu.vsWarpCb) return;

	// Only warp main-camera 3D passes (skip viewmodel MaxDepth == 0.1, CSM shadow atlases,
	// downsampled post-process passes, and 2D UI with no DSV).
	D3D11_VIEWPORT vp{};
	UINT vpCount = 1;
	ctx->RSGetViewports(&vpCount, &vp);
	if (vpCount == 0 || vp.Width < 256.0f || vp.Height < 256.0f || vp.MaxDepth < 0.5f)
		return;
	const UINT sw = g_sceneW.load(std::memory_order_relaxed);
	const UINT sh = g_sceneH.load(std::memory_order_relaxed);
	if (sw >= 256 && sh >= 256 && ((UINT)vp.Width != sw || (UINT)vp.Height != sh))
		return;

	ComPtr<ID3D11RenderTargetView> rtv0;
	ComPtr<ID3D11DepthStencilView> dsv;
	ctx->OMGetRenderTargets(1, &rtv0, &dsv);
	if (!dsv) return;
	const bool hasRtv = rtv0 != nullptr;

	ID3D11VertexShader* origVs = nullptr;
	ctx->VSGetShader(&origVs, nullptr, nullptr);
	if (!origVs) return;

	IUnknown* unk = nullptr;
	UINT unkSize = sizeof(unk);
	if (FAILED(origVs->GetPrivateData(kWarpVsGuid, &unkSize, &unk)) || !unk) {
		origVs->Release();
		return;
	}
	auto* tracker = static_cast<TrackedVs*>(unk);

	ID3D11VertexShader* patchedVs = nullptr;
	{
		std::lock_guard<std::mutex> lk(tracker->mx);
		if (!tracker->tried) {
			std::shared_ptr<const Blobs> b;
			{ std::lock_guard<std::mutex> bl(g_blobLock); b = g_blobs; }
			if (b && b->warpSnippet.valid) {
				tracker->tried = true;
				std::vector<uint8_t> patchedDxbc;
				if (WorldWarpDxbc::PatchVertexShaderDxbc(
					tracker->bytecode.data(), tracker->bytecode.size(),
					tracker->perViewSlot, b->warpSnippet, patchedDxbc)) {
					g_creatingWarpShader = true;
					const HRESULT hr = g_gpu.device->CreateVertexShader(
						patchedDxbc.data(), patchedDxbc.size(), nullptr, &tracker->patched);
					g_creatingWarpShader = false;
					if (SUCCEEDED(hr) && tracker->patched)
						g_vsPatchedCount.fetch_add(1, std::memory_order_relaxed);
				}
				std::vector<uint8_t>().swap(tracker->bytecode);
			}
		}
		patchedVs = tracker->patched.Get();
	}
	tracker->Release();

	if (!patchedVs) {
		origVs->Release();
		return;
	}

	ID3D11DepthStencilState* origDss = nullptr;
	UINT origStencilRef = 0;
	bool swappedDss = false;
	ctx->OMGetDepthStencilState(&origDss, &origStencilRef);
	D3D11_DEPTH_STENCIL_DESC dsd{};
	if (origDss) {
		origDss->GetDesc(&dsd);
		if (dsd.DepthEnable && dsd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) {
			if (dsd.DepthFunc == D3D11_COMPARISON_LESS || dsd.DepthFunc == D3D11_COMPARISON_LESS_EQUAL)
				g_reversedZ.store(false, std::memory_order_relaxed);
			else if (dsd.DepthFunc == D3D11_COMPARISON_GREATER || dsd.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL)
				g_reversedZ.store(true, std::memory_order_relaxed);
		}
	}

	// CS2's Depth-Only Pre-Pass (rtv=0, DepthWrite=ALL) writes depth that can diverge from
	// Forward Color Pass shaders under non-linear 3D vertex warping, causing solid black areas
	// where the color pass fails Early-Z. Bypass tracked depth-only pre-pass draws while vertex
	// warping is active and let opaque Forward Color Pass draws write their own exact warped
	// depth directly into the Z-buffer in a single pass.
	if (!hasRtv && origDss && dsd.DepthEnable && dsd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL
		&& dsd.DepthFunc != D3D11_COMPARISON_NEVER) {
		origDss->Release();
		origVs->Release();
		m_skip = true;
		return;
	}

	const bool revZ = g_reversedZ.load(std::memory_order_relaxed);
	const bool isDepthEq = origDss && dsd.DepthEnable && dsd.DepthFunc == D3D11_COMPARISON_EQUAL;
	bool promoteDepthWrite = false;
	if (hasRtv && origDss && dsd.DepthEnable && dsd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO) {
		ComPtr<ID3D11PixelShader> ps;
		ctx->PSGetShader(&ps, nullptr, nullptr);
		if (ps) {
			ComPtr<ID3D11BlendState> bs;
			float bf[4] = {}; UINT sm = 0;
			ctx->OMGetBlendState(&bs, bf, &sm);
			if (!bs) {
				promoteDepthWrite = true;
			} else {
				D3D11_BLEND_DESC bd{};
				bs->GetDesc(&bd);
				promoteDepthWrite = !bd.RenderTarget[0].BlendEnable
					&& ((bd.RenderTarget[0].RenderTargetWriteMask & 0x7u) != 0);
			}
		}
	}

	if ((promoteDepthWrite || isDepthEq) && origDss) {
		if (auto* relaxed = GetRelaxedDepthStencil(origDss, dsd, revZ, promoteDepthWrite)) {
			ctx->OMSetDepthStencilState(relaxed, origStencilRef);
			swappedDss = true;
		}
	}
	if (!swappedDss && origDss) {
		origDss->Release();
		origDss = nullptr;
	}

	// Ensure cb13 holds the latest settings even on the first frame after a preset change.
	if (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
		&& g_uploadedGen.load(std::memory_order_acquire) != WorldWarp_SettingsGeneration()) {
		UploadVsWarpConstants(ctx, g_uploadedDemoTime.load(std::memory_order_relaxed));
	}

	ID3D11Buffer* origCb13 = nullptr;
	ctx->VSGetConstantBuffers(WorldWarpDxbc::kWarpConstantSlot, 1, &origCb13);
	ID3D11Buffer* warpCb = g_gpu.vsWarpCb.Get();
	ctx->VSSetShader(patchedVs, nullptr, 0);
	ctx->VSSetConstantBuffers(WorldWarpDxbc::kWarpConstantSlot, 1, &warpCb);

	m_ctx = ctx;
	m_origVs = origVs;
	m_origCb13 = origCb13;
	m_origDss = origDss;
	m_origStencilRef = origStencilRef;
	m_swappedDss = swappedDss;
	m_active = true;
	g_vsDrawsThisFrame.fetch_add(1, std::memory_order_relaxed);
}

void DrawScope::Exit() noexcept {
	m_ctx->VSSetShader(m_origVs, nullptr, 0);
	m_ctx->VSSetConstantBuffers(WorldWarpDxbc::kWarpConstantSlot, 1, &m_origCb13);
	if (m_swappedDss)
		m_ctx->OMSetDepthStencilState(m_origDss, m_origStencilRef);
	if (m_origVs) m_origVs->Release();
	if (m_origCb13) m_origCb13->Release();
	if (m_origDss) m_origDss->Release();
	m_ctx = nullptr;
	m_origVs = nullptr;
	m_origCb13 = nullptr;
	m_origDss = nullptr;
	m_origStencilRef = 0;
	m_swappedDss = false;
	m_active = false;
}

void BeginDevice(ID3D11Device* device) {
	EndDevice();
	if (!device) return;
	g_gpu.device = device;
	g_gpu.failed = false;
	device->CreateDeferredContext(0, &g_gpu.ctx);
	{
		RenderPrivateTextureScope priv;
		D3D11_BUFFER_DESC cbd{};
		cbd.ByteWidth = sizeof(VsWarpConstants);
		cbd.Usage = D3D11_USAGE_DEFAULT;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		device->CreateBuffer(&cbd, nullptr, &g_gpu.vsWarpCb);
	}
	if (!g_compileStarted.exchange(true)) {
		g_compiler = std::thread(CompileAll);
		g_compiler.detach();
	}
}

void EndDevice() {
	{
		std::lock_guard<std::mutex> lk(g_stateCacheLock);
		for (int i = 0; i < 4; ++i)
			g_dssRelaxedCache[i].clear();
	}
	g_gpu = {};
	g_uploadedGen.store(0, std::memory_order_relaxed);
}

void PublishCubemap(ID3D11DeviceContext* immediate) {
	if (!immediate || !g_gpu.cubeArray || !g_gpu.cubeShow) return;
	immediate->CopyResource(g_gpu.cubeShow.Get(), g_gpu.cubeArray.Get());
	g_gpu.cubeShown = true;
}

void CaptureCubemapFace(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	int faceIndex, const float* projectionMatrix) {
	if (!immediate || !target || !g_gpu.device || faceIndex < 0 || faceIndex >= 6) return;
	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return;
	D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);
	const DXGI_FORMAT typed = TypedColorFormat(rtvd.Format != DXGI_FORMAT_UNKNOWN ? rtvd.Format : d.Format);
	if (!g_gpu.cubeArray || g_gpu.cubeW != d.Width || g_gpu.cubeH != d.Height || g_gpu.cubeFmt != typed) {
		RenderPrivateTextureScope priv;
		g_gpu.cubeArray.Reset(); g_gpu.cubeSrv.Reset();
		g_gpu.cubeShow.Reset(); g_gpu.cubeShowSrv.Reset(); g_gpu.cubeShown = false;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 6;
		td.Format = typed; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = typed; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = 6;
		if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.cubeArray))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.cubeArray.Get(), &sd, &g_gpu.cubeSrv))
			|| FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &g_gpu.cubeShow))
			|| FAILED(g_gpu.device->CreateShaderResourceView(g_gpu.cubeShow.Get(), &sd, &g_gpu.cubeShowSrv))) {
			g_gpu.cubeArray.Reset(); g_gpu.cubeSrv.Reset(); g_gpu.cubeShow.Reset(); g_gpu.cubeShowSrv.Reset();
			return;
		}
		g_gpu.cubeW = d.Width; g_gpu.cubeH = d.Height; g_gpu.cubeFmt = typed;
	}
	if (d.SampleDesc.Count > 1) {
		if (!EnsureColorCopy(immediate, src.Get(), typed)) return;
		immediate->CopySubresourceRegion(g_gpu.cubeArray.Get(), faceIndex, 0, 0, 0, g_gpu.colorCopy.Get(), 0, nullptr);
	} else {
		immediate->CopySubresourceRegion(g_gpu.cubeArray.Get(), faceIndex, 0, 0, 0, src.Get(), 0, nullptr);
	}
	if (projectionMatrix) {
		const float p00 = std::fabs(projectionMatrix[0]);
		const float p11 = std::fabs(projectionMatrix[5]);
		if (p00 > 0.05f && p11 > 0.05f) {
			g_gpu.tanHalfX = 1.0f / p00;
			g_gpu.tanHalfY = 1.0f / p11;
		}
	}
}

bool Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target,
	ID3D11DepthStencilView* depth, const float* worldToClip, const float* projection,
	const float camera[3], float depthMin, float depthMax, double demoTime) {
	const Settings s = WorldWarp_Get();
	if (s.mode == Mode::Off || !immediate || !target)
		return false;

	ComPtr<ID3D11Resource> res; target->GetResource(&res);
	ComPtr<ID3D11Texture2D> src; if (!res || FAILED(res.As(&src))) return false;
	D3D11_TEXTURE2D_DESC srcDesc{}; src->GetDesc(&srcDesc);
	UpdateFrameConstants(immediate, demoTime, srcDesc.Width, srcDesc.Height);

	// For Inception / Rolling Horizon / World Wave / World Fold: if CS2's 3D world vertex shaders
	// were already deformed natively in DrawScope this frame, skip the 2D post-process mesh so
	// there is zero pixel stretching and zero disocclusion curtains.
	if (s.mode != Mode::LittlePlanet) {
		const uint32_t draws = g_vsDrawsThisFrame.exchange(0, std::memory_order_acq_rel);
		g_vsDrawsLastFrame.store(draws, std::memory_order_relaxed);
		if (draws > 0)
			return true;
	}

	if (!g_gpu.ctx || !EnsureShaders())
		return false;

	D3D11_RENDER_TARGET_VIEW_DESC rtvd{}; target->GetDesc(&rtvd);
	if (!EnsureColorCopy(g_gpu.ctx.Get(), src.Get(), rtvd.Format)) return false;

	bool isMsaa = false;
	UINT dw = g_gpu.colorW, dh = g_gpu.colorH;
	const bool needDepth = (s.mode != Mode::LittlePlanet);
	if (needDepth && !EnsureDepthCopy(g_gpu.ctx.Get(), depth, isMsaa, dw, dh)) {
		ComPtr<ID3D11CommandList> drop; g_gpu.ctx->FinishCommandList(FALSE, &drop);
		return false;
	}

	Environment::Matrix vp{}, inv{};
	bool reversed = false;
	if (worldToClip) {
		std::memcpy(vp.data(), worldToClip, sizeof(float) * 16);
		Environment::Inverse(vp, inv);
	}
	if (projection) reversed = projection[10] < 0.0f;

	const bool cubeReady = g_gpu.cubeShown;

	Constants cb{};
	std::memcpy(cb.viewProj, vp.data(), sizeof(float) * 16);
	std::memcpy(cb.invViewProj, inv.data(), sizeof(float) * 16);
	cb.dims[0] = (float)g_gpu.colorW; cb.dims[1] = (float)g_gpu.colorH;
	cb.dims[2] = (float)dw; cb.dims[3] = (float)dh;
	cb.depthInfo[0] = depthMin; cb.depthInfo[1] = depthMax;
	cb.depthInfo[2] = reversed ? 1.0f : 0.0f; cb.depthInfo[3] = (float)(int)s.mode;
	cb.cameraTime[0] = camera ? camera[0] : 0.0f;
	cb.cameraTime[1] = camera ? camera[1] : 0.0f;
	cb.cameraTime[2] = camera ? camera[2] : 0.0f;
	cb.cameraTime[3] = (float)demoTime;
	cb.originFlags[0] = s.origin[0]; cb.originFlags[1] = s.origin[1]; cb.originFlags[2] = s.origin[2];
	cb.originFlags[3] = s.useSceneOrigin ? 1.0f : 0.0f;
	cb.planetA[0] = s.planetScale; cb.planetA[1] = s.planetSpin * kDegToRad;
	cb.planetA[2] = s.planetTilt * kDegToRad; cb.planetA[3] = s.planetConformal;
	cb.planetB[0] = s.planetRimSoftness; cb.planetB[1] = (cubeReady && g_gpu.cubeShowSrv) ? 1.0f : 0.0f;
	cb.planetB[2] = g_gpu.tanHalfX; cb.planetB[3] = g_gpu.tanHalfY;
	cb.horizonParams[0] = s.horizonBend; cb.horizonParams[1] = s.horizonFlatRadius; cb.horizonParams[2] = s.horizonCurve;
	cb.waveParams[0] = s.waveHeight; cb.waveParams[1] = s.waveLength; cb.waveParams[2] = s.waveSpeed; cb.waveParams[3] = s.waveDirection * kDegToRad;
	cb.waveExtra[0] = s.waveRadial ? 1.0f : 0.0f; cb.waveExtra[1] = s.waveFlatRadius;
	cb.foldParams[0] = s.foldAngle * kDegToRad; cb.foldParams[1] = s.foldAxisYaw * kDegToRad;
	cb.foldParams[2] = s.foldOffset; cb.foldParams[3] = s.foldSoftness;
	cb.foldExtra[0] = s.foldFlipSide ? 1.0f : 0.0f;

	auto* c = g_gpu.ctx.Get();
	c->UpdateSubresource(g_gpu.cb.Get(), 0, nullptr, &cb, 0, 0);

	D3D11_VIEWPORT vpRect{};
	vpRect.Width = (float)g_gpu.colorW; vpRect.Height = (float)g_gpu.colorH; vpRect.MaxDepth = 1.0f;
	c->RSSetViewports(1, &vpRect);
	c->RSSetState(g_gpu.raster.Get());
	const float bf[4] = {};
	c->OMSetBlendState(g_gpu.blendOpaque.Get(), bf, 0xffffffff);
	c->IASetInputLayout(nullptr);
	c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D11Buffer* cbs[1] = { g_gpu.cb.Get() };
	ID3D11SamplerState* smps[1] = { g_gpu.sampler.Get() };
	ID3D11ShaderResourceView* srvs[3] = {
		g_gpu.colorSrv.Get(),
		needDepth ? g_gpu.depthSrv.Get() : nullptr,
		(cubeReady && g_gpu.cubeShowSrv) ? g_gpu.cubeShowSrv.Get() : nullptr
	};

	if (s.mode == Mode::LittlePlanet) {
		ID3D11RenderTargetView* rts[1] = { target };
		c->OMSetRenderTargets(1, rts, nullptr);
		c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
		c->VSSetShader(g_gpu.screenVS.Get(), nullptr, 0);
		c->PSSetShader(g_gpu.planetPS.Get(), nullptr, 0);
		c->PSSetConstantBuffers(0, 1, cbs);
		c->PSSetSamplers(0, 1, smps);
		c->PSSetShaderResources(0, 3, srvs);
		c->Draw(3, 0);
	} else {
		const int msIdx = isMsaa ? 1 : 0;
		// Pass 1: clean sky/backdrop fill so disoccluded edges behind folded/lifted buildings show sky.
		ID3D11RenderTargetView* rts[1] = { target };
		c->OMSetRenderTargets(1, rts, nullptr);
		c->OMSetDepthStencilState(g_gpu.dsOff.Get(), 0);
		c->VSSetShader(g_gpu.screenVS.Get(), nullptr, 0);
		c->PSSetShader(g_gpu.backdropPS[msIdx].Get(), nullptr, 0);
		c->PSSetConstantBuffers(0, 1, cbs);
		c->PSSetSamplers(0, 1, smps);
		c->PSSetShaderResources(0, 2, srvs);
		c->Draw(3, 0);

		// Pass 2: Z-buffered 3D depth-reprojected deformed world mesh.
		c->ClearDepthStencilView(g_gpu.meshZView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		c->OMSetRenderTargets(1, rts, g_gpu.meshZView.Get());
		c->OMSetDepthStencilState(g_gpu.dsTestWrite.Get(), 0);
		c->VSSetShader(g_gpu.meshVS[msIdx].Get(), nullptr, 0);
		c->VSSetConstantBuffers(0, 1, cbs);
		c->VSSetShaderResources(0, 2, srvs);
		c->PSSetShader(g_gpu.meshPS.Get(), nullptr, 0);
		c->PSSetConstantBuffers(0, 1, cbs);
		c->PSSetSamplers(0, 1, smps);
		c->PSSetShaderResources(0, 2, srvs);
		c->Draw(kGridCols * kGridRows * 6, 0);
	}

	ID3D11ShaderResourceView* nullSrvs[3] = {};
	c->VSSetShaderResources(0, 2, nullSrvs);
	c->PSSetShaderResources(0, 3, nullSrvs);
	ID3D11RenderTargetView* nullRt[1] = {};
	c->OMSetRenderTargets(1, nullRt, nullptr);

	ComPtr<ID3D11CommandList> cl;
	if (FAILED(c->FinishCommandList(FALSE, &cl)) || !cl) return false;
	{ GPU_PASS_SCOPE(immediate, "world warp"); immediate->ExecuteCommandList(cl.Get(), TRUE); }
	return true;
}

} // namespace Filmmaker::WorldWarp
