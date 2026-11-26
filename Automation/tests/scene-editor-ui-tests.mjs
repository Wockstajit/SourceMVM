import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const repoRoot = path.resolve(import.meta.dirname, '..', '..');

function extractRawLiteral(filePath, marker) {
  const src = fs.readFileSync(path.join(repoRoot, filePath), 'utf8');
  const startRe = new RegExp(`R"${marker}\\(([\\s\\S]*?)\\)${marker}"`, 'g');
  const chunks = [];
  let m;
  while ((m = startRe.exec(src)) !== null) {
    chunks.push(m[1]);
  }
  assert.ok(chunks.length > 0, `Expected at least one R"${marker}(...) literal in ${filePath}`);
  return chunks.join('\n');
}

test('Scene Editor Panorama JS parses cleanly as strict ES5 script', () => {
  const header = extractRawLiteral('AfxHookSource2/Filmmaker/SceneEditor/SceneEditorJs.h', 'SEJS');
  const gizmo = extractRawLiteral('AfxHookSource2/Filmmaker/SceneEditor/SceneEditorGizmoJs.h', 'SEJS');
  const timeline = extractRawLiteral('AfxHookSource2/Filmmaker/SceneEditor/SceneEditorTimelineJs.h', 'SEJS');

  // Split kSceneEditorJsHeader / Body / Footer around Gizmo & Timeline fragments just like SceneEditorHud.cpp
  const jsSrc = fs.readFileSync(path.join(repoRoot, 'AfxHookSource2/Filmmaker/SceneEditor/SceneEditorJs.h'), 'utf8');
  const parts = [];
  const re = /R"SEJS\(([\s\S]*?)\)SEJS"/g;
  let match;
  while ((match = re.exec(jsSrc)) !== null) parts.push(match[1]);
  assert.equal(parts.length, 3, 'Expected Header, Body, and Footer literals in SceneEditorJs.h');

  const fullScript = [parts[0], gizmo, timeline, parts[1], parts[2]].join('\n');
  assert.ok(fullScript.includes('$.SceneEditor = api;'), 'SceneEditor exposes $.SceneEditor API');
  assert.ok(fullScript.includes("SetAttributeString('previewrect'"), 'SceneEditor publishes previewrect for WorkspacePreview scaling');
  assert.ok(fullScript.includes('seApplyHudViewport') && fullScript.includes('seRestoreHudViewport') && fullScript.includes("'AimXhairRoot'"), 'SceneEditor scales native CS2 HUD roots into preview window');
  assert.ok(!/\b(let|const)\s+/.test(fullScript), 'Must avoid ES6 let/const for CS2 Panorama V8 compat');
  assert.ok(!/=>/.test(fullScript), 'Must avoid ES6 arrow functions for CS2 Panorama V8 compat');
  assert.ok(!/`/.test(fullScript), 'Must avoid template literals for CS2 Panorama V8 compat');

  // Verify valid JS syntax via Node VM compilation
  new vm.Script(fullScript, { filename: 'SceneEditorBundle.js' });
});

test('Record & Play panel wires Open Scene Editor button', () => {
  const botsTab = fs.readFileSync(path.join(repoRoot, 'AfxHookSource2/Filmmaker/Panorama/ConfigHudBotsTabJs.h'), 'utf8');
  assert.ok(botsTab.includes('Open Scene Editor'), 'Missing Open Scene Editor button label');
  assert.ok(botsTab.includes('mirv_filmmaker sceneeditor on'), 'Missing mirv_filmmaker sceneeditor on command');
});
