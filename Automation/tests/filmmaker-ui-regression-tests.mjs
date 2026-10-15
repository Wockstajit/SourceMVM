import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "..", "..");
const read = (...parts) => fs.readFileSync(path.join(repo, ...parts), "utf8");

const timeline = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraTimelineJs.h");
const timelineRange = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraTimelineRangeJs.h");
const timelineHud = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraTimelineHud.cpp");
const playbackLoop = read("AfxHookSource2", "Filmmaker", "Movie", "PlaybackLoop.cpp");
const widgets = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraEditorWidgetsJs.h");
const camEval = read("AfxHookSource2", "Filmmaker", "Movie", "CamPathEval.cpp");
const cameraPath = read("AfxHookSource2", "Filmmaker", "Movie", "CameraPath.cpp");
const input = read("AfxHookSource2", "WindowInputHooks.cpp");
const follow = read("AfxHookSource2", "Filmmaker", "Movie", "FollowCamera.cpp");
const discovery = read("AfxHookSource2", "Filmmaker", "Movie", "FollowTargetDiscovery.cpp");
const configWidgets = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudWidgetsJs.h");
const configCamera = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudCameraTabJs.h");
const configHud = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHud.cpp");
const configHudHeader = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHud.h");
const windowResizeHeader = read("AfxHookSource2", "Filmmaker", "Window", "WindowResize.h");
const movieMode = read("AfxHookSource2", "Filmmaker", "Movie", "MovieMode.cpp");
const viewFx = read("AfxHookSource2", "Filmmaker", "Movie", "ViewFx.cpp");

test("regular-timeline loop flags are direct hit targets driven by the cursor pipe", () => {
  assert.doesNotMatch(timeline, /function pumpLoopFlags|function makeLoopFlag/);
  assert.match(timelineRange, /flag\.hittest\s*=\s*true/);
  assert.match(timelineRange, /function pumpRangeFlags\(\)[\s\S]{0,700}st\.mx[\s\S]{0,300}st\.lmb/);
  assert.doesNotMatch(timelineRange, /!rangeLastLmb\s*&&\s*st\.cursor/);
  assert.match(timelineHud, /publishCursor[^;]*PlaybackLoop_Enabled\(\)/);
  assert.match(timelineRange, /mirv_filmmaker loop range/);
});

test("native loop flags remain visible outside the editor and in Config", () => {
  assert.match(timeline, /updateRangeOverlay\(nativeBarVisible\)/);
  assert.doesNotMatch(timelineRange, /!nativeMode/);
  assert.doesNotMatch(timelineRange, /st\.configOpen[\s\S]{0,80}hideRangeOverlay/);
});

test("loop range tint follows live drags and optimistic state survives command latency", () => {
  assert.doesNotMatch(timelineRange, /dragBaseValid\s*\?/);
  assert.match(timelineRange, /rangePending[\s\S]{0,500}until/);
  assert.match(timelineRange, /rec\.held \|\| rangePending/);
});

test("enabling or moving an active loop seeks an outside playhead to loop In", () => {
  assert.match(playbackLoop, /SeekToLoopStartIfOutside/);
  assert.match(playbackLoop, /PlaybackLoop_SetEnabled[\s\S]{0,400}SeekToLoopStartIfOutside/);
  assert.match(playbackLoop, /PlaybackLoop_SetRange[\s\S]{0,500}SeekToLoopStartIfOutside/);
});

