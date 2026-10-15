// Screenshots the mockup at a true 1920x1080 through headless Chrome, one PNG per view, so a shot
// can be laid next to a capture of the real game pixel for pixel.
//
//   node UiMockup/capture-mock.mjs                     # every view in VIEWS
//   node UiMockup/capture-mock.mjs cameraeditor config # only these
//   node UiMockup/capture-mock.mjs --out some/dir --port 8823
//
// It serves ui-mockup itself, so nothing has to be running first. Chrome is found via CHROME_EXE
// or the usual install locations.
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { readFile, mkdir, rm, readdir, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import os from 'node:os';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..');

// name -> query string. The name is the PNG stem, which is also how a game capture is matched to it.
// Each entry is one thing worth looking at, not just one surface: the popups and the reported states
// are exactly the parts a static mockup gets wrong, so they each get their own shot.
const VIEWS = {
  // The VIEWMODEL card, which carries the camera Field of view alongside the weapon's own.
  // Added 2026-09-19 when the separate CAMERA FOV card was folded into it.
  camera_viewmodel: "surface=confighud&live=0&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card%28%27config.section.smoothview%27%2C0%29%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card%28%27config.section.handheld%27%2C0%29%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card%28%27config.section.weaponrecoil%27%2C0%29%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card%28%27config.section.weaponbob%27%2C0%29%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card%28%27config.section.viewmodel%27%2C1%29%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo%280%29",
  // Expanded inspector pages: spacing and mixed-control alignment regressions.
  smooth_breathing: "surface=confighud&live=0&precmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('config.section.smoothview'%2C1)&click=label:Breathing",
  smooth_tilt: "surface=confighud&live=0&precmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('config.section.smoothview'%2C1)&click=label:Tilt",
  sky_custom: "surface=confighud&live=0&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.gotoTab('effects')%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('fx.packs'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('weather.card'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  sky_stock: "surface=confighud&live=0&precmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.gotoTab('effects')%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('fx.packs'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('weather.card'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)&click=label:CS2",
  sky_orientation: "surface=confighud&live=0&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.gotoTab('effects')%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('fx.packs'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  sky_tint: "surface=confighud&live=0&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.gotoTab('effects')%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('fx.packs'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('weather.card'%2C2)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  sky_clouds: "surface=confighud&live=0&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.gotoTab('effects')%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('fx.packs'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('weather.card'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('weather.card'%2C3)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  // The boot state: what a person sees when the page (and the game) opens -- nothing of ours up.
  env_overview: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)",
  env_looks: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  env_color: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.looks'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  env_lens: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.looks'%2C2)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(0)",
  env_sky: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(78)",
  env_clouds: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.sky'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(78)",
  env_lighting: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(156)",
  env_fog: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(234)",
  env_weather: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(312)",
  env_ground: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.wx'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(312)",
  env_lightning: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.wx'%2C2)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(312)",
  // SCENE OBJECTS is one page (2026-10-10): the outliner with the selected card, then add a light.
  env_scene: "surface=confighud&click=label:ENVIRONMENT|label:SCENE%20OBJECTS&cmd=mirv_filmmaker%20props%20select%201%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(390)",
  // The same page further down: the asset library and its scrolling results, then the presets.
  env_scene_library: "surface=confighud&click=label:ENVIRONMENT|label:SCENE%20OBJECTS&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(1000)",
  env_prop: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.scene'%2C0)%3Bmirv_filmmaker%20environment%20select%201%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(390)",
  env_point: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.scene'%2C0)%3Bmirv_filmmaker%20environment%20select%202%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(390)",
  env_position: "surface=confighud&click=label:ENVIRONMENT&cmd=mirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.looks'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.sky'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.light'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.fog'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.wx'%2C0)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.card('env.scene'%2C1)%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.cardPage('env.scene'%2C0)%3Bmirv_filmmaker%20environment%20edit%20start%201%3Bmirv_filmmaker%20config%20eval%20%24.ConfigHud.scrollTo(390)",
  closed:                 'hold=300',
  confighud:              'surface=confighud',
  // The letterbox wordmark animation (WorkspaceShellJs.h), frozen at a loop time in seconds:
  // mid-streak, decelerating, landed, and whipping out.
  wordmark_streak:        'surface=confighud&cmd=' + encodeURIComponent('mirv_filmmaker config eval $.ConfigHud.wordmark(0.08)'),
  wordmark_settle:        'surface=confighud&cmd=' + encodeURIComponent('mirv_filmmaker config eval $.ConfigHud.wordmark(0.35)'),
  wordmark_rest:          'surface=confighud&cmd=' + encodeURIComponent('mirv_filmmaker config eval $.ConfigHud.wordmark(3)'),
  wordmark_out:           'surface=confighud&cmd=' + encodeURIComponent('mirv_filmmaker config eval $.ConfigHud.wordmark(7.9)'),
  confighud_effects:      'surface=confighud&click=label:EFFECTS',
  lighting_sun: 'surface=confighud&live=0&cmd='+encodeURIComponent("mirv_filmmaker config eval $.ConfigHud.gotoTab('effects');mirv_filmmaker environment climate relighting 0;mirv_filmmaker config eval $.ConfigHud.card('light.card',1);mirv_filmmaker config eval $.ConfigHud.cardPage('light.card',1);mirv_filmmaker config eval $.ConfigHud.scrollTo(430)"),
  lighting_owned: 'surface=confighud&live=0&cmd='+encodeURIComponent("mirv_filmmaker config eval $.ConfigHud.gotoTab('effects');mirv_filmmaker environment climate enabled 1;mirv_filmmaker environment climate relighting 1;mirv_filmmaker config eval $.ConfigHud.card('light.card',1);mirv_filmmaker config eval $.ConfigHud.cardPage('light.card',1);mirv_filmmaker config eval $.ConfigHud.scrollTo(430)"),
  confighud_environment:  'surface=confighud&click=label:ENVIRONMENT',
  confighud_env_open:     'surface=confighud&click=label:ENVIRONMENT|label:LIGHTING',
  confighud_ragdolls:     'surface=confighud&click=label:RAGDOLLS',
  confighud_reset:        'surface=confighud&act=Reset footer',
  cameraeditor:           'surface=cameraeditor',
  cameraeditor_gear:      'surface=cameraeditor&act=gear',
  cameraeditor_clearall:  'surface=cameraeditor&act=Clear All',
  cameraeditor_follow:    'surface=cameraeditor&act=Follow',
  render:                 'surface=render',
  render_running:         'surface=render&act=RUNNING',
  // The bottom editor stays available inside the render workspace -- verified in the running
  // game, where GRAPH docks the full curve editor under the render columns. It was suppressed
  // here, so GRAPH did nothing in Render.
  render_graph:           'surface=render&cmd=mirv_filmmaker editor curveeditor graph',
  // The ribbon's own verb, and the Render workspace's own tabs.
  editor_dock_graph:      'surface=cameraeditor&cmd=mirv_filmmaker dock graph',
  // Render v2 (three tabs): RANGE & CAMERA is slot 0, PASSES slot 2, OUTPUT slot 3.
  render_range_span:      'surface=render&cmd=' + encodeURIComponent('mirv_filmmaker render in 41800;mirv_filmmaker render out 43900'),
  render_tab_passes:      'surface=render&cmd=mirv_filmmaker render tab 2',
  render_tab_output:      'surface=render&cmd=mirv_filmmaker render tab 3',
  render_passes_view:     'surface=render&cmd=' + encodeURIComponent('mirv_filmmaker render tab 2;mirv_filmmaker render pass toggle depth world;mirv_filmmaker render pass preview world'),
  // F8: everything hidden, nothing closed.
  blackout:               'surface=cameraeditor&cmd=mirv_filmmaker blackout toggle',
  keyeditor:              'surface=keyeditor',
  customize:              'surface=customize&hold=1500',
  // Design 7b: the always-open picker on a grouped slot (knives, inside one model) and the agent pane.
  customize_knife:        'surface=customize&cmd=' + encodeURIComponent('mirv_filmmaker editor eval $.CamEditor.customizeOpenPicker(\x27knife\x27);mirv_filmmaker editor eval $.CamEditor.customizeOpenPicker(\x27knife\x27)') + '&hold=1500',
  customize_agent:        'surface=customize&cmd=' + encodeURIComponent('mirv_filmmaker editor eval $.CamEditor.customizeOpenPicker(\x27agent\x27);mirv_filmmaker editor eval $.CamEditor.customizeOpenPicker(\x27agent\x27)') + '&hold=1500',
  // Per-player death animations (design 8c): the picker with its first clip selected and playing,
  // then with a body-map region filtering the list.
  deathanim_picker:       'surface=confighud&act=Death clip preview&hold=2500',
  deathanim_area:         'surface=confighud&act=Death clip preview&cmd=' + encodeURIComponent('mirv_filmmaker config eval $.ConfigHudDap.area(\'torso\')') + '&hold=1500',
  cameratimeline:         'surface=cameratimeline',
  cameratimeline_shake:   'surface=cameratimeline&act=Shake',
  // Paused, like an editing session: this is the state where the seeded key window should stay
  // put rather than following a playhead that happens to be somewhere else in the demo.
  grapheditor:            'surface=grapheditor&live=0',
  // Same camera graph with the 100-tick time snap armed, so the snap grid and the lit SNAP pill
  // are on screen together.
  grapheditor_snap:       'surface=grapheditor&live=0&cmd=' + encodeURIComponent('mirv_filmmaker settings ui editor.graph.snap 100'),
  // SCENE GRAPH with real strips: the time-of-day macro arms a batch of properties and lays one
  // key on each, so the navigator has marks, the track list has several rows, and the overview /
  // scrollbar have something to map.
  grapheditor_scene:      'surface=grapheditor&cmd=' + encodeURIComponent(
    "mirv_filmmaker scenekeys macro golden;mirv_filmmaker settings ui editor.graph scene"),
  markerhud:              'surface=markerhud',
  markerhud_editing:      'surface=markerhud&act=editing',
  markerhud_need2:        'surface=markerhud&act=Need at least 2 markers',
  moviehud:               'surface=moviehud',
  demobar:                'surface=demobar',
  // LOOP only tints green once a range is armed (CameraTimelineJs.h patchNativeBar keys the
  // colour off loopOn AND loopReady), so the button looks inert until it has one. Both states
  // get a shot, because "the LOOP button does nothing" is what an unarmed LOOP correctly looks
  // like in game too, and only the pair shows that.
  confighud_loop_armed:   'surface=confighud&precmd=mirv_filmmaker loop range 41800 43900;mirv_filmmaker loop on',
  aimcrosshair:           'surface=aimcrosshair',
  fxdebug:                'surface=fxdebug',
  filmmakergui:           'surface=filmmakergui',
  downloaded_menu:       'surface=filmmakergui&aspect=1440&zoom=fit&click=pid:fm-tile-1',
  workshop_menu:         'surface=workshop&aspect=1440&zoom=fit',
  videoresolution:        'surface=videoresolution'
};

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg', '.svg': 'image/svg+xml', '.webm': 'video/webm', '.mp4': 'video/mp4',
  '.woff2': 'font/woff2', '.woff': 'font/woff', '.ttf': 'font/ttf', '.otf': 'font/otf'
};

