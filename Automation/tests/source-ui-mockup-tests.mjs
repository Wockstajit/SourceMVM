// The CS:S UI mockup (UiMockupSource) must run the game's UI code, not a copy of it. These checks
// pin that: the mockup build compiles every engine-free UI source the hook compiles, the browser
// host takes its mouse and key decisions from the shared headers, and the page only calls what the
// WebAssembly exports. Static, runs without either the game or Emscripten.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (path) => fs.readFileSync(new URL(path, import.meta.url), 'utf8');
const cmake = read('../../AfxHookSource/CMakeLists.txt');
const build = read('../../UiMockupSource/build-mockup.mjs');
const host = read('../../UiMockupSource/mockup-host.cpp');
const page = read('../../UiMockupSource/index.html');

test('the mockup compiles every UI source the hook compiles, except the D3D9 renderer', () => {
  const hookUi = [...cmake.matchAll(/Filmmaker\/Ui\/(\w+)\.cpp/g)].map((m) => m[1]);
  assert.ok(hookUi.includes('CameraEditorShell'), 'the hook lists its UI sources');
  for (const name of hookUi) {
    if (name === 'UiRenderDx9') continue; // the mockup draws through WebGL instead
    assert.match(build, new RegExp(`'${name}\\.cpp'`), `build-mockup.mjs compiles ${name}.cpp`);
  }
});

test('the mockup uses the hook ImGui configuration', () => {
  assert.match(cmake, /IMGUI_USER_CONFIG="Filmmaker\/Ui\/UiImConfig\.h"/);
  assert.match(build, /-DIMGUI_USER_CONFIG="Filmmaker\/Ui\/UiImConfig\.h"/);
});

test('the browser host decides mouse and keys through the shared headers', () => {
  assert.match(host, /#include "\.\.\/AfxHookSource\/Filmmaker\/Ui\/WorkspaceMouse\.h"/);
  assert.match(host, /#include "\.\.\/AfxHookSource\/Filmmaker\/Ui\/UiKeys\.h"/);
  assert.match(host, /WorkspaceKeyFor\(/);
  for (const rule of ['ClickSwitchesToGameMode', 'RightPressBecomesLook', 'kRightLookHoldMs']) {
    assert.doesNotMatch(host, new RegExp(rule), `mockup-host.cpp leaves ${rule} to WorkspaceMouse.h`);
    assert.doesNotMatch(page, new RegExp(rule), `index.html leaves ${rule} to WorkspaceMouse.h`);
  }
  // The page forwards keys; it does not decide what Esc or G do.
  assert.doesNotMatch(page, /keyCode\s*===?\s*(?:27|71)\b/);
});

test('the page calls only functions the WebAssembly exports', () => {
  const exported = new Set([...build.matchAll(/'(_\w+)'/g)].map((m) => m[1]));
  const defined = new Set([...host.matchAll(/EMSCRIPTEN_KEEPALIVE [\w\s*]+?\b(mockup_\w+)\(/g)].map((m) => '_' + m[1]));
  for (const name of defined) assert.ok(exported.has(name), `${name} is exported`);
  const called = new Set([...page.matchAll(/Module\.(_mockup_\w+)/g), ...page.matchAll(/ccall\('(mockup_\w+)'/g)]
    .map((m) => (m[1].startsWith('_') ? m[1] : '_' + m[1])));
  assert.ok(called.size > 0);
  for (const name of called) assert.ok(defined.has(name) && exported.has(name), `${name} exists and is exported`);
});

test('the game host and the mockup host share the key rules', () => {
  const game = read('../../AfxHookSource/Filmmaker/SourceUi.cpp');
  assert.match(game, /WorkspaceKeyFor\(/);
  assert.match(game, /FeedImGuiKey\(/);
  assert.doesNotMatch(game, /wParam == VK_ESCAPE|wParam == 'G'/);
});
