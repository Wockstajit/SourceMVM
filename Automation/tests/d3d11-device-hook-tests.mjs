// Static regression checks for the D3D11CreateDevice detour in RenderSystemDX11Hooks.cpp.
// node --test Automation/tests/d3d11-device-hook-tests.mjs

import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');

// Issue #114: NVIDIA Ansel (NvCamera64.dll) creates its own D3D11 devices in CS2, sometimes before the
// engine. Our serial-driver flag on Ansel's device crashed CS2 at startup on a GTX 1660 Ti, and
// "first device wins" hooked Ansel's device instead of the engine's (seen in 6 crash reports).
test('only CS2 render system devices get the NVIDIA flag and the device hooks', () => {
  const hooks = read('AfxHookSource2/RenderSystemDX11Hooks.cpp');
  const start = hooks.indexOf('HRESULT WINAPI New_D3D11CreateDevice(');
  const end = hooks.indexOf('void Before_Present()', start);
  assert.ok(start >= 0 && end > start, 'New_D3D11CreateDevice not found');
  const body = hooks.slice(start, end);
  assert.match(body, /const bool engineCaller = 0 == _stricmp\(callerName, "rendersystemdx11\.dll"\);/);
  assert.match(body, /if \(engineCaller && pAdapter && SUCCEEDED/);
  assert.match(body, /else if \(engineCaller && !pAdapter && DriverType == D3D_DRIVER_TYPE_HARDWARE\)/);
  assert.match(body, /const bool hookDevice = engineCaller && SUCCEEDED\(result\)/);
  // The caller is known before the device is created, so the flag decision can use it.
  assert.ok(body.indexOf('engineCaller =') < body.indexOf('g_Old_D3D11CreateDevice('));
});