function findChrome() {
  if (process.env.CHROME_EXE && existsSync(process.env.CHROME_EXE)) return process.env.CHROME_EXE;
  const c = [
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Microsoft/Edge/Application/msedge.exe'
  ];
  for (const p of c) if (existsSync(p)) return p;
  throw new Error('No Chrome/Edge found. Set CHROME_EXE to a browser executable.');
}

function serve(port) {
  const srv = createServer(async (req, res) => {
    const url = new URL(req.url, 'http://x');
    const rel = decodeURIComponent(url.pathname).replace(/^\/+/, '') || 'index.html';
    const file = path.join(HERE, rel);
    if (!file.startsWith(HERE)) { res.writeHead(403).end(); return; }
    try {
      const buf = await readFile(file);
      const st = await stat(file).catch(() => null);
      res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store',
        // The mockup polls this to reload a surface whose generated JS changed on disk.
        'last-modified': st ? st.mtime.toUTCString() : new Date(0).toUTCString() });
      res.end(buf);
    } catch { res.writeHead(404).end('not found'); }
  });
  return new Promise((ok, bad) => { srv.on('error', bad); srv.listen(port, '127.0.0.1', () => ok(srv)); });
}

// Chrome's --screenshot takes the shot when the page settles OR the virtual time budget runs out.
// The harness signals readiness through the title; --virtual-time-budget is the ceiling, not the
// plan, so it is set generously and the surfaces that finish early cost nothing.
function shoot(chrome, url, out, budgetMs) {
  const profile = path.join(os.tmpdir(), 'mvm-shot-' + Math.random().toString(36).slice(2));
  const args = [
    '--headless=new', '--disable-gpu', '--force-device-scale-factor=1',
    '--window-size=' + width + ',' + height, '--screenshot=' + out, '--virtual-time-budget=' + budgetMs,
    '--user-data-dir=' + profile, '--no-first-run', '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required', '--allow-file-access-from-files',
    '--disable-features=CalculateNativeWinOcclusion', url
  ];
  if (!/[?&]surface=(workshop|filmmakergui)(?:&|$)/.test(url)) args.splice(2, 0, '--hide-scrollbars');
  return new Promise((ok) => {
    const p = spawn(chrome, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => { err += d; });
    p.on('close', (code) => { rm(profile, { recursive: true, force: true }).catch(() => {}); ok({ code, err }); });
  });
}

