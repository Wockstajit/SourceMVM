#include "RenderHdrCapture.h"

#include "../Platform/ExrWriter.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace Filmmaker {

namespace {

std::mutex s_mutex;

// Several HDR-format MSAA targets exist (scene colour, reflection/utility buffers at other
// sizes). All candidates are kept (bounded, newest first) and the grab picks the one whose
// dimensions match the currently BOUND render target -- that is the scene-sized one.
struct Candidate {
	ID3D11Texture2D* tex; // AddRef'd
	D3D11_TEXTURE2D_DESC desc;
};
constexpr size_t kMaxCandidates = 8;
Candidate s_candidates[kMaxCandidates] = {};
size_t s_candidateCount = 0;

std::atomic<bool> s_grabPending{ false };
std::wstring s_grabPath; // guarded by s_mutex

// EXR sequence mode (armed by the render job driver). Written on the render thread only.
std::atomic<bool> s_seqActive{ false };
std::wstring s_seqDir;                    // guarded by s_mutex
std::atomic<unsigned> s_seqWritten{ 0 };  // render thread writes; engine thread reads at end
std::atomic<unsigned> s_seqFailed{ 0 };

// Cached resolve/staging pair so a 90-frame job does 2 allocations, not 180. Render thread.
ID3D11Texture2D* s_cacheResolved = nullptr;
ID3D11Texture2D* s_cacheStaging = nullptr;
UINT s_cacheW = 0, s_cacheH = 0;
ID3D11RenderTargetView* s_cacheRtv = nullptr;
HdrEnvironmentPass s_environmentPass = nullptr;

void ReleaseCache() {
	if (s_cacheRtv) { s_cacheRtv->Release(); s_cacheRtv = nullptr; }
	if (s_cacheResolved) { s_cacheResolved->Release(); s_cacheResolved = nullptr; }
	if (s_cacheStaging) { s_cacheStaging->Release(); s_cacheStaging = nullptr; }
	s_cacheW = s_cacheH = 0;
}

bool IsHdrColorFormat(DXGI_FORMAT f) {
	return f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

float HalfToFloat(uint16_t h) {
	const uint32_t sign = (uint32_t)(h >> 15) << 31;
	uint32_t exp = (h >> 10) & 0x1F;
	uint32_t man = h & 0x3FF;
	uint32_t bits;
	if (exp == 0) {
		if (man == 0) {
			bits = sign;
		} else { // subnormal
			exp = 127 - 15 + 1;
			while (0 == (man & 0x400)) { man <<= 1; --exp; }
			man &= 0x3FF;
			bits = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 0x1F) {
		bits = sign | 0x7F800000u | (man << 13);
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
	}
	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

} // namespace

void RenderHdrCapture_SetEnvironmentPass(HdrEnvironmentPass pass) { s_environmentPass = pass; }

void RenderHdrCapture_OnCreateTexture2D(const D3D11_TEXTURE2D_DESC& desc, ID3D11Texture2D* tex) {
	if (!tex) return;
	if (!IsHdrColorFormat(desc.Format)) return;
	if (desc.SampleDesc.Count < 2) return;
	if (0 == (desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return;
	if (desc.Width < 640 || desc.Height < 480) return;

	std::lock_guard<std::mutex> lock(s_mutex);
	// Newest-first insert; drop the oldest when full. Same-size replacements (mode changes
	// recreate the target) naturally end up at the front.
	if (s_candidateCount == kMaxCandidates) {
		s_candidates[kMaxCandidates - 1].tex->Release();
		--s_candidateCount;
	}
	for (size_t i = s_candidateCount; i > 0; --i)
		s_candidates[i] = s_candidates[i - 1];
	s_candidates[0].tex = tex;
	s_candidates[0].tex->AddRef();
	s_candidates[0].desc = desc;
	++s_candidateCount;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.hdr", "tracking HDR target candidate %ux%u fmt=%d samples=%u (n=%zu)",
			desc.Width, desc.Height, (int)desc.Format, desc.SampleDesc.Count, s_candidateCount);
}

void RenderHdrCapture_RequestGrab(const wchar_t* path) {
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_grabPath = path ? path : L"";
	}
	s_grabPending.store(true);
}

namespace {

// Resolve the tracked scene-sized HDR target, map it, and write `path`. Uses the cached
// resolve/staging pair (recreated on size change). Render thread only. With `logStats` the
// HDR proof numbers (min/max/mean, fraction > 1.0) go to console + mvm_debug.
// The scene-sized HDR candidate: the one matching the currently BOUND render target's dimensions
// (the LDR target the recorder captures is scene-sized by definition). AddRef'd, or null.
ID3D11Texture2D* SelectSceneTarget(ID3D11DeviceContext* ctx, D3D11_TEXTURE2D_DESC* outDesc, size_t* outCandidates,
	UINT* outWantW, UINT* outWantH) {
	UINT wantW = 0, wantH = 0;
	{
		ID3D11RenderTargetView* rtv = nullptr;
		ctx->OMGetRenderTargets(1, &rtv, nullptr);
		if (rtv) {
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			if (res) {
				ID3D11Texture2D* t = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
					D3D11_TEXTURE2D_DESC d = {};
					t->GetDesc(&d);
					wantW = d.Width;
					wantH = d.Height;
					t->Release();
				}
				res->Release();
			}
			rtv->Release();
		}
	}

	ID3D11Texture2D* src = nullptr;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (outCandidates) *outCandidates = s_candidateCount;
	if (outWantW) *outWantW = wantW;
	if (outWantH) *outWantH = wantH;
	for (size_t i = 0; i < s_candidateCount; ++i) {
		if (wantW && (s_candidates[i].desc.Width != wantW || s_candidates[i].desc.Height != wantH))
			continue;
		src = s_candidates[i].tex;
		src->AddRef();
		if (outDesc) *outDesc = s_candidates[i].desc;
		break;
	}
	return src;
}

bool CaptureHdrToExr(ID3D11DeviceContext* ctx, const wchar_t* path, bool logStats) {
	D3D11_TEXTURE2D_DESC desc = {};
	size_t nCandidates = 0;
	UINT wantW = 0, wantH = 0;
	ID3D11Texture2D* src = SelectSceneTarget(ctx, &desc, &nCandidates, &wantW, &wantH);
	if (!src) {
		if (logStats)
			advancedfx::Warning("render hdrgrab: no scene-sized HDR target among %zu tracked "
				"candidate(s) (bound target is %ux%u).\n", nCandidates, wantW, wantH);
		return false;
	}

	ID3D11Device* dev = nullptr;
	ctx->GetDevice(&dev);
	bool ok = false;
	do {
		if (!dev) break;

		if (s_cacheW != desc.Width || s_cacheH != desc.Height
			|| !s_cacheResolved || !s_cacheStaging) {
			ReleaseCache();
			D3D11_TEXTURE2D_DESC rd = desc;
			rd.SampleDesc.Count = 1;
			rd.SampleDesc.Quality = 0;
			rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			rd.BindFlags = D3D11_BIND_RENDER_TARGET;
			rd.MipLevels = 1;
			rd.MiscFlags = 0;
			rd.Usage = D3D11_USAGE_DEFAULT;
			rd.CPUAccessFlags = 0;
			if (FAILED(dev->CreateTexture2D(&rd, nullptr, &s_cacheResolved))) break;
			if (FAILED(dev->CreateRenderTargetView(s_cacheResolved, nullptr, &s_cacheRtv))) { ReleaseCache(); break; }
			D3D11_TEXTURE2D_DESC sd = rd;
			sd.BindFlags = 0;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_cacheStaging))) {
				ReleaseCache();
				break;
			}
			s_cacheW = desc.Width;
			s_cacheH = desc.Height;
		}

		ctx->ResolveSubresource(s_cacheResolved, 0, src, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
		if (s_environmentPass) s_environmentPass(ctx, s_cacheRtv, src);
		ctx->CopyResource(s_cacheStaging, s_cacheResolved);

		D3D11_MAPPED_SUBRESOURCE map = {};
		if (FAILED(ctx->Map(s_cacheStaging, 0, D3D11_MAP_READ, 0, &map))) break;

		if (logStats) {
			double sum = 0.0, mx = 0.0, mn = 1e30;
			unsigned long long over1 = 0, count = 0;
			for (UINT y = 0; y < desc.Height; ++y) {
				const uint16_t* row = (const uint16_t*)((const uint8_t*)map.pData + (size_t)y * map.RowPitch);
				for (UINT x = 0; x < desc.Width; ++x) {
					for (int c = 0; c < 3; ++c) {
						const float v = HalfToFloat(row[x * 4 + c]);
						if (!std::isfinite(v)) continue;
						sum += v;
						if (v > mx) mx = v;
						if (v < mn) mn = v;
						if (v > 1.0f) ++over1;
						++count;
					}
				}
			}
			ok = ExrWriteHalfRgba(path, (const uint16_t*)map.pData,
				(int)desc.Width, (int)desc.Height, (int)map.RowPitch);
			advancedfx::Message("render hdrgrab: %s %ux%u srcFmt=%d | min=%.4f max=%.4f mean=%.4f "
				">1.0: %.2f%% of samples%s\n",
				ok ? "wrote" : "FAILED writing", desc.Width, desc.Height, (int)desc.Format,
				mn, mx, count ? sum / (double)count : 0.0,
				count ? 100.0 * (double)over1 / (double)count : 0.0,
				ok ? "" : " (file I/O error)");
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("render.hdr",
					"grab ok=%d min=%.4f max=%.4f over1=%.2f%%", ok ? 1 : 0, mn, mx,
					count ? 100.0 * (double)over1 / (double)count : 0.0);
		} else {
			ok = ExrWriteHalfRgba(path, (const uint16_t*)map.pData,
				(int)desc.Width, (int)desc.Height, (int)map.RowPitch);
		}
		ctx->Unmap(s_cacheStaging, 0);
	} while (false);

	if (dev) dev->Release();
	src->Release();
	return ok;
}

} // namespace

