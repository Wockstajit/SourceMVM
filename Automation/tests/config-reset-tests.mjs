// Every RESET in the CONFIG panel goes through one runner, with one behaviour.
//
// Three defects this guards, all reported from the game:
//
//   * A reset only wrote "set it to the default". If a value was ALREADY at its default the
//     write was a no-op and nothing re-applied, so resetting a card could leave the old state
//     rendering. Resets now go OFF first, then to defaults.
//   * The environment card resets never touched the renderer's master `enabled`, so afterwards
//     the master and the passes disagreed: a card read as on and did nothing until it was
//     switched off and on again by hand. envRelatch does that latch itself now.
//   * The tab footers armed on the first click and only fired on a second one within four
//     seconds -- so a click, a glance, and a second click just re-armed it. From the outside,
//     a RESET button that does nothing. Footer resets are single-click, like the card chips.
//
// Static checks against the sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (name) =>
  fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Panorama/' + name, import.meta.url), 'utf8');

const widgets = read('ConfigHudWidgetsJs.h');
const nav = read('ConfigHudEnvironmentNavJs.h');
const hud = read('ConfigHudJs.h');

test('there is exactly one reset runner and every builder uses it', () => {
  assert.match(widgets, /function cfgRunReset\(spec\)/);
  // The three phases, in order.
  assert.match(widgets, /if \(spec\.off\)[\s\S]{0,120}if \(spec\.cmds\)[\s\S]{0,200}if \(spec\.after\)/);
  // Both surviving builders route through it rather than firing commands inline.
  assert.match(widgets, /body\.__headerReset = function \(cmds, resetName\)/);
  assert.match(widgets, /cfgRunReset\(cfgResetSpec\(cmds\)\)/);
  // The footer's run() goes through it too, after an optional confirm (spec.confirm).
  assert.match(widgets, /function run\(\) \{[\s\S]{0,300}cfgConfirm\([\s\S]{0,120}cfgRunReset\(cfgResetSpec\(resolve\(\)\)\)[\s\S]{0,80}cfgRunReset\(cfgResetSpec\(e\)\);/);
  // The third, unused builder is gone -- it had no call sites and would drift.
  assert.doesNotMatch(widgets, /function effectReset\(/,
    'effectReset had no call sites; a third reset builder must not come back');
});

test('a footer reset fires on one click', () => {
  // The arm/confirm two-step is what made RESET VFX read as broken.
  assert.match(widgets, /b\.SetPanelEvent\('onactivate', function \(\) \{ run\(\); \}\)/);
  assert.doesNotMatch(widgets, /\$\.Schedule\(4\.0, function \(\) \{ if \(t2 === token\) disarm\(\); \}\)/,
    'the 4-second auto-disarm must not come back');
});

test('an environment card reset goes off -> default -> release, and NEVER re-enables', () => {
  assert.match(nav, /function envResetCmds\(keys\)/);
  assert.match(nav, /return \{ off: off, cmds: def\.concat\(off\), after: function \(\) \{ envAfterReset\(keys\); \} \};/);
  assert.match(nav, /function envAfterReset\(keys, extra\)/);
  // THE RELATCH MUST NOT COME BACK. It dropped the master and raised it again, which is how
  // resetting one card could leave the renderer -- and unrelated passes -- switched ON. A reset
  // may only ever release an idle renderer, never start one.
  assert.doesNotMatch(nav, /envCommand\('climate enabled 0'\);[\s\S]{0,160}envCommand\('climate enabled 1'\)/,
    'a card reset must never raise the renderer master');
  assert.doesNotMatch(nav, /function envRelatch\(/, 'envRelatch must stay gone');
  assert.match(nav, /envReleaseIfIdle\(just\)/);
  // It must decide on the POST-reset values, not the stale mirrored state.
  assert.match(nav, /just\[keys\[i\]\] = CL_DEFAULT\[keys\[i\]\]/);
});

test('shot FOV is whole degrees and persists with CAMERA keep', () => {
  const cameraTab = read('ConfigHudCameraTabJs.h');
  const store = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Config/FilmmakerSettingsStore.cpp', import.meta.url), 'utf8');
  const keep = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Config/EffectsPersistence.cpp', import.meta.url), 'utf8');
  // The Field of view slider used decimals: 1, which is how 100.5 showed up. Whole degrees only.
  assert.match(cameraTab, /label: 'Field of view', min: 5, max: 170, decimals: 0/);
  assert.match(cameraTab, /cmd\('mirv_fov ' \+ v\.toFixed\(0\)\)/);
  assert.doesNotMatch(cameraTab, /label: 'Field of view'[\s\S]{0,200}decimals: 1/,
    'shot FOV must not return to tenths of a degree');
  // Keep / CAMERA RESET must cover both shot FOV and the viewmodel override.
  assert.match(cameraTab, /effects reset camera/);
  assert.match(keep, /settings reset camFov/);
  assert.match(keep, /settings reset viewmodel/);
  assert.match(keep, /settings reset camFov/);
  assert.match(store, /WriteCamFov/);
  assert.match(store, /LoadCamFov/);
  assert.match(store, /BoolField\("camFovOn"/);
  assert.match(store, /IntField\("camFov"/);
});

test('every reset is registered so it can be driven without a mouse', () => {
  assert.match(hud, /api\.reset = function \(which\)/);
  assert.match(widgets, /function cfgRegisterReset\(name, resolve\)/);
  // The three groups the user asked to have working, plus the per-card environment resets.
  const names = ['camera', 'effects', 'ragdolls', 'environment', 'fx.vfx',
                 'env.sky', 'env.light', 'env.fog', 'env.looks', 'env.wx',
                 'cam.smoothview', 'cam.handheld', 'cam.weaponrecoil', 'cam.weaponbob', 'cam.viewmodel', 'cam.dof', 'cam.flare',
                 'fx.atmosphere', 'fx.lighting', 'fx.weathersky'];
  const all = [nav, hud, widgets, read('ConfigHudCameraTabJs.h'), read('ConfigHudAtmosphereTabJs.h'),
               read('ConfigHudLightingTabJs.h'), read('ConfigHudWeatherSkyTabJs.h'),
               read('ConfigHudEnvironmentTabJs.h'), read('ConfigHudEffectsResetJs.h'),
               read('ConfigHudRagdollsTabJs.h'), read('ConfigHudEnvironmentStateJs.h'), read('ConfigHudEffectsTabJs.h'), read('ConfigHudLensTabJs.h')].join('\n');
  for (const n of names) {
    assert.ok(all.includes("'" + n + "'"), 'reset "' + n + '" is not registered by name');
  }
});


test('tab footers execute the owning backend reset lists', () => {
  for (const [file, name] of [['ConfigHudCameraTabJs.h','camera'],
      ['ConfigHudEffectsResetJs.h','effects'],['ConfigHudRagdollsTabJs.h','ragdolls']]) {
    assert.ok(read(file).includes('mirv_filmmaker effects reset ' + name));
  }
  const footer = read('ConfigHudEffectsResetJs.h');
  assert.match(footer, /label:'EFFECTS'/);
  assert.match(footer, /for\(var key in fxResetLocal\)/);
  const keep = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Config/EffectsPersistence.cpp', import.meta.url),'utf8');
  const camera = keep.split('kCameraReset[] = {')[1].split('};')[0];
  for (const command of ['weaponbob reset','lens reset all','pivot recoil reset',
      'pivot bob reset','pivotorb recoil off','pivotorb bob off','xhair off'])
    assert.ok(camera.includes(command), 'Camera reset misses ' + command);
  const rag = keep.split('kRagdollReset[] = {')[1].split('};')[0];
  assert.ok(rag.includes('nervous reset'));
  const effects = keep.split('kEffectsReset[] = {')[1].split('};')[0];
  for (const command of ['fx off','light reset','sun reset','shadows reset','customshadow reset',
      'smoke appearance reset','modulate reset','sky default','skyorient reset','clouds reset'])
    assert.ok(effects.includes(command), 'Effects reset misses ' + command);
});

test('default tuning never disables Reset while a lens or bob switch is on', () => {
  const lens = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Lens/LensFxState.cpp', import.meta.url),'utf8');
  assert.match(lens, /g_values\[group == Group::Dof \? DofOn : FlareOn\] = 0/);
  assert.doesNotMatch(lens, /InGroup\(i, grp\) && i != DofOn/);
  const bob = fs.readFileSync(new URL('../../AfxHookSource2/Filmmaker/Movie/WeaponBob.cpp', import.meta.url),'utf8');
  assert.match(bob, /ResetSettings\(\) \{\s*m_enabled = false/);
  assert.match(bob, /return !m_enabled && m_amount/);
  assert.match(read('ConfigHudCameraTabJs.h'), /svReset.setDisabled\(!!st.smoothViewDefault && !st.fisheye\)/);
});
