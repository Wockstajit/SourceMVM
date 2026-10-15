// The launcher's ReShade.ini / ReShade2.ini repair (dlss-tool.cjs mergeIni). Both inis must end up
// with neural rendering off at launch, with the add-on load line, whatever an older install or ReShade itself
// left in them: the old add-on's HookPoint/RequireDlss keys are dropped, and overlay tuning stays.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const require = createRequire(import.meta.url);
const { mergeIni } = require(path.join(root, 'SourceMovieMakerLauncher', 'electron', 'dlss-tool.cjs'));
const BIN = 'C:\\private\\game\\bin\\win64';
const template = fs.readFileSync(path.join(root, 'SourceMovieMakerLauncher', 'electron', 'dlss', 'ReShade.isolated.ini'), 'utf8')
  .split('{{BINDIR}}').join(BIN);

function section(text, name) {
  const lines = text.split(/\r?\n/);
  const start = lines.findIndex((l) => l.trim() === `[${name}]`);
  if (start < 0) return null;
  const out = [];
  for (let i = start + 1; i < lines.length && !/^\s*\[/.test(lines[i]); i += 1) out.push(lines[i]);
  return out;
}
const value = (text, name, key) => {
  const line = (section(text, name) || []).find((l) => l.startsWith(`${key}=`));
  return line === undefined ? undefined : line.slice(key.length + 1);
};

test('template starts neural rendering off; Present remains available in ReShade', () => {
  assert.equal(value(template, 'RENODX-DLSS', 'DirectNeuralRenderingHookStage'), '0');
  assert.equal(value(template, 'RENODX-DLSS', 'DirectNeuralRenderingHookPoint'), undefined);
});

test('a previously enabled install starts off again while other tuning survives', () => {
  const old = [
    '[ADDON]', 'LoadFromDllMain=C:\\old\\renodx-dlss.addon64', '',
    '[GENERAL]', 'PresetPath=.\\ReShadePreset.ini', '',
    '[RENODX-DLSS]', 'DirectNeuralRenderingHookPoint=3', 'DirectNeuralRenderingRequireDlss=0',
    'DirectNeuralRenderingHookStage=3', 'DirectNeuralRenderingIntensity=0.6', 'DLSSQualityMode=0', '',
    '[STYLE]', 'FontScale=1.500000', '',
  ].join('\r\n');
  const merged = mergeIni(old, template);
  assert.equal(value(merged, 'ADDON', 'LoadFromDllMain'), `${BIN}\\renodx-dlss.addon64`);
  assert.equal(value(merged, 'RENODX-DLSS', 'DirectNeuralRenderingHookStage'), '0');
  assert.equal(value(merged, 'RENODX-DLSS', 'DirectNeuralRenderingHookPoint'), undefined);
  assert.equal(value(merged, 'RENODX-DLSS', 'DirectNeuralRenderingRequireDlss'), undefined);
  assert.equal(value(merged, 'RENODX-DLSS', 'DirectNeuralRenderingIntensity'), '0.6', 'overlay tuning survives');
  assert.equal(value(merged, 'RENODX-DLSS', 'DLSSQualityMode'), '0');
  assert.equal(value(merged, 'STYLE', 'FontScale'), '1.500000');
  assert.equal(mergeIni(merged, template), merged, 'repair is idempotent');
});

test('a ReShade-written ini with no add-on sections gains them', () => {
  const merged = mergeIni('[GENERAL]\r\nNoDebugInfo=1\r\n', template);
  assert.equal(value(merged, 'GENERAL', 'NoDebugInfo'), '1');
  assert.equal(value(merged, 'ADDON', 'LoadFromDllMain'), `${BIN}\\renodx-dlss.addon64`);
  assert.equal(value(merged, 'RENODX-DLSS', 'DirectNeuralRenderingHookStage'), '0');
});
