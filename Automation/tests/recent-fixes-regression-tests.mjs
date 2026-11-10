// Static regression checks for the bug pass of 2026-11-10 (fix/recent-feature-bugs): each rule
// below was a defect found by reading the last two weeks of features.
// node --test Automation/tests/recent-fixes-regression-tests.mjs
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8').replace(/\r\n/g, '\n');
const takes = read('AfxHookSource2/Filmmaker/Takes/Takes.cpp');

test('ReShade alone also keeps the clean copy the pass compositor reads', () => {
  const hooks = read('AfxHookSource2/RenderSystemDX11Hooks.cpp');
  assert.match(hooks, /const bool lensRuns = beautyEffects && Filmmaker::LensFx_Active\(\);/);
  assert.match(hooks, /const bool reshadeRuns = beautyEffects && g_ReShadeAdvancedfx\.IsConnected\(\) && g_bEnableReShade;/);
  const keep = hooks.indexOf('if ((lensRuns || reshadeRuns) && pLensCommands && pLensCommands->KeepCleanColor) {');
  assert.ok(keep > 0, 'the copy is taken when either effect will run');
  // Before both effects, and only once.
  assert.ok(keep < hooks.indexOf('if (lensRuns) {') && keep < hooks.indexOf('if (reshadeRuns) {'));
  assert.equal(hooks.match(/AfxRenderPass::KeepCleanColor\(pDeviceContext, pSceneTexture\);/g).length, 1);
});

test('deleting the last take leaves an ordinary match alone, and an emptied scene does not come back', () => {
  assert.equal((takes.match(/KickExtraBots\(\);/g) || []).length, 3, 'WaitForSpawns plus the two guarded calls');
  assert.equal((takes.match(/if \(g\.setupDone\) KickExtraBots\(\);/g) || []).length, 2, 'clear and delete both wait for setup');
  const autosave = takes.match(/void Autosave\(\) \{[\s\S]*?\n\}\n/)[0];
  assert.match(autosave, /SaveScene\("_last"\);\s*return;/);
  assert.match(autosave, /if \(map\.empty\(\) \|\| map != g\.lastMap\) return;/, 'never before this map\'s autosave was read');
  assert.match(autosave, /std::filesystem::remove\(SceneFolder\(map\) \/ std::filesystem::u8path\(SafeFileName\("_last"\) \+ "\.mvmtakes"\), ec\);/);
  assert.match(takes, /if \(g\.setupDone\) KickExtraBots\(\);\s*Autosave\(\);\s*Say\("scene cleared"\);/);
});

test('one F6 or F7 press is one action, however long the key is held', () => {
  const key = takes.match(/bool HandleKey\(unsigned vkey, bool down\) \{[\s\S]*?\n\}\n/)[0];
  // The repeat comes from the key message, so no held state can be left set by a release that
  // the pause menu or the ReShade overlay swallowed.
  assert.match(key, /if \(down && !g_keyRepeat\.load\(std::memory_order_relaxed\)\) g_pendingKey\.store/);
  assert.doesNotMatch(key, /s_held|held\./);
  const hooks = read('AfxHookSource2/WindowInputHooks.cpp');
  const note = hooks.indexOf('Filmmaker::Takes::NoteKeyRepeat(0 != (lParam & (1LL << 30)));');
  assert.ok(note > 0 && note < hooks.indexOf('Filmmaker::MovieInput_OnKey((int)wParam, true)'), 'noted before the key is handled');
});

