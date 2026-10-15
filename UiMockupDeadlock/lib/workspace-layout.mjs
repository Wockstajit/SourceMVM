// The workspace layout, ported from AfxHookSource2/Filmmaker/Deadlock/DeadlockWorkspaceLayoutMath.h.
//
// WHY A PORT EXISTS AT ALL. In game the panel draws NOTHING of its own accord: C++ solves the
// layout and hands it over in the state blob, and `render()` bails on its first lines when
// `st.layout.valid` is false. So a mockup with no solver shows an empty screen -- the most-edited
// surface in the tool would be the one it could not display.
//
// THE RISK THIS CARRIES, stated plainly: two copies of one piece of arithmetic can disagree, and a
// mockup that lays out differently from the game is worse than no mockup, because it lies
// confidently. Two things hold it honest, both in Automation/tests/deadlock-mockup-tests.mjs:
//
//   1. The same cases the C++ unit test asserts are asserted here against this port.
//   2. The CONSTANTS are read back out of the C++ header and compared, so the realistic drift --
//      someone retunes the minimum column width or the letterbox guard in the header -- fails the
//      test rather than silently moving the mockup out of step.
//
// Kept engine-free and dependency-free so both the browser and Node can run it unchanged.

// A column narrower than this cannot hold two value tiles side by side, which is the unit the card
// grammar is built from; wider than 62% of the screen and the preview stops being a preview.
export const MIN_INSPECTOR = 320;
export const MAX_INSPECTOR_FRACTION = 0.62;

// A band smaller than this is not a preview, it is a smear.
export const MIN_AREA_W = 64;
export const MIN_AREA_H = 48;

export const DEFAULT_ASPECT = 16 / 9;

function finitePositive(v) {
  return Number.isFinite(v) && v > 0;
}

export function clampInspector(requested, screenW) {
  if (!(requested > 0)) return 0;
  let w = Math.max(MIN_INSPECTOR, requested);
  if (screenW > 1) w = Math.min(w, Math.max(MIN_INSPECTOR, screenW * MAX_INSPECTOR_FRACTION));
  return Math.floor(w);
}

export function solve(input) {
  const {
    screenW = 0,
    screenH = 0,
    inspectorW = 0,
    leftW = 0,
    dockH = 0,
    headerH = 0,
    bottomBandH = 0,
    worldAspect = DEFAULT_ASPECT,
  } = input || {};

  const out = {
    valid: false,
    screenW, screenH,
    inspectorW: 0, leftW: 0, dockH: 0, headerH: 0,
    areaX: 0, areaY: 0, areaW: 0, areaH: 0,
    previewX: 0, previewY: 0, previewW: 0, previewH: 0,
    frameTopY: 0, frameBottomY: 0, bandBottomY: 0,
    blitX0: 0, blitY0: 0, blitX1: 0, blitY1: 0,
  };

  if (!finitePositive(screenW) || !finitePositive(screenH)) return out;

  out.inspectorW = clampInspector(inspectorW, screenW);

  const dock = Number.isFinite(dockH) ? Math.max(0, dockH) : 0;
  const header = Number.isFinite(headerH) ? Math.max(0, headerH) : 0;
  out.dockH = dock;
  out.headerH = header;

  // The left column -- the render workspace's JOB column -- insets the band exactly the way the
  // inspector does on the right, clamped to what is left so a narrow screen loses the column
  // rather than producing a negative band.
  const left = Math.min(Number.isFinite(leftW) ? Math.max(0, leftW) : 0,
                        Math.max(0, screenW - out.inspectorW - MIN_AREA_W));
  out.leftW = Math.floor(left);

  out.areaX = out.leftW;
  out.areaY = header;
  out.areaW = screenW - out.inspectorW - out.leftW;
  out.areaH = screenH - header - dock;

  if (out.areaW < MIN_AREA_W || out.areaH < MIN_AREA_H) return out;

  let aspect = worldAspect;
  if (!finitePositive(aspect)) aspect = DEFAULT_ASPECT;

  // The chrome paints a band across the foot of the area; the picture is letterboxed ABOVE it.
  const band = Number.isFinite(bottomBandH)
    ? Math.max(0, Math.min(bottomBandH, out.areaH * 0.5))
    : 0;
  const innerH = out.areaH - band;

  // Letterbox: fit the world's aspect inside the inner band, centred.
  let pw = out.areaW;
  let ph = pw / aspect;
  if (ph > innerH) {
    ph = innerH;
    pw = ph * aspect;
  }

  out.previewW = Math.floor(pw);
  out.previewH = Math.floor(ph);
  out.previewX = Math.floor(out.areaX + (out.areaW - out.previewW) * 0.5);
  out.previewY = Math.floor(out.areaY + (innerH - out.previewH) * 0.5);

  // The chrome's own rects, out of the same solve -- see DeadlockWorkspaceLayoutMath.h.
  out.frameTopY = out.areaY - 2;
  out.frameBottomY = Math.floor(out.areaY + out.areaH - band);
  out.bandBottomY = out.frameBottomY + 2;

  const clamp01 = (v) => Math.max(0, Math.min(1, v));
  out.blitX0 = clamp01(out.previewX / screenW);
  out.blitY0 = clamp01(out.previewY / screenH);
  out.blitX1 = clamp01((out.previewX + out.previewW) / screenW);
  out.blitY1 = clamp01((out.previewY + out.previewH) / screenH);

  out.valid = (out.blitX1 - out.blitX0) > 0.01 && (out.blitY1 - out.blitY0) > 0.01;
  return out;
}

// Where a tick sits along a timeline strip, and back again. The timeline and the graph are two views
// of one shot, so both map ticks to pixels with this rather than each rolling its own and
// disagreeing by a pixel at the ends.
export function tickToX(tick, firstTick, lastTick, stripX, stripW) {
  if (!(lastTick > firstTick) || !finitePositive(stripW)) return stripX;
  const t = Math.max(0, Math.min(1, (tick - firstTick) / (lastTick - firstTick)));
  return stripX + t * stripW;
}

export function xToTick(x, firstTick, lastTick, stripX, stripW) {
  if (!(lastTick > firstTick) || !finitePositive(stripW)) return firstTick;
  const t = Math.max(0, Math.min(1, (x - stripX) / stripW));
  return firstTick + t * (lastTick - firstTick);
}
