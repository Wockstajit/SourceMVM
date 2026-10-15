import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

test('GUI command line enables NetCon by default and preserves an explicit port', () => {
  const source = fs.readFileSync(path.join(root, 'SourceMovieMakerLauncher/electron/main.cjs'), 'utf8');
  const fn = source.slice(source.indexOf('function buildCommandLine('), source.indexOf('function environmentFor('));
  const build = vm.runInNewContext(`${fn}; buildCommandLine`);
  assert.match(build({}), /-netconport 29010/);
  const custom = build({ customLaunchOptions: '-netconport 29011 -windowed' });
  assert.match(custom, /-netconport 29011/);
  assert.equal((custom.match(/-netconport/g) || []).length, 1);
});

test('underlying compatibility launch runs with no DLSS files, installer, or driver tool', () => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'mvm-no-dlss-'));
  try {
    fs.mkdirSync(path.join(temp, 'Automation/dlss'), { recursive: true });
    fs.mkdirSync(path.join(temp, 'Automation/launch'), { recursive: true });
    const script = path.join(temp, 'Automation/dlss/launch-isolated.ps1');
    fs.copyFileSync(path.join(root, 'Automation/dlss/launch-isolated.ps1'), script);
    fs.writeFileSync(path.join(temp, 'Automation/launch/launch-cs2-netcon.ps1'), `
param($Cs2Dir,$Monitor,$Width,$Height,$Port,$SettleSeconds,$OutDir)
@{port=$Port;cs2=$Cs2Dir;monitor=$Monitor} | ConvertTo-Json | Set-Content -LiteralPath $OutDir
`);
    const output = path.join(temp, 'result.json');
    execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script,
      '-Cs2Dir', 'standard CS2', '-OutDir', output], { windowsHide: true });
    const result = JSON.parse(fs.readFileSync(output, 'utf8').replace(/^\uFEFF/, ''));
    assert.equal(result.port, 29010);
    assert.equal(result.cs2, 'standard CS2');
    assert.equal(result.monitor, 3);
    assert.equal(fs.existsSync(path.join(temp, 'build')), false);
    execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script,
      '-Cs2Dir', 'standard CS2', '-Monitor', '0', '-OutDir', output], { windowsHide: true });
    const primary = JSON.parse(fs.readFileSync(output, 'utf8').replace(/^\uFEFF/, ''));
    assert.equal(primary.monitor, 0);
    assert.equal(fs.existsSync(path.join(temp, 'build')), false);
  } finally {
    fs.rmSync(temp, { recursive: true, force: true });
  }
});
