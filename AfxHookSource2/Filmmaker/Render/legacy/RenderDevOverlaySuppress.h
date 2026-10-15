#pragma once

// Suppresses CS2's ToolsVis diagnostic overlay for the duration of a debug-view render job:
// the "%s (mat_fullbright: %d)" watermark text and the albedo/reflectivity reference charts.
//
// WHY A DETOUR AND NOT A CONVAR. There is none: the overlay draw in client.dll's ToolsVis
// pipeline builder is unconditional in tools mode (`developer 0` and r_showdebugoverlays were
// live-tested 2026-08-07 and change nothing), and retail strips the whole ToolsVis pipeline so
// it cannot render the AOVs at all. Verified in Ghidra (client.dll updated 2026-08-04):
//
//  - client.dll+0x117ef40 builds the ToolsVis view. It draws the watermark through a small
//    outlined-text helper at client.dll+0x116fe10 (9 shadow passes + 1 color pass through
//    vtable+0xE0; only 4 callsites, all diagnostic overlay text), and gates the reference
//    charts on two render-attribute lookups `GetAttr(dict, hash, default=1)` at
//    client.dll+0x166e00 with hashes 0x1F900C45 (albedo chart) and 0xDB409112
//    (reflectivity scale).
//
// So, while armed, the text helper becomes a no-op (kills the watermark and any sibling
// debug text). That is the ONLY detour this module owns.
//
// SCOPE LIMIT (measured 2026-08-07): the albedo/reflectivity reference CHART is NOT killable
// client-side -- it is composited inside the production shaders' ToolsVis branch from the
// map's baked `debug_chart_color` lightmap channel, with no gate anywhere (forcing the
// builder's five overlay attrs to 0, `developer 0`, and a loose-file texture override were
// each proven ineffective on a recorded frame; the attr forcing even BROKE probe lighting for
// characters/viewmodel in the lighting AOVs and was removed). The catalog avoids the chart
// instead: the `albedo` view ships as TOOLS_VIS_MODE_ALBEDO_NAN_INF_NEGATIVE, whose shader
// branch draws no chart.
//
// The ToolsVis pass itself (the AOV) is untouched. Armed only while a render job is saving
// state through restore -- RenderSessionDriver owns the arm/disarm calls -- plus a manual
// toggle for live A/B via `mirv_filmmaker render devoverlay`.
//
// Both prologue AOBs are version-coupled to client.dll; resolution failure warns and leaves
// the feature INACTIVE (jobs still run, with the overlay burned in and the manifest saying so).

namespace Filmmaker {

// Resolves the patterns and attaches both detours once. Safe to call repeatedly.
bool RenderDevOverlaySuppress_EnsureInstalled();

bool RenderDevOverlaySuppress_Installed();

// While armed, the overlay text helper no-ops and the two chart attributes read 0.
void RenderDevOverlaySuppress_Arm(bool armed);
bool RenderDevOverlaySuppress_Armed();

} // namespace Filmmaker
