// Nothing leaves the player's PC without the player pressing Send (Docs/features/filmmaker_community.md).
// Static checks over the sources: crash handling, renders and launcher start-up must have no path to
// the report webhook, and the only callers of a send are the two Send buttons' bridge verbs.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8');
const COMMUNITY = 'AfxHookSource2/Filmmaker/Community';
const crash = read(`${COMMUNITY}/CrashReport.cpp`);
const hud = read(`${COMMUNITY}/CommunityHud.cpp`);
const js = read(`${COMMUNITY}/CommunityJs.h`);

// The body of `name(...) { ... }` in a C++ source, by brace matching outside string, char
// literals and comments ("AbortMessage({" must not count).
function body(src, signature) {
  const at = src.indexOf(signature);
  assert.ok(at >= 0, `${signature} not found`);
  const i = src.indexOf('{', at);
  let depth = 0;
  for (let j = i; j < src.length; ++j) {
    const c = src[j];
    if (c === '"' || c === "'") {
      for (++j; j < src.length && src[j] !== c; ++j) if (src[j] === '\\') ++j;
    } else if (c === '/' && src[j + 1] === '/') {
      j = src.indexOf('\n', j);
    } else if (c === '{') ++depth;
    else if (c === '}' && --depth === 0) return src.slice(i, j + 1);
  }
  throw new Error(`unbalanced ${signature}`);
}

test('a crash only writes its report; nothing is sent from the crash path', () => {
  for (const fn of ['DWORD WINAPI CrashWorker(void*)', 'void HandleCrash(', 'void PreparePending()', 'void RecordUnexpectedExit()']) {
    assert.doesNotMatch(body(crash, fn), /BugReportSend_|HttpRequest|WinHttp/, `${fn} must not send`);
  }
  // The one crash send is SendApproved, started only by CrashReport_SendPending.
  assert.equal(crash.match(/BugReportSend_Blocking\(/g).length, 1);
  assert.match(body(crash, 'void SendApproved()'), /BugReportSend_Blocking\(/);
  assert.match(body(crash, 'void CrashReport_SendPending()'), /RunPendingWork\(SendApproved\)/);
  assert.doesNotMatch(body(crash, 'void CrashReport_MainThreadFrame()'), /SendApproved|CrashReport_SendPending/);
});

test('reports are sent only from the Send buttons', () => {
  // Writing a report never sends it.
  assert.doesNotMatch(body(hud, 'void WriteReport('), /BugReportSend_Start/);
  assert.equal(hud.match(/BugReportSend_Start\(/g).length, 1);
  assert.match(body(hud, 'void SendReport(int seq)'), /BugReportSend_Start\(/);
  assert.match(hud, /verb == "sendreport"\) \{\s*SendReport\(/);
  assert.equal(hud.match(/CrashReport_SendPending\(\)/g).length, 1);
  assert.match(hud, /verb == "crashsend"\) \{\s*CrashReport_SendPending\(\);/);
  // The UI only issues those verbs from a button's click handler.
  for (const verb of ['sendreport', 'crashsend']) {
    const uses = [...js.matchAll(new RegExp(`C\\.cmd\\('${verb}`, 'g'))];
    assert.equal(uses.length, 1, `${verb} is issued once`);
    // Inside a click handler: the nearest button(...) opened before it, with its function () body.
    const before = js.slice(Math.max(0, uses[0].index - 300), uses[0].index);
    assert.match(before, /button\([^;]*function \(\) \{[^}]*$/, `${verb} comes from a button`);
  }
});

test('no Steam identity is read or sent', () => {
  for (const file of fs.readdirSync(path.join(ROOT, COMMUNITY))) {
    if (file.endsWith('.local.h')) continue;
    // Nothing reads the persona or writes an ID field. (The scrub that removes IDs from logged text,
    // and CrashReport.cpp refusing old reports that had a "SteamID" line, only mention them.)
    const src = read(`${COMMUNITY}/${file}`).replace('find("\\nSteamID   :")', '');
    assert.doesNotMatch(src, /MyPersonaAPI|GetXuid|steamId64|steamName|steamcommunity\.com|"SteamID|"Steam  /, file);
  }
});

test('report text is scrubbed of the Steam name and IDs when written and when sent', () => {
  const report = read(`${COMMUNITY}/BugReport.cpp`), send = read(`${COMMUNITY}/BugReportSend.cpp`);
  assert.match(body(report, 'std::string BugReport_ScrubPrivate('), /ScrubSteamIdentity\(/);
  assert.match(report, /std::string text = BugReport_ScrubPrivate\(out\.str\(\)\)/);
  assert.match(send, /BugReport_ScrubPrivate\(bytes\.str\(\)\)/);
  assert.match(hud, /BugReport_NoteLocalPlayerName\(\)/);
});

test('sent reports delete themselves and the folder is pruned', () => {
  const send = read(`${COMMUNITY}/BugReportSend.cpp`);
  assert.match(send, /if \(Post\(report, detail\)\) \{\s*\/\/[^\n]*\n\s*DeleteFileW\(report\.path\.c_str\(\)\);/);
  const sent = body(crash, 'void SendApproved()');
  for (const ext of ['.pending', '.session.log', '.dmp', '.txt']) assert.ok(sent.includes(`Path(b + "${ext}")`), ext);
  assert.match(body(crash, 'void PreparePending()'), /PruneOldReports\(\);/);
});

test('renders and launcher start-up have no upload', () => {
  assert.ok(!fs.existsSync(path.join(ROOT, `${COMMUNITY}/CamPathShare.cpp`)));
  assert.doesNotMatch(read('AfxHookSource2/Filmmaker/Render/RenderSessionDriver.cpp'), /CamPathShare|BugReportSend/);
  assert.ok(!fs.existsSync(path.join(ROOT, 'SourceMovieMakerLauncher/electron/usage-ping.cjs')));
  const main = read('SourceMovieMakerLauncher/electron/main.cjs');
  assert.doesNotMatch(main, /require\('\.\/usage-ping|sendUsagePings|webhook/i);
});
