#!/usr/bin/env node
// Drives the mockup the way a person would -- click a tab, press a button that hands over to another
// panel, drag the speed dial, stack two states -- and asserts what the game would show. Headless, so
// it runs without a browser pane and can gate a change to the shim or the router.
//
//   node UiMockup/verify-flow.mjs
//
// Each case is a URL for compare.js (?cmd= sends console commands through the real router,
// ?drag= feeds the stage's mouse pipe, ?act= presses sidebar buttons) plus an assertion over the
// settled DOM and state that compare.js parks in the page.
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, rm, mkdir, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.webm': 'video/webm', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf' };

function findChrome() {
  for (const p of [process.env.CHROME_EXE, 'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']) {
    if (p && existsSync(p)) return p;
  }
  throw new Error('No Chrome/Edge found. Set CHROME_EXE.');
}

function serve(port) {
  const srv = createServer(async (req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(HERE, rel);
    if (!file.startsWith(HERE)) { res.writeHead(403).end(); return; }
    if (process.argv.includes('--without-menu') && rel === 'shim/native-menu.js') {
      res.writeHead(200, { 'content-type': MIME['.js'] }).end('window.MvmNativeMenu={ready:async()=>{},tick:()=>{},commands:()=>[],model:()=>null};'); return;
    }
    if (process.argv.includes('--default-menu') && rel === 'generated/menu-fixture.json') { res.writeHead(404).end(); return; }
    let buf; try { buf = await readFile(file); } catch { res.writeHead(404).end(); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(buf);
  });
  return new Promise((ok, bad) => { srv.on('error', bad); srv.listen(port, '127.0.0.1', () => ok(srv)); });
}

function dumpDom(chrome, url) {
  const profile = path.join(os.tmpdir(), 'mvm-flow-' + Math.random().toString(36).slice(2));
  return new Promise((ok) => {
    const p = spawn(chrome, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--window-size=1920,1080', '--dump-dom',
      '--virtual-time-budget=24000', '--user-data-dir=' + profile, '--no-first-run', url], { stdio: ['ignore', 'pipe', 'pipe'] });
    let out = ''; p.stdout.on('data', (d) => { out += d; });
    p.on('close', () => { rm(profile, { recursive: true, force: true }).catch(() => {}); ok(out); });
  });
}

function unescapeHtml(s) { return s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&amp;/g, '&'); }
function parsePre(html, id) {
  const m = new RegExp('<pre id="' + id + '"[^>]*>([\\s\\S]*?)<\\/pre>').exec(html);
  if (!m) return null;
  try { return JSON.parse(unescapeHtml(m[1])); } catch { return null; }
}
// A panel's visibility from the geometry dump: present, not display:none, and laid out.
// A panel's rendered text, from the geometry dump.
// A control's rendered text. A button's caption lives on a child Label, not on the button panel,
// so this gathers the whole subtree: the dump is a pre-order walk, so the subtree is the run of
// following rows with a greater depth.
function textOf(geom, pid) {
  for (const rows of Object.values(geom.roots || {})) {
    if (!rows) continue;
    const i = rows.findIndex((x) => x.id === pid);
    if (i < 0) continue;
    let t = rows[i].t || '';
    for (let k = i + 1; k < rows.length && rows[k].d > rows[i].d; k++) t += rows[k].t || '';
    return t.trim();
  }
  return null;
}
// Every root that is laid out and visible on this surface.
function liveRoots(geom) {
  return Object.entries(geom.roots || {})
    .filter(([, rows]) => rows && rows[0] && rows[0].v === 1 && rows[0].w > 0)
    .map(([id]) => id);
}

function shown(geom, pid) {
  for (const rows of Object.values(geom.roots || {})) {
    if (!rows) continue;
    const r = rows.find((x) => x.id === pid);
    if (r) return r.v === 1 && r.w > 0 && r.h > 0;
  }
  return false;
}


