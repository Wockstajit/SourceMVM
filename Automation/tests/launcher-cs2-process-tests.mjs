import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { test } from 'node:test';
const require = createRequire(import.meta.url);
const { parseTasklistCsv, findCs2Processes, installBlocker } =
  require('../../SourceMovieMakerLauncher/electron/cs2-process.cjs');

test('tasklist CSV rows give process ids; the localized no-match line gives none', () => {
  assert.deepEqual(parseTasklistCsv('"cs2.exe","1234","Console","1","1,234,567 K"\r\n'), [1234]);
  assert.deepEqual(parseTasklistCsv('"cs2.exe","1","Console","1","1 K"\n"cs2.exe","22","Console","1","2 K"'), [1, 22]);
  assert.deepEqual(parseTasklistCsv('INFO: No tasks are running which match the specified criteria.'), []);
  assert.deepEqual(parseTasklistCsv('INFORMATIONEN: Es werden keine Aufgaben mit den angegebenen Kriterien ausgeführt.'), []);
});

test('a failed or slow tasklist falls back to PowerShell instead of reporting CS2 as running', () => {
  const calls = [];
  const found = findCs2Processes((file) => {
    calls.push(file);
    if (file === 'tasklist.exe') throw new Error('spawnSync tasklist.exe ETIMEDOUT');
    return '\r\n';
  });
  assert.deepEqual(calls, ['tasklist.exe', 'powershell.exe']);
  assert.deepEqual(found, { ids: [] });
  assert.equal(installBlocker({ launching: false, launchedPid: 0, found }), true);
});

test('when no method can list processes the message says so instead of "close CS2"', () => {
  const found = findCs2Processes(() => { throw new Error('blocked'); });
  assert.match(found.error, /tasklist: blocked; PowerShell: blocked/);
  const message = installBlocker({ launching: false, launchedPid: 0, found });
  assert.match(message, /Could not check whether CS2 is running/);
  assert.doesNotMatch(message, /still running/);
});

test('a running CS2 is named by PID with what to do about a stuck one', () => {
  const one = installBlocker({ launching: false, launchedPid: 0, found: { ids: [4321] } });
  assert.match(one, /cs2\.exe \(PID 4321\) is still running/);
  assert.match(one, /Task Manager > Details/);
  const launched = installBlocker({ launching: false, launchedPid: 7, found: { ids: [9] } });
  assert.match(launched, /PIDs 7, 9/);
  assert.match(installBlocker({ launching: true, launchedPid: 0, found: {} }), /being launched/);
});