test("Game Mode preserves the custom slider appearance", () => {
  assert.doesNotMatch(widgets, /function cursorPipeUsable\(\) \{ return !!\(st && st\.cursor/);
  assert.match(widgets, /nativeFallback/);
  assert.match(widgets, /rec\.knob\.visible\s*=\s*!nativeFallback/);
});

test("Smooth remains effective for a two-key path", () => {
  assert.doesNotMatch(camEval, /needs >=4 markers/);
  assert.match(camEval, /m_smoothProgress/);
  assert.doesNotMatch(cameraPath, /m_data\.Count\(\) < 4/);
});

test("resize cursors survive UI mouse release", () => {
  assert.match(input, /IsResizeCursor/);
  assert.match(input, /IsResizeHitTest/);
});

test("switching to Weapon preserves the explicitly selected player", () => {
  assert.match(follow, /weaponPlayerIndex\s*=\s*type == FollowTargetType::Weapon \? m_lastPlayerIndex/);
  assert.match(discovery, /m_state\.weaponSource == WeaponSource::Held/);
  assert.match(discovery, /c\.held && c\.ownerIndex != watched/);
});

test("every camera effect has an independent Reset that disables at defaults", () => {
  // The four cards the CAMERA tab actually has. View Dynamics, View Bob, Turn Roll and Recoil
  // Shake were removed during 2026-09; their resets went with them.
  //
  // The Reset moved from the top of the card BODY into the card HEADER (Docs/panorama/panorama_ui_guide.md), so it is
  // reachable while the card is shut. It returns the same { setDisabled } handle either way,
  // which is what let the render hooks stay untouched across that move.
  for (const [card, reset] of [["svSec", "svReset"], ["hhSec", "hhReset"],
                               ["wrSec", "wrReset"], ["wbSec", "wbReset"]]) {
    assert.match(configCamera, new RegExp(`var ${reset} = ${card}\\.__headerReset\\(`));
    assert.match(configCamera, new RegExp(`${reset}\\.setDisabled\\(`));
  }
  assert.match(configWidgets, /body\.__headerReset = function[\s\S]{0,1200}setDisabled/);
  assert.match(configWidgets, /b\.hittest\s*=\s*!off/);
  for (const flag of ["smoothViewDefault", "handheldDefault",
                      "weaponRecoilDefault", "weaponBobDefault"]) {
    assert.match(configHud, new RegExp(flag));
  }
});

test("value drags are relative, and Shift keeps its own finer gain", () => {
  // Docs/panorama/panorama_ui_guide.md maintains the relative-drag contract, originally phrased as
  // "never jumping to the click, Shift = 1/5 gain". The Config panel's pump is relative for
  // every control it drives. The Camera Editor's keeps the older ABSOLUTE mapping for the legacy
  // sliders that still use it, and switches to relative for any rec flagged __relative -- which is
  // every redesigned value tile (edValueTile in CameraEditorCardsJs.h).
  for (const source of [widgets, configWidgets]) {
    assert.doesNotMatch(source, /DRAG_GAIN\s*=\s*0\.5/);
    // Relative: the committed value is the value at press plus the pointer's travel, scaled.
    assert.match(source, /__baseV \+ \(\(mx - rec\.__anchorX\) \/ w\)/);
  }
  assert.match(widgets, /DRAG_GAIN_FINE\s*=\s*0\.08/);
  assert.match(widgets, /rec\.__fine \|\| rec\.__relative/);
  assert.match(widgets, /GetPositionWithinWindow[\s\S]{0,300}mx\s*-\s*a\.x/);
  assert.match(configWidgets, /CFG_DRAG_FINE\s*=\s*0\.2/);
  // The Config pump has no absolute pointer-to-track branch left at all.
  assert.doesNotMatch(configWidgets.slice(configWidgets.indexOf("function cfgCursorPump"),
                                          configWidgets.indexOf("function cfgDragging")),
                      /mx\s*-\s*a\.x/);
});

test("Camera cards carry their Reset and their master switch in the header", () => {
  // Header order is action -> Reset -> switch -> chevron (Docs/panorama/panorama_ui_guide.md), and Panorama flows
  // siblings in creation order, so the call order in the fragment IS the visual order.
  assert.doesNotMatch(configWidgets, /\\u21BB Reset/);
  assert.match(configWidgets,
    /body\.__headerAction = function[\s\S]{0,4000}body\.__headerReset = function[\s\S]{0,2500}body\.__headerPill = function/);
  for (const [card, reset, toggle] of [["svSec", "svReset", "svToggle"],
                                       ["hhSec", "hhReset", "hhToggle"],
                                       ["wrSec", "wrReset", "wrToggle"],
                                       ["wbSec", "wbReset", "wbToggle"]]) {
    assert.match(configCamera,
      new RegExp(`var ${reset} = ${card}\\.__headerReset\\([\\s\\S]{0,500}var ${toggle} = ${card}\\.__headerPill\\(`));
  }
});

test("Config no longer duplicates the native Video resolution workflow", () => {
  assert.doesNotMatch(configCamera, /WINDOW SIZE|ConfigWindowWidth|mirv_filmmaker window size/);
});