test('a Record demo that never started is called off', () => {
  assert.match(takes, /void DoStop\(\) \{[\s\S]{0,260}g\.demoPending = false;\s*if \(g\.rec\.active \|\| g\.rec\.armed\)/);
  assert.equal((takes.match(/g\.demoPending = false;/g) || []).length, 4, 'pass start, Stop, bot timeout, match gone');
});

test('the window does not resize under a running render', () => {
  const win = read('AfxHookSource2/Filmmaker/Window/WindowResize.cpp');
  const guard = win.indexOf('(0 == _stricmp(sub, "size") || 0 == _stricmp(sub, "aspect")) && Render_IsJobActive()');
  assert.ok(guard > 0);
  assert.ok(guard < win.indexOf('RequestSize(width, height, cmd);', guard));
  assert.match(read('AfxHookSource2/Filmmaker/Panorama/CameraEditorRenderJs.h'), /picSizeRow\.visible = sizeFits > 0 && !r\.jobActive;/);
});

test('a map picture job is retired, never destroyed while it runs', () => {
  const alt = read('AfxHookSource2/Filmmaker/AltEnvironment/AltEnvironment.cpp');
  const pick = alt.match(/void Pick\(const std::string& name, const std::string& addon\) \{[\s\S]*?\n\}\n/)[0];
  assert.doesNotMatch(pick, /pickJob\.reset\(\)/);
  assert.match(pick, /s\.pickJob->cancel = true;\s*s\.retired\.push_back\(std::move\(s\.pickJob\)\);/);
});

test('the viewer\'s HUD values survive a quit or crash with the In-game view up', () => {
  const hud = read('AfxHookSource2/Filmmaker/Movie/GameplayHudLook.cpp');
  assert.match(hud, /const bool keep = g_restorePending > 0 \|\| LoadOwed\(\);/, 'values owed are kept, not read from CS2 again');
  assert.match(hud, /if \(!owed\.empty\(\)\) \{\s*FilmmakerSettings_SetUi\(kOwedKey, owed\.c_str\(\)\);/);
  assert.match(hud, /if \(!inGameViewShown && LoadOwed\(\)\) \{[\s\S]{0,300}Restore\(\);/, 'a new session puts them back');
  assert.match(hud, /if \(g_restorePending == 0\) FilmmakerSettings_SetUi\(kOwedKey, "none"\);/);
});

test('a removed ragdoll is caught in the entity list before its memory is read', () => {
  const bodies = read('AfxHookSource2/Filmmaker/Movie/RagdollBodies.cpp');
  const cur = bodies.match(/bool Current\(const BodySet& set\) \{[\s\S]*?\n\}\n/)[0];
  const slot = cur.indexOf('EntityInSlot(set.carrierIndex) != set.carrier) return false;');
  assert.ok(slot > 0 && slot < cur.indexOf('RouteObject(set.route, set.carrier)'));
});

test('a left-hand window keeps its left edge when its column width changes', () => {
  assert.match(read('AfxHookSource2/Filmmaker/Panorama/ModuleWindowJs.h'),
    /if \(lastW > 0 && w !== lastW && opts\.side !== 'left'\) x -= \(w - lastW\) \* scale;/);
});

test('the fog drift survives a grid rebuild and starts again only after the fog was off', () => {
  const fog = read('AfxHookSource2/Filmmaker/Environment/EnvironmentGroundFog.cpp');
  const release = fog.match(/void ReleaseFluid\(\) \{[\s\S]*?\n\}\n/)[0];
  assert.doesNotMatch(release, /state\.drift = /, 'PrepareGrid releases the fluid when collision data arrives');
  assert.match(fog, /\+\+state\.idle == kIdleFrames\) \{\s*ReleaseFluid\(\);\s*state\.drift = GroundFogDrift\{\};/);
  assert.equal((fog.match(/state\.drift = GroundFogDrift\{\};/g) || []).length, 1);
});

test('one unreadable HUD variable does not drop what the others are owed', () => {
  const hud = read('AfxHookSource2/Filmmaker/Movie/GameplayHudLook.cpp');
  const join = hud.match(/std::string JoinSaved\(\) \{[\s\S]*?\n\}\n/)[0];
  assert.match(join, /out \+= c\.saved\.empty\(\) \? "x" : c\.saved;/);
  assert.match(join, /return any \? out : std::string\(\);/);
  const load = hud.match(/bool LoadOwed\(\) \{[\s\S]*?\n\}\n/)[0];
  assert.match(load, /g_cvars\[i\]\.saved = parts\[i\] == "x" \? std::string\(\) : parts\[i\];/);
  assert.match(load, /if \(!any\) return false;/);
  // The startup recovery waits for the console variables, not for one particular variable.
  assert.match(hud, /if \(!s_startupChecked && AnyCvarReadable\(\)\) \{/);
});

test('a user resize is answered with the current size while a render runs', () => {
  const win = read('AfxHookSource2/Filmmaker/Window/WindowResize.cpp');
  // The border drag: the proposal is replaced before the snap logic sees it.
  assert.match(win, /case WM_SIZING: \{[\s\S]{0,700}if \(Render_IsJobActive\(\)\) \{\s*RECT now;\s*if \(::GetWindowRect\(hwnd, &now\)\) \*proposed = now;\s*break;/);
  // Snap, maximise and restore: only the user's, never a resize the hook asks for itself.
  assert.match(win, /if \(Render_IsJobActive\(\) && \(s_inModalLoop \|\| s_userSizeCommand\)\) \{/);
  assert.match(win, /command == SC_MAXIMIZE \|\| command == SC_RESTORE \|\| command == SC_SIZE/);
  // Minimising, and coming back from it, are left alone.
  assert.match(win, /\(pos->flags & SWP_NOSIZE\) == 0 && !::IsIconic\(hwnd\) && pos->x > -30000/);
});

test('the HUD debt is on disk before the player values are written', () => {
  const hud = read('AfxHookSource2/Filmmaker/Movie/GameplayHudLook.cpp');
  const apply = hud.match(/void Apply\(\) \{[\s\S]*?\n\}\n/)[0];
  const flush = apply.indexOf('FilmmakerSettings_Flush();');
  assert.ok(flush > 0 && flush < apply.indexOf('ExecClientCmd('), 'flushed before the first command');
  assert.ok(apply.indexOf('FilmmakerSettings_SetUi(kOwedKey') < flush);
});
