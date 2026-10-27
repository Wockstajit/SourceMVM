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
const uiClickMath = read("AfxHookSource2", "Filmmaker", "Movie", "UiClickMath.h");
const viewFx = read("AfxHookSource2", "Filmmaker", "Movie", "ViewFx.cpp");
const filmmaker = read("AfxHookSource2", "Filmmaker", "Filmmaker.cpp");
const configJs = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudJs.h");
const propsGizmo = read("AfxHookSource2", "Filmmaker", "Panorama", "ConfigHudPropsGizmoJs.h");
const graphJs = read("AfxHookSource2", "Filmmaker", "Panorama", "GraphEditorJs.h");

test("regular-timeline loop flags are direct hit targets driven by the cursor pipe", () => {
  assert.doesNotMatch(timeline, /function pumpLoopFlags|function makeLoopFlag/);
  // Each flag's painted tab, and a grab strip over its thin line, take the click themselves.
  assert.match(timelineRange, /var tab = mk\('Panel', rail\); tab\.hittest\s*=\s*true/);
  assert.match(timelineRange, /var grab = mk\('Panel', rail\); grab\.hittest\s*=\s*true/);
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

test("leaving Config hands the mouse back to the game, and a right-click look always ends", () => {
  // Issue 41 follow-up: Config closed in UI mode (the ENVIRONMENT tab keeps the left button for
  // editing) left the timeline cursor on, so GameClick skipped every click in plain demo viewing.
  // The dock re-asserts EditorHosted while Config holds it, so the reset must follow the release.
  assert.match(configHud, /void ConfigHud::OnExit\(\)[\s\S]{0,400}TimelineDock_Release\(DockHost::Config\);[\s\S]{0,600}if \(!IsLive\(\) && !CameraTimelineHudRef\(\)\.EditorHosted\(\)\)\s*CameraTimelineHudRef\(\)\.SetCursor\(false\)/);
  // While the game holds the mouse no WM_RBUTTONUP arrives and GetAsyncKeyState does not see the
  // button (polling it ended every look one frame in), so the raw release ends the look.
  assert.match(input, /void WindowInputHooks_FilterRawInput[\s\S]{0,700}RI_MOUSE_LEFT_BUTTON_UP\) Filmmaker::MovieInput_ButtonUp\(0\);\s*if \(m\.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP\) Filmmaker::MovieInput_ButtonUp\(1\);/);
  assert.doesNotMatch(input, /GetAsyncKeyState\(VK_RBUTTON\)[^;]*RightButton/);
  assert.match(movieMode, /void MovieMode::OnButtonUp\(int button\)\s*\{[\s\S]{0,1400}if \(button != 1 \|\| !m_rightLook\.exchange\(false\)\) return;\s*EnqueueCmd\("mirv_filmmaker camtl cursor on"\)/);
  // The right-hold look is the fly cam's, which reads the mouse itself: in CS2's own demo cameras
  // the press often never started CS2's drag-look under the Config panel (2026-10-17), so the hold
  // switches to the fly cam first, before handing it the mouse.
  assert.match(movieMode, /m_rightLook = true;[\s\S]{0,700}if \(!CameraBridge_GetFreeCamEnabled\(\)\) EnqueueCmd\("mirv_input camera"\);\s*EnqueueCmd\("mirv_filmmaker camtl cursor off"\);/);
});

test("a quick right-click in the ENVIRONMENT view opens the object's menu, a hold looks", () => {
  // The view's pick menu was dropped for the right-hold look (#42); the user wants both. The press
  // waits: released within 200 ms and 4 px it is a click (the menu), held or moved it is the look.
  assert.match(movieMode, /UiClick::RightPressLooks\(ClickInput\(x, y\)\)\) \{\s*\{\s*std::lock_guard<std::mutex> lk\(m_rightMutex\);\s*m_rightPress\.pending = true;/);
  // The 200 ms / 4 px rule is UiClickMath.h's, shared with the CS:S hook.
  assert.match(uiClickMath, /constexpr int kRightLookHoldMs = 200;\s*constexpr int kRightLookSlopPx = 4;/);
  assert.match(movieMode, /void MovieMode::StartRightLookIfHeld\(\) \{[\s\S]{0,700}if \(!UiClick::RightPressBecomesLook\(heldMs, x - m_rightPress\.x, y - m_rightPress\.y\)\) return;/);
  assert.match(movieMode, /if \(!rmb\) \{ m_rightPress\.pending = false; return; \}/);
  assert.match(movieMode, /void MovieMode::FlushActions\(\) \{\s*StartRightLookIfHeld\(\);/);
  assert.match(movieMode, /if \(click\) \{[\s\S]{0,200}EnqueueCmd\("mirv_filmmaker config viewmenu"\);/);
  assert.match(configHud, /0 == _stricmp\(arg, "viewmenu"\)[\s\S]{0,300}RequestEval\("\$\.ConfigHud\.viewMenu\(\)"\)/);
  assert.match(configJs, /api\.viewMenu = function \(\) \{ propsMenuRequest = true; return 'ok'; \};/);
  assert.match(propsGizmo, /if \(propsMenuRequest\) \{[\s\S]{0,300}propsCommand\('pick ' \+ u\.toFixed\(4\) \+ ' ' \+ v\.toFixed\(4\)\);/);
  assert.match(propsGizmo, /propsOpenMenu\('p' \+ p\.selected\)[\s\S]{0,200}propsOpenMenu\('e' \+ envSel\)/);
  // The TIMELINE dock's right-click swallow leaves CONFIG's column alone: the Outliner's own
  // right-click menu (oncontextmenu) needs the message.
  assert.match(movieMode, /GraphEditorExperiment_WantsCursor\(\) && !OnConfigColumn\(x, y\)\)\s*return true;/);
  assert.match(movieMode, /bool MovieMode::OnConfigColumn\(int x, int y\) \{[\s\S]{0,600}TimelineDock_Height\(\) \/ 1080\.f[\s\S]{0,200}return !in\.preview\.Contains\(in\.x, in\.y\);/);
});

test("a key in the TIMELINE dock can be gone to: double-click it, or its menu's Go to this key", () => {
  assert.match(graphJs, /function keyDoubleClick\(t\) \{[\s\S]{0,300}now - prev\.at > KEY_DBL_MS \|\| Math\.abs\(prev\.t - t\) > 0\.5/);
  assert.match(graphJs, /if \(keyDoubleClick\(t\)\) \{ seekTo\(t\); gesture\.seek = false; \}/);
  assert.match(graphJs, /var lt = n \? keyTimeNear\(r\.node, cc\.tx\) : null;\s*if \(lt !== null && keyDoubleClick\(lt\)\) seekTo\(lt\);/);
  assert.match(graphJs, /\{ id: 'goto', label: 'Go to this key', sc: 'Double-click' \},\s*\{ id: 'delete'/);
  assert.match(graphJs, /if \(id === 'goto'\) seekTo\(entry\.t\);\s*else if \(id === 'delete'\) deleteSelection\(\);/);
  // The second press of a quick double-click can arrive as WM_LBUTTONDBLCLK: still a press to the pipe.
  assert.match(input, /if \(uMsg == WM_LBUTTONDBLCLK\) \{ g_UiCursorProbe\.lmb = true;/);
});

test("a camera path that plays to its end gives the view back to CS2's own camera", () => {
  // Leaving our free cam parked on the last key left users with no mouse control after a path
  // (2026-10-17). The release armed by the end, once the last key has landed, re-applies the camera
  // CS2 was showing; any other release or scrub stop clears the request.
  assert.match(cameraPath, /void CameraPath::FinishPlaybackAtLastKey\(\)[\s\S]{0,1200}HoldEditorAtMarker\(lastIndex\);\s*\/\/[^\n]*\n\s*m_handBackOnRelease = m_scrubReleasePending;/);
  assert.match(cameraPath, /void CameraPath::ArmScrubRelease\(int targetTick\) \{\s*m_scrubReleasePending = true;\s*m_handBackOnRelease = false;/);
  assert.match(cameraPath, /void CameraPath::StopScrub\(\) \{\s*m_scrubReleasePending = false;\s*m_handBackOnRelease = false;/);
  assert.match(cameraPath, /free cam handed back[\s\S]{0,200}if \(m_handBackOnRelease\) \{\s*m_handBackOnRelease = false;\s*HandBackAtLastKey\(\);/);
  // CS2's spec_goto turns its roaming camera without moving it, so the restore is CS2's own camera
  // as it was, the same one closing the Camera Editor applies.
  assert.match(cameraPath, /void CameraPath::HandBackAtLastKey\(\) \{\s*if \(Render_IsJobActive\(\) \|\| FollowCameraRef\(\)\.OwnsView\(\)\)\s*return;\s*MovieModeRef\(\)\.SetMode\(MovieModeRef\(\)\.GetMode\(\)\);/);
});

test("outside the workspaces a click on our free cam is a hold-to-look, so the mouse comes back", () => {
  // After a right-hold look in Config the free cam stays on; the first click after Config closed
  // took the mouse for it until Esc (2026-10-17). Outside Config, the Camera Editor and Render the
  // press steers only while held, and either button's release (legacy or raw) hands the mouse back.
  assert.match(movieMode, /bool MovieMode::OnMouseButton\(int button, bool down, int x, int y\) \{\s*if \(!down\) OnButtonUp\(button\);/);
  assert.match(movieMode, /MouseCapture_TakeForCamera\("freecam click"\);[\s\S]{0,300}if \(!CameraEditor_Enabled\(\) && !ConfigHud_Enabled\(\) && !Render_IsModalOpen\(\)\)\s*m_flyHoldButton = button;/);
  assert.match(movieMode, /void MovieMode::OnButtonUp\(int button\) \{\s*int held = button;\s*if \(m_flyHoldButton\.compare_exchange_strong\(held, -1\)\) \{\s*MouseCapture_ReleaseForUi\("freecam hold released"\);/);
});

test("the TIMELINE dock under Config follows the shared cursor flag, so a right-hold can look", () => {
  // The dock's cursor claim suspends MirvInput. Docked in Config it claimed the cursor whatever the
  // flag said, so "camtl cursor off" from the right-hold never reached the fly cam (2026-10-17).
  // Only the standalone graph editor, with no workspace hosting the dock, keeps the cursor always.
  const body = filmmaker.slice(filmmaker.indexOf("bool GraphEditorExperiment_WantsCursor()"),
                               filmmaker.indexOf("// --- Fisheye lens (barrel"));
  assert.match(body, /if \(TimelineDock_Host\(\) != DockHost::None \|\| CameraEditorHudRef\(\)\.Enabled\(\)\)\s*return CameraTimelineHudRef\(\)\.Cursor\(\);\s*return true;/);
  assert.doesNotMatch(body, /return !CameraEditorHudRef\(\)\.Enabled\(\) \|\|/);
});

// 2026-10-17 (mvm_debug 08:11-08:13): three input leaks outside the workspaces.
test("a dock scrub flies the camera path only while the path layer's AUTO is on", () => {
  const graphHud = read("AfxHookSource2", "Filmmaker", "Panorama", "GraphEditorExperimentHud.cpp");
  assert.match(graphHud, /return ScrubDrives\(\) \|\| m_editDriveFrames > 0 \|\| m_releaseDrive;/);
  assert.match(graphHud, /bool GraphEditorExperimentHud::ScrubDrives\(\) const \{\s*return TimelinePlayhead_Scrubbing\(\) && CameraPath_AutoPath\(\);/);
  assert.match(graphHud, /if \(!ScrubDrives\(\) && !editDrive\)\s*return;/, "the per-frame drive takes the same gate");
  assert.match(graphHud, /if \(wasScrubbing && CameraPath_AutoPath\(\)\) m_releaseDrive = true;/);
});

test("the UI-mouse flag only counts inside a workspace, and the editor gives it up on close", () => {
  const filmmaker = read("AfxHookSource2", "Filmmaker", "Filmmaker.cpp");
  const editorHud = read("AfxHookSource2", "Filmmaker", "Panorama", "CameraEditorHud.cpp");
  assert.match(filmmaker, /return tl\.Cursor\(\) && \(tl\.EditorHosted\(\) \|\| tl\.CursorForced\(\) \|\| ConfigHud_Enabled\(\)\);/);
  assert.match(editorHud, /if \(!ConfigHud_Enabled\(\)\) tl\.SetCursor\(false\);/);
});

test("outside the workspaces a press on the native demo bar is the bar's, not a free-cam look", () => {
  const movie = read("AfxHookSource2", "Filmmaker", "Movie", "MovieMode.cpp");
  assert.match(timeline, /root\.SetAttributeString\('barrect', r\)/, "the bar's rect is published");
  assert.match(timeline, /publishCameraMode\(\);\s*publishBarRect\(\);/, "every frame, with the camera mode");
  assert.match(timelineHud, /m_symBarRect = m_bridge\.MakeSymbol\("barrect"\);/);
  assert.match(timelineHud, /ReadBarRect\(\);/);
  assert.match(movie, /if \(!CameraEditor_Active\(\) && !ConfigHud_Enabled\(\) && CameraTimeline_DemoBarContains\(x, y\)\) \{[\s\S]{0,200}return true;/,
    "OnUiClick hands the press to Panorama before the free cam or a game click can take it");
});

// 2026-10-17: the end-of-demo hold restarted the demo from the beginning whenever playback came
// while it held (play, a scrub's seek, a camera path), and held on the final round's first tick.
test("the end-of-demo hold stays put, a second after the final round, and never restarts the demo", () => {
  const hold = read("AfxHookSource2", "Filmmaker", "Movie", "DemoEndHold.cpp");
  assert.doesNotMatch(hold, /"demo_gototick %d"/, "no seek back to the start");
  assert.match(hold, /constexpr int kPostGameTailTicks = 64;/);
  assert.match(hold, /holdTick = s_postGameTick \+ kPostGameTailTicks;/);
  assert.match(hold, /if \(EndHoldHost_ScrubActive\(\)\) return;\s*\n[\s\S]{0,700}EndHoldHost_Pause\(\);/,
    "a scrub's own steps are left alone; anything else playing past the hold is paused");
  const host = read("AfxHookSource2", "Filmmaker", "Movie", "DemoEndHoldHostSource2.cpp");
  assert.match(host, /bool EndHoldHost_ScrubActive\(\) \{ return DemoScrub_Active\(\); \}/);
  assert.match(host, /void EndHoldHost_Pause\(\) \{ DemoCmd\("demo_pause"\); \}/);
});

// 2026-10-17 (mvm_debug: clickMode=0 free=1): a click while a camera path played, or on Clippy,
// called MouseCapture_SetFree(true), which also switches click-to-capture off for good -- after
// that no click gave CS2 the mouse back and looking around was dead until a restart.
test("only explicit mouse commands switch click-to-capture off", () => {
  const movie = read("AfxHookSource2", "Filmmaker", "Movie", "MovieMode.cpp");
  const hooks = read("AfxHookSource2", "WindowInputHooks.cpp");
  const osu = read("AfxHookSource2", "Filmmaker", "EasterEggs", "Osu", "OsuGame.cpp");
  assert.doesNotMatch(movie, /MouseCapture_SetFree\(/);
  assert.doesNotMatch(hooks, /MouseCapture_SetFree\(/);
  assert.match(osu, /if \(g_prevClickToCapture\) \{ MouseCapture_SetClickToCapture\(true\);/, "osu! gives back the mode it found");
});

// 2026-10-17 (mvm_debug fx log, MoNaTy USP-S demo): CS2 no longer creates blood_impact_headshot, so
// money only paid out on helmeted heads. A head hit no helmet swap covered spawns the burst itself.
test("money on headshot covers heads without a helmet, and never a seek's replayed hits", () => {
  const money = read("AfxHookSource2", "Filmmaker", "Movie", "ParticleFxMoney.cpp");
  const hook = read("AfxHookSource2", "Filmmaker", "Movie", "ParticleFxHook.cpp");
  const fxCpp = read("AfxHookSource2", "Filmmaker", "Movie", "ParticleFx.cpp");
  assert.match(money, /if \(!gate\.death && gate\.hitgroup == 1\) \{/, "the head hit queues a spawn");
  assert.match(money, /SpawnParticleAt\(kMoneyBurst, at\)/);
  assert.match(money, /const bool helmet = m\.tick >= 0 && std::abs\(helmetTick - m\.tick\) <= 1;/, "no second burst on a helmet swap");
  assert.match(money, /std::abs\(tickNow - m\.tick\) > kMoneyStaleTicks/, "a seek's replayed events are skipped");
  assert.match(hook, /g_helmetMoneyTick\.store\(/);
  assert.match(fxCpp, /MoneyShot_PumpMainThread\(\);/);
});

// 2026-10-18: a new helper reused the name of a label the Config panel already declared, and the
// whole Scene objects panel stopped loading ("Identifier 'propsPaintModels' has already been
// declared"). The Config panel's fragments share one scope.
test("the Config panel's fragments never declare one name twice", () => {
  const dir = path.join(repo, "AfxHookSource2", "Filmmaker", "Panorama");
  for (const file of fs.readdirSync(dir).filter((f) => /^ConfigHud.*Js\.h$/.test(f))) {
    const src = read("AfxHookSource2", "Filmmaker", "Panorama", file);
    const seen = new Map();
    for (const m of src.matchAll(/^    (?:function (\w+)\s*\(|var (\w+)\s*=)/gm)) {
      const name = m[1] || m[2];
      assert.ok(!seen.has(name), `${file} declares ${name} twice`);
      seen.set(name, true);
    }
  }
});

// 2026-10-18: the wheel over an open developer console moved the camera instead of scrolling it.
test("the open developer console keeps its own wheel, clicks and keys", () => {
  const gate = read("AfxHookSource2", "UiInputGate.cpp");
  const main = read("AfxHookSource2", "main.cpp");
  assert.match(gate, /if \(AfxConsoleVisible\(\)\)\s*return true;/);
  assert.match(main, /bool AfxConsoleVisible\(\) \{ return g_pGameUIService && g_pGameUIService->Con_IsVisible\(\); \}/);
});

// 2026-10-18: a camera riding a gun made the player look jittery. Smooth View's breathing, lean and
// sway (logged up to 4 degrees with pov=0) were added to our own cameras after they wrote the pose.
test("Smooth View leaves our own cameras alone", () => {
  const main = read("AfxHookSource2", "main.cpp");
  assert.match(main, /g_PendingPostDemoView\.filmmakerCamera = owner\.Any\(\);/);
  assert.match(main, /if \(smoothView\.Enabled\(\) && !pending\.filmmakerCamera && !g_MirvInputEx\.m_MirvInput->GetCameraControlMode\(\)\) \{/);
});
