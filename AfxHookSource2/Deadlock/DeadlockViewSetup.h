#pragma once

// Where the camera pose reaches Deadlock.
//
// CS2's approach, for contrast: AfxHookSource2 AOB-scans client.dll for the engine's OWN
// `IsPlayingDemo()` call -- the one that gates its demo-view override -- overwrites those sixteen
// bytes with a call to a trampoline of ours, and uses the trampoline's RETURN VALUE as the
// view-ownership decision, so returning false makes the game skip its own demo-view pass and our
// pose survives. That is a precise, surgical hook, and it depends on knowing exactly which call
// site inside the function to replace.
//
// For Deadlock that call site is not identified yet. What IS identified, and confirmed unique on
// the installed build, is the view-setup function itself. So this detours the whole function and
// writes the pose into the view struct around the original call. Which side of the original the
// write lands on is the open question -- writing before it risks the engine recomputing over us,
// writing after risks the view already having been consumed -- so the phase is a runtime setting
// (`mirv_filmmaker view phase before|after|both`) rather than a guess baked into the build.
//
// Once the internal gate is found, this file is where the surgical patch replaces the blunt one.
// Docs/deadlock/signatures.md tracks that as an open item.

#include <windows.h>

namespace Deadlock {

// Installs the detour on the resolved view-setup function. Safe to call twice.
bool ViewSetup_Install();

bool ViewSetup_Installed();

// Opt-in read-only callback tracing for compatibility derivation; never drives the main pump.
bool ViewSetup_InstallProbe();

// Turns the hook's BODY off while leaving the detour installed and counting. `mirv_filmmaker view
// enable 0` is the one-launch answer to "is the camera hook what is breaking this".
bool ViewSetup_Enabled();
void ViewSetup_SetEnabled(bool enabled);

/// <summary>Skips only the write into the view, leaving the read, the gate and the override
/// stack running. Separates "the pose we produce is wrong" from "writing at all is wrong".</summary>
void ViewSetup_SetWriteEnabled(bool enabled);

// When the pose is written relative to the original function body.
enum class ViewPhase {
	Before,  // write, then call the original
	After,   // call the original, then write   (default)
	Both,    // write, call, write again
};

ViewPhase ViewSetup_GetPhase();
void ViewSetup_SetPhase(ViewPhase phase);
bool ViewSetup_PhaseFromName(const char* name, ViewPhase* out);
const char* ViewSetup_PhaseName(ViewPhase phase);

// Field displacements measured from the accepted main-view object in the installed Deadlock build.
// They remain settable at runtime so an update can be diagnosed without a rebuild.
struct ViewOffsets {
	ptrdiff_t origin;   // float[3]
	ptrdiff_t angles;   // float[3]  pitch, yaw, roll
	ptrdiff_t fov;      // float
	ptrdiff_t width;    // int
	ptrdiff_t height;   // int
};

ViewOffsets ViewSetup_GetOffsets();
void ViewSetup_SetOffsets(const ViewOffsets& offsets);

// ---------------------------------------------------------------------------------------------
// The preview viewport.
//
// A workspace that frames the game in a sub-rectangle needs the world RENDERED into that
// rectangle, or the frame is decoration around a crop. Deadlock offers no pre-UI render hook, so a
// post-process blit is not available: by the time this port can touch the back buffer, Panorama has
// already composited the chrome into it and a blit would shrink chrome and world together.
//
// The remaining lever is the view struct's own viewport, written at the same point the pose is.
// The engine then renders the world into the sub-rectangle natively, and the chrome composites over
// the rest of the screen at full size.
//
// THIS IS A HYPOTHESIS ABOUT AN UNVERIFIED FIELD LAYOUT. The viewport WIDTH and HEIGHT displacements
// are measured; the X and Y displacements next to them are inferred from the interleaved
// scaled/unscaled pairs at +0x440/+0x444 and +0x448/+0x44c, which is evidence but not proof. So the
// write is checked by reading the fields straight back, and a value that did not stick switches the
// whole mechanism off for the rest of the session rather than fighting the engine every frame. With
// it off the workspace still draws its framed preview; it frames a crop instead of the whole frame,
// and `mirv_filmmaker preview` says so.
// ---------------------------------------------------------------------------------------------

// Publish the rect the world should render into, as normalised screen fractions in [0,1].
// active=false restores the engine's own full-screen viewport.
void ViewSetup_SetPreviewViewport(bool active, float x0, float y0, float x1, float y1);

// True when the last viewport write read back as written, i.e. the world really is being scaled.
bool ViewSetup_PreviewViewportTaking();

// Master switch, for `mirv_filmmaker preview on|off`. Off is the honest fallback, not a failure.
void ViewSetup_SetPreviewViewportEnabled(bool enabled);
bool ViewSetup_PreviewViewportEnabled();

// The viewport X/Y displacements, separately settable so the inference above can be corrected from
// the console against a live `view dump` rather than needing a rebuild.
void ViewSetup_SetViewportOriginOffsets(ptrdiff_t x, ptrdiff_t y);
void ViewSetup_GetViewportOriginOffsets(ptrdiff_t& x, ptrdiff_t& y);

void ViewSetup_PreviewReport();

/// <summary>
/// Prints a window of the last accepted main-view object as dwords, floats and ints, so the field
/// offsets can be read off the live object rather than inherited from another game.
/// </summary>
void ViewSetup_Dump(size_t first, size_t last);

// How many times the detour has run, and how many of those wrote a pose. A call count of zero
// means the detour never installed or the function is not the per-frame one; a call count that
// climbs with a write count of zero means nothing is claiming the view.
unsigned long long ViewSetup_CallCount();
unsigned long long ViewSetup_WriteCount();

// Dumps the last observed view values to the console. The first thing to run when the camera does
// not move: it separates "we are not being called" from "we are writing to the wrong offsets".
void ViewSetup_Report();

} // namespace Deadlock
