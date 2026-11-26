// Validates WorldWarpDxbcMath against real D3DCompile + D3DDisassemble + D3D11 CreateVertexShader
// (WARP software device) on mock CS2 3D world, depth-only, and non-zero SV_Position vertex shaders.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "../../AfxHookSource2/Filmmaker/WorldWarp/WorldWarpDxbcMath.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace Filmmaker::WorldWarpDxbc;

namespace {

constexpr char kWarpSnippetHlsl[] = R"HLSL(
cbuffer NativeView : register(b12) {
    column_major float4x4 g_worldToProj : packoffset(c0);
    column_major float4x4 g_projToWorld : packoffset(c4);
};
cbuffer WorldWarpCB : register(b13) {
    float4 modeTime;       // x=mode, y=demoTime, z=useOrigin, w=activeFlag
    float4 originPos;      // xyz=origin
    float4 horizonParams;  // x=bend, y=flatRadius, z=curve
    float4 waveParams;     // x=height, y=wavelength, z=speed, w=dirRad
    float4 waveExtra;      // x=radial, y=flatRadius
    float4 foldParams;     // x=angleRad, y=axisYawRad, z=offset, w=softness
    float4 foldExtra;      // x=flipSide
};
float4 WarpSnippetVS(float4 clipIn : POSITION) : SV_Position {
    float4 outPos = clipIn;
    if (modeTime.w > 0.5 && clipIn.w > 0.05 && clipIn.z > 1e-5) {
        float4 wp4 = mul(g_projToWorld, clipIn);
        if (abs(wp4.w) > 1e-5) {
            float3 p = wp4.xyz / wp4.w;
            float4 eye4 = mul(g_projToWorld, float4(0.0, 0.0, 1.0, 0.0));
            float3 cam = (abs(eye4.w) > 1e-5) ? (eye4.xyz / eye4.w) : p;
            float camD = length(p - cam);
            if (camD < 11000.0) {
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
                outPos = mul(g_worldToProj, float4(pDef, 1.0));
            }
        }
    }
    return outPos;
}
)HLSL";

ComPtr<ID3DBlob> CompileVs(const char* src, const char* entry) {
	ComPtr<ID3DBlob> blob, err;
	HRESULT hr = D3DCompile(src, std::strlen(src), entry, nullptr, nullptr, entry, "vs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &err);
	if (FAILED(hr) || !blob) {
		std::fprintf(stderr, "CompileVs(%s) failed: %s\n", entry,
			err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown");
		std::exit(1);
	}
	return blob;
}

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		std::exit(1);
	}
}

void VerifyDisasmAndCreate(ID3D11Device* dev, const std::vector<uint8_t>& dxbc, const char* label, const char* expectSubstr) {
	ComPtr<ID3DBlob> dis;
	HRESULT hr = D3DDisassemble(dxbc.data(), dxbc.size(), 0, nullptr, &dis);
	Check(SUCCEEDED(hr) && dis, label);
	std::string text(static_cast<const char*>(dis->GetBufferPointer()), dis->GetBufferSize());
	if (expectSubstr && text.find(expectSubstr) == std::string::npos) {
		std::fprintf(stderr, "FAIL(%s): missing '%s' in disasm:\n%s\n", label, expectSubstr, text.c_str());
		std::exit(1);
	}
	ComPtr<ID3D11VertexShader> vs;
	hr = dev->CreateVertexShader(dxbc.data(), dxbc.size(), nullptr, &vs);
	if (FAILED(hr) || !vs) {
		std::fprintf(stderr, "FAIL(%s): CreateVertexShader hr=0x%08X\ndisasm:\n%s\n", label, (unsigned)hr, text.c_str());
		std::exit(1);
	}
}

} // namespace

