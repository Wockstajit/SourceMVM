#!/usr/bin/env node
// Builds the Source 1 workspace mockup: the game's UI C++ (AfxHookSource/Filmmaker/Ui) and
// mockup-host.cpp compiled to WebAssembly with Emscripten, into build/ui-mockup-source/.
//
//   node UiMockupSource/build-mockup.mjs
//
// Emscripten is found through SOURCEMVM_EMSDK, then EMSDK, then %USERPROFILE%\emsdk. Verdana and
// Tahoma are read from this machine's Windows Fonts folder and packed into the build output, which
// stays out of git (they are Microsoft's fonts).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { digest } from '../Automation/lib/ui-source-sync.mjs';

const ROOT = path.resolve(import.meta.dirname, '..');
export const OUT = path.join(ROOT, 'build', 'ui-mockup-source');
const IMGUI = path.join(ROOT, 'Deps', 'release', 'imgui');
const FILMMAKER = path.join(ROOT, 'AfxHookSource', 'Filmmaker');
const UI = path.join(FILMMAKER, 'Ui');
const FONTS = ['verdana.ttf', 'verdanab.ttf', 'tahoma.ttf', 'tahomabd.ttf'];

// Everything the build reads, for the preview's change watcher.
export const SOURCE_DIRS = [UI, FILMMAKER, import.meta.dirname];
export const SHARED_HEADERS = [
  path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'Movie', 'UiClickMath.h'),
  path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'Movie', 'TimelineDock.h'),
  path.join(ROOT, 'AfxHookSource2', 'Filmmaker', 'Panorama', 'InspectorWidthMath.h'),
];

// ImGui itself changes only with the submodule, so its objects are kept between builds.
const IMGUI_SOURCES = ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
  path.join('backends', 'imgui_impl_opengl3.cpp')].map((f) => path.join(IMGUI, f));
const OWN_SOURCES = [path.join(import.meta.dirname, 'mockup-host.cpp'),
  ...['CameraEditorShell.cpp', 'DemoBar.cpp', 'UiStyle.cpp', 'UiWidgets.cpp'].map((f) => path.join(UI, f)),
  ...['SourceDemoBarModel.cpp', 'SourceDemoFile.cpp', 'SourceDemoMarkers.cpp'].map((f) => path.join(FILMMAKER, f))];

const EXPORTS = ['_main', '_malloc', '_free', '_mockup_mouse', '_mockup_motion', '_mockup_key',
  '_mockup_focus_lost', '_mockup_lock_lost', '_mockup_mode', '_mockup_resize', '_mockup_set_backdrop',
  '_mockup_command', '_mockup_state', '_mockup_step'];

export function findEmscripten() {
  const roots = [process.env.SOURCEMVM_EMSDK, process.env.EMSDK, path.join(os.homedir(), 'emsdk')].filter(Boolean);
  for (const root of roots) {
    for (const name of ['em++.exe', 'em++.bat', 'em++']) {
      const candidate = path.join(root, 'upstream', 'emscripten', name);
      if (fs.existsSync(candidate)) return candidate;
    }
  }
  throw new Error(`Emscripten not found (looked in ${roots.join(', ')}). Install emsdk or set SOURCEMVM_EMSDK.`);
}

function fontsFolder() {
  return path.join(process.env.WINDIR || 'C:\\Windows', 'Fonts');
}

function run(tool, args) {
  return new Promise((resolve, reject) => {
    execFile(tool, args, { cwd: ROOT, windowsHide: true, maxBuffer: 16 << 20 }, (error, stdout, stderr) => {
      if (error) reject(new Error(`${path.basename(args.find((a) => a.endsWith('.cpp')) || 'link')} failed:\n${stderr || stdout || error.message}`));
      else resolve();
    });
  });
}

const COMPILE_FLAGS = ['-std=c++17', '-O2', '-Wno-nontrivial-memaccess',
  '-DIMGUI_USER_CONFIG="Filmmaker/Ui/UiImConfig.h"', '-DIMGUI_IMPL_OPENGL_ES3',
  '-I', path.join(ROOT, 'AfxHookSource'), '-I', IMGUI, '-I', path.join(IMGUI, 'backends')];

function stampOf(files) {
  return digest(files.map((f) => `${f}:${fs.statSync(f).mtimeMs}`).join('\n'));
}

export async function build({ log = console.log } = {}) {
  const emxx = findEmscripten();
  const obj = path.join(OUT, 'obj');
  fs.mkdirSync(obj, { recursive: true });
  const started = Date.now();

  const imguiInputs = [...IMGUI_SOURCES, path.join(IMGUI, 'imgui.h'), path.join(IMGUI, 'imgui_internal.h'),
    path.join(UI, 'UiImConfig.h'), path.join(IMGUI, 'backends', 'imgui_impl_opengl3.h')];
  const imguiStampFile = path.join(obj, 'imgui.stamp');
  const imguiStamp = stampOf(imguiInputs);
  const objectOf = (src) => path.join(obj, path.basename(src, '.cpp') + '.o');
  const imguiFresh = fs.existsSync(imguiStampFile) && fs.readFileSync(imguiStampFile, 'utf8') === imguiStamp
    && IMGUI_SOURCES.every((s) => fs.existsSync(objectOf(s)));
  const compile = [...(imguiFresh ? [] : IMGUI_SOURCES), ...OWN_SOURCES];
  await Promise.all(compile.map((src) => run(emxx, [...COMPILE_FLAGS, '-c', src, '-o', objectOf(src)])));
  if (!imguiFresh) fs.writeFileSync(imguiStampFile, imguiStamp);

  const fonts = FONTS.map((f) => path.join(fontsFolder(), f)).filter((f) => fs.existsSync(f));
  if (fonts.length < FONTS.length) log(`Some CS:S UI fonts are missing from ${fontsFolder()}; the mockup falls back to ImGui's font for those.`);
  await run(emxx, [...[...IMGUI_SOURCES, ...OWN_SOURCES].map(objectOf),
    '-O2', '-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2', '-sALLOW_MEMORY_GROWTH=1', '-sENVIRONMENT=web',
    `-sEXPORTED_FUNCTIONS=${EXPORTS.join(',')}`, '-sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8',
    ...fonts.flatMap((f) => ['--preload-file', `${f}@/fonts/${path.basename(f).toLowerCase()}`]),
    '-o', path.join(OUT, 'mockup.js')]);

  const stamp = { built: new Date().toISOString(), ms: Date.now() - started, fonts: fonts.length };
  fs.writeFileSync(path.join(OUT, 'stamp.json'), JSON.stringify(stamp));
  log(`Source UI mockup built in ${(stamp.ms / 1000).toFixed(1)} s (${compile.length} files compiled).`);
  return stamp;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  build().catch((e) => { console.error(e.message); process.exitCode = 1; });
}
