# Render modules outside the build

This folder keeps render-pass code for possible reuse. Nothing here is listed in `AfxHookSource2/CMakeLists.txt`, so it does not build. The include paths already match this location. To build a file, add it to the CMake source list and wire up its callers.

## RenderDevOverlaySuppress

It suppresses CS2's ToolsVis watermark text overlay ("%s (mat_fullbright: %d)" etc.), so a debug-view or AOV frame can be delivered without the burned-in label. The module is self-contained:

- Lazy install: `RenderDevOverlaySuppress_Arm(true)` AOB-scans client.dll for the outlined
  overlay-text helper and detours it on first arm. No startup hook is needed.
- The pattern (`kDrawOverlayTextPattern`) is **version-coupled** to client.dll; a CS2 update can
  move it. A resolve failure warns and stays inactive instead of crashing.
- The module deliberately leaves the render-attribute getter alone (client.dll+0x166e00 era). Forcing
  those attributes to 0 black-holes characters and the viewmodel in the lighting AOVs. Only the text
  no-op is proven safe.

**Current subject-filter passes** live in `Render/RenderWorldOnly`, `Render/RenderWorldPlayersNoGun`
and `Render/RenderPlayersOnlyGreen`, driven by `Render/RenderSessionDriver`. Check
`Docs/features/filmmaker_render_passes.md` before assuming any particular capability is missing.
