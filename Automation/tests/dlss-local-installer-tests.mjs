import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const installer = fs.readFileSync(path.join(root, 'Automation/dlss/install-isolated.ps1'), 'utf8');
const legacyInstaller = fs.readFileSync(path.join(root, 'Automation/dlss/install-dlss.ps1'), 'utf8');
const batch = fs.readFileSync(path.join(root, 'install-dlss5.bat'), 'utf8');

test('DLSS batch prompts for old GPUs and maps both choices to explicit local variants', () => {
  assert.match(batch, /choice \/C YN/i);
  assert.match(batch, /RTX 40, 30, 20 compatible/);
  assert.match(batch, /NVIDIA original/);
  assert.match(batch, /install-isolated\.ps1/i);
});

test('installer uses the local payload and contains no network or download command', () => {
  assert.match(installer, /Join-Path \$repo 'dlss 5'/);
  assert.doesNotMatch(installer, /Invoke-WebRequest|Start-BitsTransfer|curl(?:\.exe)?|ensure-packages|dlss5-downloads/i);
  assert.match(installer, /The local DLSS 5 payload is incomplete/);
});

test('installer keeps ReShade out of the normal Steam executable directory', () => {
  assert.match(installer, /\$privateBin = Join-Path \$privateRoot 'game\\bin\\win64'/);
  assert.match(installer, /\$sharedReShadeProxies/);
  assert.match(installer, /Test-IsReShadeProxy/);
  assert.match(installer, /Remove-Item -LiteralPath \$source -Force/);
  assert.match(installer, /dlss5-steam-backups/);
  assert.match(installer, /Normal Steam CS2 will not load ReShade/);
});

test('installer checks hashes and skips files that are already current', () => {
  assert.match(installer, /Get-FileHash -Algorithm SHA256/);
  assert.match(installer, /DLSS 5 \(\$RuntimeVariant\) is already installed and current\. Nothing was copied/);
  assert.match(installer, /is already current; skipped/);
});

test('legacy installer cannot install DLSS into the shared Steam CS2 tree', () => {
  assert.match(legacyInstaller, /shared-CS2 DLSS installer has been retired/);
  assert.match(legacyInstaller, /install-isolated\.ps1/);
  assert.doesNotMatch(legacyInstaller, /game\\bin\\win64|Copy-Item|Set-Content|Remove-Item|Move-Item/);
});
