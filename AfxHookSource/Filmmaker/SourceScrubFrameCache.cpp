#include "stdafx.h"

#include "SourceScrubFrameCache.h"
#include "SourceCameraPath.h"
#include "SourceSettings.h"
#include "SourceTimelinePlayhead.h"

#include <AfxHookSource2/Filmmaker/Movie/CamPathShake.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <AfxHookSource2/Filmmaker/Movie/DemoScrubHost.h>
#include <AfxHookSource2/Filmmaker/Movie/ScrubFrameCacheHost.h>

#include <mutex>
#include <vector>

namespace SourceFilmmaker {

namespace {

using Filmmaker::ScrubCache::Index;

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// Between the main thread (lookups, drops, what each frame shows) and the render thread (keeping and
// showing frames). The textures themselves are the render thread's alone.
struct Shared {
	std::mutex mutex;
	Index index;
	bool enabled = true;
	int budgetMb = Filmmaker::ScrubCache::kDefaultBudgetMb;
	bool reset = true; // the textures go and the index is sized again at the next present
	Filmmaker::ScrubFramePublish latest;
	bool hold = false;
	unsigned width = 0, height = 0;
	unsigned long long stores = 0, served = 0, serveMissing = 0, evictions = 0;
};
Shared g;

// Render thread only.
std::vector<IDirect3DTexture9*> g_slots;
IDirect3DTexture9* g_held = nullptr; // the last frame the preview showed settled, for a hold
bool g_haveHeld = false;
UINT g_slotW = 0, g_slotH = 0;
D3DFORMAT g_slotFormat = D3DFMT_UNKNOWN;

void Release(IDirect3DTexture9*& t) {
	if (t) t->Release();
	t = nullptr;
}

void ReleaseAll() {
	for (IDirect3DTexture9*& t : g_slots) Release(t);
	g_slots.clear();
	Release(g_held);
	g_haveHeld = false;
}

bool Ensure(IDirect3DDevice9* device, IDirect3DTexture9*& t) {
	return t || SUCCEEDED(device->CreateTexture(g_slotW, g_slotH, 1, D3DUSAGE_RENDERTARGET, g_slotFormat, D3DPOOL_DEFAULT, &t, nullptr));
}

bool Copy(IDirect3DDevice9* device, IDirect3DTexture9* from, IDirect3DTexture9* to) {
	IDirect3DSurface9 *src = nullptr, *dst = nullptr;
	bool ok = SUCCEEDED(from->GetSurfaceLevel(0, &src)) && SUCCEEDED(to->GetSurfaceLevel(0, &dst))
		&& SUCCEEDED(device->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_LINEAR));
	if (src) src->Release();
	if (dst) dst->Release();
	return ok;
}

} // namespace

IDirect3DTexture9* ScrubFrames_PreviewTexture(IDirect3DDevice9* device, IDirect3DTexture9* frame, const Ui::Rect& preview) {
	if (!device || !frame || !preview.Valid()) return frame;
	D3DSURFACE_DESC desc;
	if (FAILED(frame->GetLevelDesc(0, &desc))) return frame;
	const UINT w = (UINT)(preview.W() + 0.5f), h = (UINT)(preview.H() + 0.5f);

	bool enabled, hold, reset;
	int budgetMb;
	Filmmaker::ScrubFramePublish published;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		enabled = g.enabled;
		hold = g.hold;
		reset = g.reset;
		budgetMb = g.budgetMb;
		published = g.latest;
		g.reset = false;
	}
	// A new size or format (the preview, or the game's video mode) or a new budget: the frames start over.
	if (reset || w != g_slotW || h != g_slotH || desc.Format != g_slotFormat) {
		ReleaseAll();
		g_slotW = w;
		g_slotH = h;
		g_slotFormat = desc.Format;
		const int capacity = enabled ? Filmmaker::ScrubCache::CapacityForBudget((uint64_t)budgetMb << 20, w, h, 4) : 0;
		g_slots.assign((size_t)capacity, nullptr);
		std::lock_guard<std::mutex> lock(g.mutex);
		g.index.Reset(capacity);
		g.width = w;
		g.height = h;
	}

	// The scrub shows a kept frame instead of the world.
	if (enabled && published.serve) {
		int slot;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			slot = g.index.Find(published.serveTick, published.id.revision);
			if (slot >= 0 && g_slots[(size_t)slot]) ++g.served;
			else ++g.serveMissing;
		}
		if (slot >= 0 && g_slots[(size_t)slot]) {
			// What the preview shows is what a hold falls back on.
			if (Ensure(device, g_held) && Copy(device, g_slots[(size_t)slot], g_held)) g_haveHeld = true;
			return g_slots[(size_t)slot];
		}
	}
	// A restore and its play-in are in flight: the last settled frame, never the raw restore.
	if (hold && g_haveHeld) return g_held;

	// A picture of its own tick is kept.
	if (enabled && Filmmaker::ScrubCache::Cacheable(published.id)) {
		Index::Store store;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			store = g.index.PlanStore(published.id, published.nearTick);
		}
		if (store.slot >= 0 && (size_t)store.slot < g_slots.size() && Ensure(device, g_slots[(size_t)store.slot])
			&& Copy(device, frame, g_slots[(size_t)store.slot])) {
			std::lock_guard<std::mutex> lock(g.mutex);
			g.index.Commit(store.slot, published.id);
			++g.stores;
			if (store.evicted) ++g.evictions;
		}
	}
	if (Ensure(device, g_held) && Copy(device, frame, g_held)) g_haveHeld = true;
	return frame;
}

