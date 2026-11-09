// SOURCE:MVM ReShade addon.
//
// Two jobs. RENDERED EFFECTS: ReShade on its own draws its effects on the backbuffer inside Present,
// after the recorder has already copied its frame out of the scene target, so nothing the user sets
// up in the overlay reached a render (user report 2026-11-08). MvmReShadeRenderEffects runs the
// effects on the scene target at the hook's before-UI boundary instead, so the recorder, the scaled
// preview and the live view all get them, and the HUD and our own panels are no longer graded.
// ReShade then skips its own Present pass for that frame: effects run once per present.
//
// OVERLAY STATE: tell AfxHookSource2 whether the ReShade overlay is open, from inside ReShade itself.
// The `reshade_open_overlay` addon event fires exactly on open and on close (a LEVEL signal,
// never blocked here -- the callback always returns false so it cannot alter ReShade's
// behaviour). The per-frame `reshade_overlay` draw event was tried first and measured WRONG for
// this purpose: it also fires for ReShade's always-on widgets (FPS counter), reading as
// "overlay open" during normal play.
//
// WHY AN ADDON. ReShade exports no is-the-overlay-open query, and mirroring the overlay toggle
// key from the game's WndProc was measured unreliable (double/missed toggles desynced the
// mirror, 2026-08-31). This is the ground truth; AfxHookSource2's UiInputGate reads the
// exported flag via GetProcAddress and stands its director input taps down while it is set.
//
// Standalone on purpose: builds with build.ps1 in this folder, ships as
// mvm_reshade_state.addon64 beside ReShade (game/bin/win64), and depends on nothing else in
// this repo.

#include <Windows.h>

#include "include/reshade.hpp"

using namespace reshade::api;

static volatile LONG g_overlayOpen = 0;

// CS2 makes two swapchains and ReShade attaches a runtime to each. Which one owns the overlay and
// presents is not predictable, so the effects run on whichever presented last. The lock keeps a
// runtime from being destroyed (window resize, quit) while the render thread is inside it.
static SRWLOCK g_lock = SRWLOCK_INIT;
static effect_runtime* g_runtime = nullptr;

// The hook's linear depth copy, bound to the effects' DEPTH texture. Kept between frames because
// ReShade keeps the binding: this view has to outlive every frame that may still read it.
static resource g_depthResource = { 0 };
static resource_view g_depthView = { 0 };

static void ReleaseDepthView(device* dev) {
	if (g_depthView.handle != 0) dev->destroy_resource_view(g_depthView);
	g_depthView = { 0 };
	g_depthResource = { 0 };
}

static void OnInitRuntime(effect_runtime* runtime) {
	AcquireSRWLockExclusive(&g_lock);
	if (g_runtime == nullptr) g_runtime = runtime;
	ReleaseSRWLockExclusive(&g_lock);
}

static void OnDestroyRuntime(effect_runtime* runtime) {
	AcquireSRWLockExclusive(&g_lock);
	if (g_runtime == runtime) {
		runtime->update_texture_bindings("DEPTH", resource_view{ 0 }, resource_view{ 0 });
		ReleaseDepthView(runtime->get_device());
		g_runtime = nullptr;
	}
	ReleaseSRWLockExclusive(&g_lock);
}

static void OnPresent(effect_runtime* runtime) {
	if (g_runtime == runtime) return; // the normal frame: no lock
	AcquireSRWLockExclusive(&g_lock);
	g_runtime = runtime;
	ReleaseSRWLockExclusive(&g_lock);
}

static bool OnReShadeOpenOverlay(reshade::api::effect_runtime*, bool open, reshade::api::input_source) {
	InterlockedExchange(&g_overlayOpen, open ? 1 : 0);
	return false; // never veto the state change
}

extern "C" __declspec(dllexport) const char* NAME = "SOURCE:MVM";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
	"Puts ReShade's effects into SOURCE:MVM renders and reports the overlay state to it.";

// Read by AfxHookSource2 (UiInputGate) via GetProcAddress: nonzero while the overlay is open.
extern "C" __declspec(dllexport) unsigned long long MvmReShadeOverlayOpen() {
	return (unsigned long long)g_overlayOpen;
}

// Called by AfxHookSource2 on the render thread at the before-UI boundary. Both pointers are native
// ID3D11Resource*, which is ReShade's D3D11 resource handle. depthTexture may be null. Returns true
// when the effects ran on colorTexture; on false ReShade still draws them at Present as usual, where
// the recorder does not see them.
extern "C" __declspec(dllexport) bool MvmReShadeRenderEffects(void* colorTexture, void* depthTexture) {
	if (colorTexture == nullptr) return false;
	bool ran = false;
	AcquireSRWLockShared(&g_lock);
	effect_runtime* runtime = g_runtime;
	if (runtime != nullptr) {
		device* dev = runtime->get_device();
		const resource color = { (uint64_t)(uintptr_t)colorTexture };
		const resource_desc desc = dev->get_resource_desc(color);
		// ReShade refuses a multisampled target. The before-UI target is resolved; never assume it.
		if (desc.texture.samples <= 1) {
			if (depthTexture != nullptr) {
				const resource depth = { (uint64_t)(uintptr_t)depthTexture };
				if (depth.handle != g_depthResource.handle) {
					// Only this thread touches the view while a runtime lives: the shared lock is
					// enough, the exclusive paths above cannot run alongside it.
					const format depthFormat = format_to_default_typed(dev->get_resource_desc(depth).texture.format, 0);
					resource_view view = { 0 };
					if (dev->create_resource_view(depth, resource_usage::shader_resource, resource_view_desc(depthFormat), &view)) {
						runtime->update_texture_bindings("DEPTH", view, view);
						ReleaseDepthView(dev);
						g_depthResource = depth;
						g_depthView = view;
					}
				} else if (g_depthView.handle != 0) {
					runtime->update_texture_bindings("DEPTH", g_depthView, g_depthView);
				}
			} else if (g_depthView.handle != 0) {
				// No depth this frame: an older frame's depth under this frame's colour is worse than none.
				runtime->update_texture_bindings("DEPTH", resource_view{ 0 }, resource_view{ 0 });
				ReleaseDepthView(dev);
			}
			// A view per call: a kept render-target view holds a reference on the engine's target,
			// which is what makes the engine's own resize fail.
			resource_view rtv = { 0 }, rtvSrgb = { 0 };
			const format linear = format_to_default_typed(desc.texture.format, 0);
			const format srgb = format_to_default_typed(desc.texture.format, 1);
			if (dev->create_resource_view(color, resource_usage::render_target, resource_view_desc(linear), &rtv)) {
				if (srgb == linear || !dev->create_resource_view(color, resource_usage::render_target, resource_view_desc(srgb), &rtvSrgb))
					rtvSrgb = rtv;
				runtime->render_effects(runtime->get_command_queue()->get_immediate_command_list(), rtv, rtvSrgb);
				if (rtvSrgb.handle != rtv.handle) dev->destroy_resource_view(rtvSrgb);
				dev->destroy_resource_view(rtv);
				ran = true;
			}
		}
	}
	ReleaseSRWLockShared(&g_lock);
	return ran;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OnReShadeOpenOverlay);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
		reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
