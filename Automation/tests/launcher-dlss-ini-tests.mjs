// The launcher's ReShade.ini / ReShade2.ini repair (dlss-tool.cjs mergeIni). Both inis must end up
// with neural rendering off at launch, with the add-on load line, whatever an older install or ReShade itself
// left in them: the old add-on's HookPoint/RequireDlss keys are dropped, and overlay tuning stays.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const require = createRequire(import.meta.url);
const tool = require(path.join(root, 'SourceMovieMakerLauncher', 'electron', 'dlss-tool.cjs'));
const { mergeIni } = tool;
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

test('search paths reach the reshade-shaders folder and keep what the user added', () => {
  const old = ['[GENERAL]', 'EffectSearchPaths=.\\,D:\\Library\\Shaders\\**', 'TextureSearchPaths=.\\', 'PresetPath=.\\Mine.ini', ''].join('\r\n');
  const merged = mergeIni(old, template);
  assert.equal(value(merged, 'GENERAL', 'EffectSearchPaths'), '.\\,D:\\Library\\Shaders\\**,.\\reshade-shaders\\Shaders\\**');
  assert.equal(value(merged, 'GENERAL', 'TextureSearchPaths'), '.\\,.\\reshade-shaders\\Textures\\**');
  assert.equal(value(merged, 'GENERAL', 'PresetPath'), '.\\Mine.ini', 'the chosen preset stays');
  assert.equal(mergeIni(merged, template), merged, 'a second launch changes nothing');
});

test('importing a ReShade folder copies presets and shaders and selects the preset it was using', () => {
  const data = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-reshade-'));
  try {
    tool.init(data);
    const bin = path.join(data, 'dlss5-cs2', 'game', 'bin', 'win64');
    fs.mkdirSync(bin, { recursive: true });
    const source = path.join(data, 'other-game');
    fs.mkdirSync(path.join(source, 'reshade-shaders', 'Shaders', 'sub'), { recursive: true });
    fs.writeFileSync(path.join(source, 'reshade-shaders', 'Shaders', 'sub', 'Tint.fx'), '// fx');
    fs.writeFileSync(path.join(source, 'Cinema.ini'), 'Techniques=Tint@Tint.fx\r\n');
    fs.writeFileSync(path.join(source, 'Other.ini'), 'TechniqueSorting=Tint@Tint.fx\r\n');
    fs.writeFileSync(path.join(source, 'settings.ini'), '[Not]\r\nA=preset\r\n');
    fs.writeFileSync(path.join(source, 'ReShade.ini'),
      '[GENERAL]\r\nPresetPath=.\\Cinema.ini\r\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\**,E:\\Shared\\Shaders\\**\r\n');

    const result = tool.importReShade(source);
    assert.deepEqual(result.presets.sort(), ['Cinema.ini', 'Other.ini']);
    assert.equal(result.preset, 'Cinema.ini');
    assert.ok(fs.existsSync(path.join(bin, 'reshade-shaders', 'Shaders', 'sub', 'Tint.fx')));
    assert.ok(!fs.existsSync(path.join(bin, 'settings.ini')), 'an ini that is not a preset stays behind');
    for (const ini of ['ReShade.ini', 'ReShade2.ini']) {
      const text = fs.readFileSync(path.join(bin, ini), 'utf8');
      assert.equal(value(text, 'GENERAL', 'PresetPath'), '.\\Cinema.ini');
      assert.match(value(text, 'GENERAL', 'EffectSearchPaths'), /E:\\Shared\\Shaders\\\*\*/, 'a shader library outside the folder is searched, not copied');
      assert.match(value(text, 'ADDON', 'LoadFromDllMain'), /renodx-dlss\.addon64/);
    }
    assert.equal(fs.readFileSync(path.join(source, 'Cinema.ini'), 'utf8'), 'Techniques=Tint@Tint.fx\r\n', 'the source is left alone');
    const info = tool.reshadeInfo();
    assert.equal(info.effects, 1);
    assert.equal(info.activePreset, 'Cinema.ini');
  } finally {
    fs.rmSync(data, { recursive: true, force: true });
  }
});

test('importing a folder with nothing of ReShade in it says so and changes nothing', () => {
  const data = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-reshade-'));
  try {
    tool.init(data);
    fs.mkdirSync(path.join(data, 'dlss5-cs2', 'game', 'bin', 'win64'), { recursive: true });
    const empty = path.join(data, 'empty');
    fs.mkdirSync(empty);
    assert.throws(() => tool.importReShade(empty), /No ReShade presets or shaders/);
  } finally {
    fs.rmSync(data, { recursive: true, force: true });
  }
});