const argv = process.argv.slice(2);
let outDir = path.join(ROOT, 'Automation/output/ui-compare/mock');
let port = 8899, budget = 9000, width = 1920, height = 1080;
const want = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--out') outDir = path.resolve(argv[++i]);
  else if (argv[i] === '--port') port = Number(argv[++i]);
  else if (argv[i] === '--budget') budget = Number(argv[++i]);
  else if (argv[i] === '--width') width = Number(argv[++i]);
  else if (argv[i] === '--height') height = Number(argv[++i]);
  else want.push(argv[i]);
}
const names = want.length ? want.filter((n) => { if (VIEWS[n]) return true; console.error('unknown view: ' + n); return false; }) : Object.keys(VIEWS);

// --serve just holds the no-store static server open, which is what you want when driving the page
// by hand or from a debugger: the plain python -m http.server caches app.js and quietly serves a
// stale copy after an edit.
if (argv.includes('--serve')) {
  await serve(port);
  console.log('serving ui-mockup (no-store) on http://127.0.0.1:' + port + '  ->  open /index.html');
  await new Promise(() => {});
}

const chrome = findChrome();
await mkdir(outDir, { recursive: true });
const srv = await serve(port);
console.log('serving ui-mockup on http://127.0.0.1:' + port + '  ->  ' + outDir);

let bad = 0;
for (const name of names) {
  const url = `http://127.0.0.1:${port}/index.html?bare=1&${VIEWS[name]}`;
  const out = path.join(outDir, name + '.png');
  const t0 = Date.now();
  const r = await shoot(chrome, url, out, budget);
  const ok = existsSync(out);
  if (!ok) bad++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name.padEnd(24)} ${String(Date.now() - t0).padStart(5)}ms  ${ok ? '' : r.err.split('\n').slice(-3).join(' ')}`);
}
srv.close();
const files = (await readdir(outDir)).filter((f) => f.endsWith('.png'));
console.log(`\n${files.length} PNG in ${outDir}${bad ? `  (${bad} failed)` : ''}`);
process.exit(bad ? 1 : 0);