int main() {
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1,
		D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	Check(SUCCEEDED(hr) && dev, "create WARP D3D11 device");

	// 1. Compile and extract WarpSnippetVS
	ComPtr<ID3DBlob> snipBlob = CompileVs(kWarpSnippetHlsl, "WarpSnippetVS");
	WarpSnippet snip;
	Check(ExtractWarpSnippet(snipBlob->GetBufferPointer(), snipBlob->GetBufferSize(), snip), "ExtractWarpSnippet");
	Check(snip.valid && snip.tempCount >= 2 && !snip.body.empty(), "snippet metadata valid");

	// 2. Patch a full mock CS2 world VS (PerView at b0, outputs o0=SV_Position, o1=TEXCOORD0, o2=NORMAL)
	constexpr char kWorldVs[] = R"HLSL(
cbuffer PerViewConstantBuffer_t : register(b0) {
    column_major float4x4 g_matWorldToProjection : packoffset(c0);
    column_major float4x4 g_matProjectionToWorld : packoffset(c4);
    float4 g_vWorldToCameraOffset : packoffset(c24);
};
struct In { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 nrm : NORMAL; };
Out Main(In i) {
    Out o;
    o.pos = mul(g_matWorldToProjection, float4(i.pos + g_vWorldToCameraOffset.xyz, 1.0));
    o.uv = i.uv;
    o.nrm = i.nrm;
    return o;
}
)HLSL";
	ComPtr<ID3DBlob> worldBlob = CompileVs(kWorldVs, "Main");
	std::vector<uint8_t> patchedWorld;
	Check(PatchVertexShaderDxbc(worldBlob->GetBufferPointer(), worldBlob->GetBufferSize(), 0, snip, patchedWorld),
		"PatchVertexShaderDxbc world VS");
	VerifyDisasmAndCreate(dev.Get(), patchedWorld, "world VS", "CB13[7]");

	// 3. Patch a minimal CS2 depth-only VS whose cb0 only declared 4 rows (must bump cb0[4] -> cb0[8]!)
	constexpr char kDepthOnlyVs[] = R"HLSL(
cbuffer PerViewConstantBuffer_t : register(b1) {
    column_major float4x4 g_matWorldToProjection : packoffset(c0);
};
float4 Main(float3 pos : POSITION) : SV_Position {
    return mul(g_matWorldToProjection, float4(pos, 1.0));
}
)HLSL";
	ComPtr<ID3DBlob> depthBlob = CompileVs(kDepthOnlyVs, "Main");
	std::vector<uint8_t> patchedDepth;
	Check(PatchVertexShaderDxbc(depthBlob->GetBufferPointer(), depthBlob->GetBufferSize(), 1, snip, patchedDepth),
		"PatchVertexShaderDxbc depth-only VS");
	VerifyDisasmAndCreate(dev.Get(), patchedDepth, "depth-only VS", "CB1[8]");

	// 4. Patch a VS where SV_Position is in o2 (non-zero output register)
	constexpr char kNonZeroPosVs[] = R"HLSL(
cbuffer PerViewConstantBuffer_t : register(b0) {
    column_major float4x4 g_matWorldToProjection : packoffset(c0);
};
struct Out { float4 c0 : TEXCOORD0; float4 c1 : TEXCOORD1; float4 pos : SV_Position; };
Out Main(float3 pos : POSITION, float4 c0 : TEXCOORD0, float4 c1 : TEXCOORD1) {
    Out o;
    o.c0 = c0;
    o.c1 = c1;
    o.pos = mul(g_matWorldToProjection, float4(pos, 1.0));
    return o;
}
)HLSL";
	ComPtr<ID3DBlob> nzBlob = CompileVs(kNonZeroPosVs, "Main");
	std::vector<uint8_t> patchedNz;
	Check(PatchVertexShaderDxbc(nzBlob->GetBufferPointer(), nzBlob->GetBufferSize(), 0, snip, patchedNz),
		"PatchVertexShaderDxbc non-zero SV_Position VS");
	VerifyDisasmAndCreate(dev.Get(), patchedNz, "non-zero SV_Position VS", "o2.xyzw");

	// 5. Reject a VS that already uses b13
	constexpr char kOccupiedB13Vs[] = R"HLSL(
cbuffer PerViewConstantBuffer_t : register(b0) { column_major float4x4 g_matWorldToProjection : packoffset(c0); };
cbuffer Extra : register(b13) { float4 k; };
float4 Main(float3 pos : POSITION) : SV_Position { return mul(g_matWorldToProjection, float4(pos + k.xyz, 1.0)); }
)HLSL";
	ComPtr<ID3DBlob> b13Blob = CompileVs(kOccupiedB13Vs, "Main");
	std::vector<uint8_t> dummy;
	Check(!PatchVertexShaderDxbc(b13Blob->GetBufferPointer(), b13Blob->GetBufferSize(), 0, snip, dummy),
		"reject VS with occupied b13");

	std::printf("WorldWarpDxbcTests: ALL PASSED (WARP CreateVertexShader verified on world, depth-only cb bump, non-zero SV_Position, b13 guard).\n");
	return 0;
}