const CASES = [
  { name: 'Environment: creation selects a stable ID and opens its editor',
    q: 'surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker environment add point',
    check: (g,s) => s.state.envSelected > 3 && s.state.environment.objects.some(o=>o.id===s.state.envSelected&&o.kind==='point')
      && shown(g,'EnvPositionOverlay') },
  { name: 'Environment: edit position displays axes without moving the camera',
    q: 'surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker environment edit start 1',
    check: (g,s) => s.state.envEditing===1 && shown(g,'EnvPositionOverlay') && !s.commands.some(c=>/camera.*(pos|ang)/.test(c)) },
  { name: 'Environment: close cancels an uncommitted position edit',
    q: 'surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker environment edit start 1;mirv_filmmaker config close',
    check: (g,s) => !s.state.envEditing && !shown(g,'ConfigHudRoot') },

  { name: 'source reload rebuilds the open camera editor and graph',
    q: 'surface=grapheditor&reload=1',
    check: (g, s) => shown(g, 'CamEditorRoot') && shown(g, 'GraphExpRoot')
      && s.apis.includes('CamEditor') && s.apis.includes('GraphExp') },
  { name: 'source reload preserves the open Render workspace',
    q: 'surface=render&reload=1',
    check: (g, s) => s.state.nav.renderOpen === true && shown(g, 'CamEditorRenderRoot') },
  { name: 'source reload rebuilds the open Config panel',
    q: 'surface=confighud&reload=1',
    check: (g, s) => shown(g, 'ConfigHudRoot') && s.apis.includes('ConfigHud') },
  { name: 'Config: CAM EDITOR is hit-testable and swaps to editor CONFIG',
    q: 'surface=confighud&click=pid:ModeSwapBtn',
    check: (g, s) => s.surface === 'cameraeditor' && s.ui.modeSwapText === 'CONFIG' && shown(g, 'CamEditorRoot') },
  { name: 'Config: Handheld child toggle does not fire the master toggle',
    q: 'surface=confighud&precmd=mirv_filmmaker settings ui config.tab camera;mirv_filmmaker settings ui config.section.handheld 1;mirv_filmmaker handheld on;mirv_filmmaker handheld viewmodel on&click=label:Decouple Viewmodel',
    check: (g, s) => s.state.handheldOn === true && s.state.hhVmDecoupled === false
      && s.commands.includes('mirv_filmmaker handheld viewmodel toggle')
      && !s.commands.includes('mirv_filmmaker handheld toggle') },
  { name: 'Panorama surface text cannot be selected',
    q: 'surface=confighud',
    check: (g, s) => s.ui.stageUserSelect === 'none' },
  { name: 'native bar: LOOP is hit-testable and toggles playback looping',
    q: 'surface=confighud&precmd=mirv_filmmaker loop off&click=pid:LoopBtn',
    check: (g, s) => s.state.loopOn === true
      && s.commands.includes('mirv_filmmaker loop toggle quiet') },
  { name: 'Config: collapsed Handheld card survives editor round-trip',
    q: 'surface=confighud&click=label:HANDHELD CAMERA&cmd=mirv_filmmaker editor on;mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && s.state.uiConfigHandheldOpen === false },
  { name: 'editor: TIMELINE tab docks the unified timeline',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock timeline',
    check: (g, s) => s.state.dock.tab === 'timeline' && shown(g, 'GraphExpRoot') && !shown(g, 'CamTimelineBar') },
  // Camera and scene keys share the dock: armed scene properties appear as layers NEXT TO the
  // camera path, and opening the SCENE KEYS inspector tab takes nothing away.
  { name: 'the timeline shows scene layers next to the camera path',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker settings ui editor.inspector scene;mirv_filmmaker scenekeys strip exposure on;mirv_filmmaker dock timeline',
    check: (g, s) => s.state.dock.tab === 'timeline' && shown(g, 'GraphExpRoot')
      && Array.isArray(s.state.channels) && s.state.channels.some((c) => c.name === 'Pos X')
      && Array.isArray(s.state.sceneKeys.sceneChannels) && s.state.sceneKeys.sceneChannels.some((c) => c.fieldId === 'exposure') },
  { name: 'editor: REGULAR TIMELINE brings the native bar back',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock timeline;mirv_filmmaker dock native',
    // the timeline bar stays laid out under the native bar in game too (see the geometry dumps), so
    // the assertion is the mode plus the native controls being back, not the bar being gone
    check: (g, s) => s.state.dock.tab === 'native' && shown(g, 'ControlRow') },
  { name: 'the floating camera timeline opens with no editor around it',
    q: 'surface=cameratimeline',
    check: (g, s) => !shown(g, 'CamEditorRoot') && shown(g, 'CamTimelineBar') && s.state.dock.tlVisible === true },
  { name: 'Timeline row opens inside the editor (TIMELINE tab up)',
    q: 'surface=grapheditor',
    check: (g, s) => shown(g, 'CamEditorRoot') && shown(g, 'GraphExpRoot') },
  // Config HOSTS the dock and shows the remembered tab, so its own REGULAR TIMELINE / TIMELINE
  // tabs actually switch it. (It used to lose the one-shot claim when opened from the editor and
  // then never re-claim, leaving the ribbon's tabs doing nothing but repainting.)
  { name: 'Config hosts the remembered TIMELINE tab, so its tabs are live',
    q: 'surface=grapheditor&cmd=mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && shown(g, 'GraphExpRoot')
      && shown(g, 'ConfigHudRoot') && s.ui.modeSwapText === 'CAM EDITOR' },
  { name: 'the editor gets its docked timeline back after a trip through Config',
    q: 'surface=grapheditor&cmd=mirv_filmmaker config on;mirv_filmmaker editor on',
    check: (g, s) => s.surface === 'cameraeditor' && shown(g, 'GraphExpRoot')
      && s.state.dock.tab === 'timeline' },
  // The render workspace is a modal ON the editor, so it belongs to one row and has to let go.
  // The TIMELINE dock stays available under the render columns, and '✕ Back' sends
  // 'render close' to leave.
  { name: 'Render: TIMELINE docks inside the render workspace',
    q: 'surface=render&cmd=mirv_filmmaker dock timeline',
    check: (g, s) => s.surface === 'render' && shown(g, 'GraphExpRoot')
      && shown(g, 'CamEditorRenderRoot') && s.state.dock.tab === 'timeline' },
  { name: 'Render: Back leaves the render workspace for the editor',
    q: 'surface=render&cmd=mirv_filmmaker render close',
    check: (g, s) => s.surface === 'cameraeditor' && !shown(g, 'CamEditorRenderRoot')
      && shown(g, 'CamEditorRoot') && s.state.nav.renderOpen === false },
  { name: 'the editor does not come up as the render workspace once Render has been open',
    q: 'surface=render&cmd=mirv_filmmaker render close;mirv_filmmaker dock timeline',
    check: (g, s) => s.surface === 'cameraeditor' && !shown(g, 'CamEditorRenderRoot')
      && shown(g, 'GraphExpRoot') },
  // The graph draws from state.channels. They were absent from the captured state (the graph was
  // shut when it was taken), so the editor drew an empty grid with no channel names.
  // Not pinned to a channel count: generated/ is gitignored, so a fresh clone runs on the
  // built-in stand-in state (2 channels) until someone captures from the game (7).
  { name: 'the graph editor has camera path curves to draw',
    q: 'surface=grapheditor',
    check: (g, s) => shown(g, 'GraphExpRoot')
      && Array.isArray(s.state.channels) && s.state.channels.length >= 2
      && s.state.channels.every((c) => (c.keys || []).length > 0) },
  { name: 'Config: RENDER hands over to the render workspace',
    q: 'surface=confighud&cmd=mirv_filmmaker editor render',
    check: (g, s) => s.surface === 'render' && shown(g, 'CamEditorRenderRoot') },
  { name: 'Config: CUSTOMIZE PLAYER hands over to the customize modal',
    q: 'surface=confighud&cmd=mirv_filmmaker editor customize&hold=800',
    check: (g, s) => s.surface === 'customize' },
  { name: 'Config: Exit drops back to playback',
    q: 'surface=confighud&cmd=mirv_filmmaker config close',
    check: (g, s) => s.surface === 'demobar' },
  { name: 'demo bar: CONFIG opens Config',
    q: 'surface=demobar&cmd=mirv_filmmaker config toggle',
    check: (g, s) => s.surface === 'confighud' && shown(g, 'ConfigHudRoot') },
  { name: 'demo bar: CAM EDITOR opens the editor',
    q: 'surface=demobar&cmd=mirv_filmmaker editor toggle',
    check: (g, s) => s.surface === 'cameraeditor' && shown(g, 'CamEditorRoot') },
  { name: 'editor: Exit drops back to playback',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker editor off',
    check: (g, s) => s.surface === 'demobar' },
  { name: 'speed dial: dragging changes the demo speed',
    q: 'surface=demobar&drag=MirvSpeedRow,70',
    check: (g, s) => typeof s.state.speed === 'number' && Math.abs(s.state.speed - 1) > 0.05 },
  { name: 'states stack: two at once, both applied',
    q: 'surface=cameraeditor&act=No POV (free roam)|Mouse: GAME',
    check: (g, s) => s.state.povReady === false && s.state.cursor === false },
  { name: 'states toggle: pressing one again undoes only that one',
    q: 'surface=cameraeditor&act=No POV (free roam)|Mouse: GAME|Mouse: GAME',
    check: (g, s) => s.state.povReady === false && s.state.cursor === true },
  // The demo bar's one swap button: it names the workspace you are NOT in, and takes you there.
  // This is the button the whole CONFIG <-> CAM EDITOR round trip hangs off.
  { name: "Config bar offers CAM EDITOR (not CONFIG)",
    q: 'surface=confighud',
    check: (g) => textOf(g, 'ModeSwapBtn') === 'CAM EDITOR' },
  { name: "the editor bar offers CONFIG",
    q: 'surface=cameraeditor',
    check: (g) => textOf(g, 'ModeSwapBtn') === 'CONFIG' },
  { name: 'Config -> CAM EDITOR button lands in the editor',
    q: 'surface=confighud&cmd=mirv_filmmaker editor on',
    check: (g, s) => s.surface === 'cameraeditor' && textOf(g, 'ModeSwapBtn') === 'CONFIG' },
  { name: 'editor -> CONFIG button lands in Config',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && textOf(g, 'ModeSwapBtn') === 'CAM EDITOR' },

  // Only ONE workspace root is ever up at a time. Two full-screen panels stacked is what made
  // half the controls unclickable and put Config's tabs under the editor's.
  { name: 'Config shows only the Config panel, not the editor too',
    q: 'surface=confighud',
    check: (g) => { const r = liveRoots(g); return r.includes('ConfigHudRoot') && !r.includes('CamEditorRoot'); } },
  { name: 'the editor shows only the editor, not Config too',
    q: 'surface=cameraeditor',
    check: (g) => { const r = liveRoots(g); return r.includes('CamEditorRoot') && !r.includes('ConfigHudRoot'); } },

  // State outlives a surface switch: shut a section in Config, go to the editor and back, and it
  // is still shut. It used to be rebuilt from scratch, so everything sprang open again.
  { name: 'a closed section stays closed across a surface switch',
    q: 'surface=confighud&cmd=mirv_filmmaker settings ui config.section.smoothview 0;'
       + 'mirv_filmmaker editor on;mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && s.state.ui['config.section.smoothview'] === '0' },

  // ---- one persistent context, booted the way the game boots -------------------------------
  // No ?surface=: the page comes up the way CS2 does after a demo loads. Nothing of ours open,
  // the native demo bar with CAM EDITOR / CONFIG, the director card, and no workspace root built.
  { name: 'boot lands closed: demo bar + director card, no workspace built',
    q: '',
    check: (g, s) => s.surface === 'demobar' && shown(g, 'ControlRow') && shown(g, 'MovieHudRoot')
      && shown(g, 'CamEditorBtn') && shown(g, 'ConfigBtn')
      && !liveRoots(g).includes('CamEditorRoot') && !liveRoots(g).includes('ConfigHudRoot')
      && !s.apis.includes('CamEditor') && !s.apis.includes('ConfigHud') },
  // The dock ribbon's own tabs send `mirv_filmmaker dock <tab>` (TimelineDockRibbonJs.h).
  { name: 'ribbon: dock toggle flips REGULAR TIMELINE <-> TIMELINE (TimelineDock_Pick)',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock native;mirv_filmmaker dock toggle',
    check: (g, s) => s.state.dock.tab === 'timeline' && shown(g, 'GraphExpRoot') },
  { name: 'ribbon: dock toggle again lands on the native bar',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock timeline;mirv_filmmaker dock toggle',
    check: (g, s) => s.state.dock.tab === 'native' && !shown(g, 'GraphExpRoot') },
  { name: 'the old tab names are gone: dock graph changes nothing',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock native;mirv_filmmaker dock graph',
    check: (g, s) => s.state.dock.tab === 'native' },
  // The editor's own RENDER button sends `render open` (CameraEditorRenderJs.h openRender);
  // the mockup used to route it into state and then overwrite that state, so nothing happened.
  { name: 'editor: RENDER (render open) opens the render workspace',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker render open',
    check: (g, s) => s.surface === 'render' && shown(g, 'CamEditorRenderRoot') },
  { name: 'render open outside the editor is refused, as RenderCommand.cpp refuses it',
    q: 'surface=demobar&cmd=mirv_filmmaker render open',
    check: (g, s) => s.surface === 'demobar' && s.state.nav.renderOpen === false && !liveRoots(g).includes('CamEditorRenderRoot') },
  // The editor's ESC EXIT sends `editor close` (CameraEditorCommand.cpp treats off/close/0 alike).
  { name: 'editor: ESC EXIT (editor close) tears the editor down',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker editor close',
    check: (g, s) => s.surface === 'demobar' && !liveRoots(g).includes('CamEditorRoot')
      && !s.apis.includes('CamEditor') && shown(g, 'CamEditorBtn') },
  // Customize Player commits: Apply writes the override, the next state frame reports it (the
  // modal reopens on it), and Reset to default brings the recorded loadout back. Built-in state so
  // the target is known (donk, AK-47 paint 675).
  { name: 'customize: Apply is reported back on the player and the spectated target',
    q: 'surface=customize&live=0&cmd=mirv_filmmaker cosmetics player current weapon 7 paint 801 wear 0.2000 seed 0;'
      + 'mirv_filmmaker cosmetics player current agent agents/models/tm_leet/tm_leet_varianta.vmdl',
    check: (g, s) => s.state.customizeTarget.weapons.primary.paintKit === 801
      && s.state.customizePlayers['3'].agentModel === 'agents/models/tm_leet/tm_leet_varianta.vmdl' },
  { name: 'customize: Reset to default (clearPlayer) restores the recorded loadout',
    q: 'surface=customize&live=0&cmd=mirv_filmmaker cosmetics player current weapon 7 paint 801 wear 0.2000 seed 0;'
      + 'mirv_filmmaker cosmetics clearPlayer 76561199063068840',
    check: (g, s) => s.state.customizeTarget.weapons.primary.paintKit === 675
      && s.state.customizePlayers['3'].weapons.primary.paintKit === 675 },
  // Per-player death animations: the picker lists the demo's players, the catalog loads from the
  // shipped packs, and Assign / Use automatic pick write the player's own clip.
  { name: 'death animations: the picker opens over Config with the full clip catalog',
    q: 'surface=confighud&live=0&act=Per-player death animations&hold=2500',
    check: (g, s) => {
      const text = Object.values(g.roots || {}).flat().filter(Boolean).map((r) => r.t || '');
      return s.surface === 'confighud' && s.commands.includes('mirv_filmmaker deathanim catalog')
        && text.some((t) => /of 449 clips/.test(t)) && text.includes('donk') && text.includes('m0NESY');
    } },
  { name: 'death animations: Assign gives the player their own clip, Automatic clears it',
    q: 'surface=confighud&live=0&cmd=mirv_filmmaker deathanim player s76561199063068840 Death_05;'
      + 'mirv_filmmaker deathanim player s76561198386265483 Death_08;mirv_filmmaker deathanim player s76561198386265483 auto',
    check: (g, s) => s.state.daPlayerClips && s.state.daPlayerClips.s76561199063068840 === 'Death_05'
      && !('s76561198386265483' in s.state.daPlayerClips) },
  { name: 'the Render workspace tabs (render tab N) switch',
    q: 'surface=render&cmd=mirv_filmmaker render tab 2',
    check: (g, s) => s.surface === 'render' && s.state.render.tab === 2 },
  // Config hosts the dock whether it was opened from playback OR from the editor now: both
  // workspaces re-claim every frame, so the open-from-editor race self-heals instead of leaving
  // Config with a ribbon whose tabs do nothing.
  { name: 'Config opened from playback hosts the remembered TIMELINE tab',
    q: 'surface=demobar&cmd=mirv_filmmaker dock timeline;mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && shown(g, 'GraphExpRoot') && s.state.dock.host === 'config' },
  { name: 'Config opened from the editor hosts the dock and its tabs work',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock timeline;mirv_filmmaker config on',
    check: (g, s) => s.surface === 'confighud' && shown(g, 'GraphExpRoot') && s.state.dock.host === 'config'
      && s.state.dock.tab === 'timeline' },
  // F8 is a render-time flag: everything hides, nothing closes, the second press restores.
  { name: 'F8 blackout hides the editor, the bar and the card without closing anything',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker blackout toggle',
    check: (g, s) => s.state.nav.blackout === true && s.state.nav.editorOpen === true
      && !shown(g, 'CamEditorRoot') && !shown(g, 'ControlRow') && !shown(g, 'MovieHudRoot') },
  { name: 'a second F8 puts the editor back exactly as it was',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker dock timeline;mirv_filmmaker blackout toggle;mirv_filmmaker blackout toggle',
    check: (g, s) => s.state.nav.blackout === false && shown(g, 'CamEditorRoot') && shown(g, 'GraphExpRoot') },
  // Teardown really deletes: after a round trip the roots are gone and the api is null, as in
  // CameraEditorHud::Teardown.
  { name: 'closing the editor deletes its roots and nulls $.CamEditor',
    q: 'surface=render&cmd=mirv_filmmaker editor off',
    check: (g, s) => !liveRoots(g).includes('CamEditorRoot') && !liveRoots(g).includes('CamEditorRenderRoot')
      && !s.apis.includes('CamEditor') && !s.apis.includes('GraphExp') },
  { name: 'the director card comes back when the editor closes',
    q: 'surface=cameraeditor&cmd=mirv_filmmaker editor off',
    check: (g, s) => s.surface === 'demobar' && shown(g, 'MovieHudRoot') },
  // The docked timeline's option rows. Each one sends a `marker …` verb (MarkerCommand.cpp) and
  // lights itself from the NAME CameraPath publishes back; unrouted, the click registered and the
  // highlight never moved -- "I can't click the timeline options".
  { name: 'timeline: Constant speed mode lights up when clicked',
    q: 'surface=cameratimeline&click=label:Constant',
    check: (g, s) => s.state.speedMode === 'Constant' && s.commands.includes('mirv_filmmaker marker speedmode constant') },
  { name: 'timeline: Per-Seg speed mode lights up when clicked',
    q: 'surface=cameratimeline&click=label:Per-Seg',
    check: (g, s) => s.state.speedMode === 'Per-Segment' },
  { name: 'timeline: Freeze timing lights up when clicked',
    q: 'surface=cameratimeline&click=label:Freeze',
    check: (g, s) => s.state.timing === 'Freeze' && s.commands.includes('mirv_filmmaker marker timing freeze') },
  { name: 'timeline: Live timing lights up when clicked after Freeze',
    q: 'surface=cameratimeline&click=label:Freeze|label:Live',
    check: (g, s) => s.state.timing === 'Live' },
  { name: 'nothing the timeline sends is left unrouted',
    q: 'surface=cameratimeline&click=label:Constant|label:Freeze|label:Linear|label:∿ Smooth',
    check: (g, s) => Object.keys(s.unrouted || {}).length === 0 },
  { name: 'the render workspace tabs switch their page when clicked',
    q: 'surface=render&click=label:HUD',
    check: (g, s) => s.state.render.tab === 2 && shown(g, 'CamEditorRenderRoot') },
  { name: 'the workspaces are mutually exclusive both ways',
    q: 'surface=demobar&cmd=mirv_filmmaker config on;mirv_filmmaker editor on;mirv_filmmaker config on',
    check: (g, s) => s.state.nav.configOpen === true && s.state.nav.editorOpen === false
      && liveRoots(g).includes('ConfigHudRoot') && !liveRoots(g).includes('CamEditorRoot') },
];

const chrome = findChrome();
const menuFixturePath = path.join(HERE, 'generated/menu-fixture.json');
const menuFixture = JSON.parse(await readFile(!process.argv.includes('--default-menu') && existsSync(menuFixturePath) ? menuFixturePath : path.join(HERE, 'fixtures/menu-default.json'), 'utf8'));
const menuPlayer = menuFixture.demos.demos.find(d => d.index === 1).players.filter(p => p.team === 0).sort((a,b) => b.score-a.score)[1].name;
const menuMap = menuFixture.maps.find(m => menuFixture.tiles.some(t => String(t.id) === String(m.workshop_id) && t.modes.includes('deathmatch'))).name;
CASES.push(
  { name: 'menu: selecting a downloaded demo fills its native scoreboard', q: 'surface=filmmakergui&aspect=1440&click=pid:fm-tile-1', check: (g,s) => shown(g,'players-table-CT') && s.ui.menu.demos.demos.some(d=>d.index===1 && /anubis/.test(d.map)) && s.apis.includes('Filmmaker') },
  { name: 'menu: clicking a player updates the round performance title', q: 'surface=filmmakergui&click=pid:fm-tile-1|label:' + encodeURIComponent(menuPlayer), check: (g,s) => textOf(g,'id-mi-player-stats-title').includes(menuPlayer) },
  { name: 'menu: delete confirms and removes only one fixture demo', q: 'surface=filmmakergui&click=pid:fm-tile-1|pid:id-mi-delete|pid:NativeMenuConfirmOkay', check: (g,s) => s.ui.menuCommands.includes('hide 1') && !s.ui.menu.demos.demos.some(d=>d.index===1) },
  { name: 'menu: clear all confirms and displays the empty state', q: 'surface=filmmakergui&click=pid:FmClearAllBtn|pid:NativeMenuConfirmOkay', check: (g,s) => s.ui.menu.demos.demos.length===0 && shown(g,'FmEmptyMsg') },
  { name: 'menu: cancel keeps fixture demos', q: 'surface=filmmakergui&click=pid:FmClearAllBtn|pid:NativeMenuConfirmCancel', check: (g,s) => s.ui.menu.demos.demos.length>0 && !s.ui.menuCommands.includes('clearall') },
  { name: 'menu: hide-training control reaches the simulated host', q: 'surface=workshop&click=pid:MvmWsHide', check: (g,s) => s.ui.menuCommands.some(c=>/^ws hide [01]$/.test(c)) && shown(g,'MvmWorkshopFilters') },
  { name: 'menu: Workshop GO offers native game mode choices', q: 'surface=workshop&click=label:' + encodeURIComponent(menuMap) + '|pid:WorkshopGo', check: (g,s) => shown(g,'WorkshopModePopup') && shown(g,'WorkshopMode-deathmatch') && shown(g,'CancelButton') }
);
const port = Number(process.env.MVM_PORT || 8905);
const srv = await serve(port);
let failed = 0;
const selectedCases = process.argv[2] ? CASES.filter(c => c.name.includes(process.argv[2])) : CASES;
for (const c of selectedCases) {
  const html = await dumpDom(chrome, `http://127.0.0.1:${port}/index.html?bare=1&geom=1&watch=0&${c.q}`);
  const geom = parsePre(html, 'mvm-geom'), st = parsePre(html, 'mvm-state');
  let ok = false, why = '';
  if (!geom || !st) why = 'page never signalled ready';
  else { try { ok = !!c.check(geom, st); } catch (e) { why = String(e); } }
  if (!ok && !why) why = `surface=${st && st.surface} dock=${st && st.state.dock && st.state.dock.tab} speed=${st && st.state.speed} log=${st && JSON.stringify(st.log)}`;
  if (!ok) {
    failed++;
    const dir = path.resolve(HERE, '../Automation/output/ui-flow'); await mkdir(dir, {recursive:true});
    await writeFile(path.join(dir,c.name.replace(/[^a-z0-9]+/gi,'-')+'.html'),html);
  }
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${c.name}${ok ? '' : '\n       ' + why}`);
}
srv.close();
console.log(failed ? `\n${failed} of ${selectedCases.length} failed` : `\nall ${selectedCases.length} flows behave like the game`);
process.exit(failed ? 1 : 0);