void ScrubFrames_DeviceLost() {
	ReleaseAll();
	g_slotW = g_slotH = 0;
	std::lock_guard<std::mutex> lock(g.mutex);
	g.reset = true;
}

} // namespace SourceFilmmaker

// --- What the shared cache and scrub ask CS:S (ScrubFrameCacheHost.h, DemoScrubHost.h) ---------------

namespace Filmmaker {

using SourceFilmmaker::g;

bool ScrubCacheHost_Enabled() { return SourceFilmmaker::Settings_ScrubCache(); }

void ScrubCacheHost_SetEnabled(bool enabled) { SourceFilmmaker::Settings_SetScrubCache(enabled); }

uint64_t ScrubCacheHost_PictureRevision() {
	uint64_t h = 0x84222325cbf29ce4ull;
	h = SourceFilmmaker::Mix(h, SourceFilmmaker::Settings_PictureRevision());
	h = SourceFilmmaker::Mix(h, SourceFilmmaker::CameraPath_Revision());
	// The per-key shake is a view-stage layer the pose check cannot see (it compares the path).
	h = SourceFilmmaker::Mix(h, CamPathShake_Revision());
	return h ? h : 1;
}

bool ScrubCacheHost_PathPose(double tick, double out[7]) { return SourceFilmmaker::CameraPath_EvalPoseAtTick(tick, out); }

bool ScrubCacheHost_PathFirstTick(double& tick) { return SourceFilmmaker::CameraPath_FirstTick(tick); }

bool ScrubCacheHost_PathScrubbing() { return SourceFilmmaker::TimelinePlayhead_Scrubbing(); }

void ScrubCacheHost_Camera(double out[7]) {
	double o[3], a[3], fov = 90.0;
	CameraBridge_GetCurrentCamera(o, a, fov);
	out[0] = o[0]; out[1] = o[1]; out[2] = o[2]; out[3] = a[0]; out[4] = a[1]; out[5] = a[2]; out[6] = fov;
}

void ScrubFrames_Publish(const ScrubFramePublish& frame) {
	std::lock_guard<std::mutex> lock(g.mutex);
	g.latest = frame;
}

void ScrubFrames_Configure(bool enabled, int budgetMb) {
	std::lock_guard<std::mutex> lock(g.mutex);
	if (enabled == g.enabled && budgetMb == g.budgetMb) return;
	g.enabled = enabled;
	g.budgetMb = budgetMb;
	g.reset = true;
}

int ScrubFrames_Find(double tick, uint64_t revision, ScrubCache::Index::Entry* out) {
	std::lock_guard<std::mutex> lock(g.mutex);
	const int slot = g.index.Find(tick, revision);
	if (slot >= 0 && out) *out = g.index.At(slot);
	return slot;
}

int ScrubFrames_DropOtherRevisions(uint64_t revision) {
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.index.DropOtherRevisions(revision);
}

int ScrubFrames_DropSlot(int slot) {
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.index.DropSlot(slot);
}

int ScrubFrames_Clear() {
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.index.Clear();
}

void ScrubFrames_GetStats(ScrubFrameStats& out) {
	std::lock_guard<std::mutex> lock(g.mutex);
	out.enabled = g.enabled;
	out.frames = g.index.Count();
	out.capacity = g.index.Capacity();
	out.width = g.width;
	out.height = g.height;
	out.bytes = (unsigned long long)out.frames * g.width * g.height * 4;
	out.stores = g.stores;
	out.served = g.served;
	out.serveMissing = g.serveMissing;
	out.evictions = g.evictions;
	out.poseMismatches = 0;
}

void ScrubHost_HoldPreview(bool hold) {
	std::lock_guard<std::mutex> lock(g.mutex);
	g.hold = hold;
}

} // namespace Filmmaker