ID3D11Texture2D* RenderHdrCapture_AcquireSceneTarget(ID3D11DeviceContext* ctx, D3D11_TEXTURE2D_DESC* outDesc) {
	return ctx ? SelectSceneTarget(ctx, outDesc, nullptr, nullptr, nullptr) : nullptr;
}

void RenderHdrCapture_RenderThread_Tick(ID3D11DeviceContext* ctx) {
	if (!s_grabPending.load(std::memory_order_relaxed) || !ctx)
		return;
	s_grabPending.store(false);
	std::wstring path;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		path = s_grabPath;
	}
	CaptureHdrToExr(ctx, path.c_str(), true);
}

void RenderHdrCapture_BeginSequence(const wchar_t* dir) {
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_seqDir = dir ? dir : L"";
	}
	s_seqWritten = 0;
	s_seqFailed = 0;
	s_seqActive.store(!s_seqDir.empty());
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.hdr", "EXR sequence armed");
}

void RenderHdrCapture_OnRecordedFrameBeforeUi(ID3D11DeviceContext* ctx) {
	if (!s_seqActive.load(std::memory_order_relaxed) || !ctx)
		return;
	std::wstring dir;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		dir = s_seqDir;
	}
	wchar_t path[MAX_PATH * 2];
	swprintf_s(path, L"%s\\frame_%06u.exr", dir.c_str(),
		s_seqWritten.load() + s_seqFailed.load() + 1);
	if (CaptureHdrToExr(ctx, path, false))
		s_seqWritten.fetch_add(1);
	else
		s_seqFailed.fetch_add(1);
}

void RenderHdrCapture_EndSequence(unsigned* outWritten, unsigned* outFailed) {
	s_seqActive.store(false);
	if (outWritten) *outWritten = s_seqWritten.load();
	if (outFailed) *outFailed = s_seqFailed.load();
	// The resolve/staging cache is deliberately kept: it is recreated on size change, and
	// releasing it here (engine thread) could race a render-thread capture still in flight.
}

bool RenderHdrCapture_SequenceActive() { return s_seqActive.load(); }

void RenderHdrCapture_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	std::wstring path;
	if (argc >= 4) {
		// Console strings are UTF-8.
		const char* p = args->ArgV(3);
		int n = ::MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
		if (n > 0) {
			path.resize((size_t)n - 1);
			::MultiByteToWideChar(CP_UTF8, 0, p, -1, path.data(), n);
		}
	}
	if (path.empty()) {
		wchar_t tmp[MAX_PATH] = {};
		::GetTempPathW(MAX_PATH, tmp);
		path = std::wstring(tmp) + L"mvm_hdrgrab.exr";
	}
	RenderHdrCapture_RequestGrab(path.c_str());
	advancedfx::Message("%s: HDR grab queued for the next frame.\n", cmd);
}

} // namespace Filmmaker
