import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { test } from 'node:test';

const js = readFileSync(new URL('../../Website/main.js', import.meta.url), 'utf8').split('// ---- Logo')[0];
async function run(releases, { ok = true, report = {}, fallback } = {}) {
  const elements = Object.fromEntries(['download', 'download-label', 'download-status', 'download-checksums', 'virustotal-container', 'virustotal-report'].map(id =>
    [id, { textContent: '', hidden: id === 'virustotal-container', attributes: { 'aria-disabled': 'true' },
      removeAttribute(key) { delete this.attributes[key]; if (key === 'href') delete this.href; },
      getAttribute(key) { return this.attributes[key]; },
      setAttribute(key, value) { this.attributes[key] = value; } }]));
  if (fallback) elements.download.attributes.href = fallback;
  const context = vm.createContext({ URL, AbortSignal, document: { getElementById: id => elements[id] },
    fetch: async url => ({ ok, status: ok ? 200 : 403, json: async () => url === './virustotal.json' ? report : releases }) });
  vm.runInContext(js, context);
  await new Promise(resolve => setImmediate(resolve));
  return elements;
}
const installer = { name: 'SourceMovieMaker-Complete-Setup-0.1.7.exe', state: 'uploaded',
  browser_download_url: 'https://github.com/Wockstajit/SourceMVM/releases/download/v0.1.7/setup.exe' };
test('empty feed keeps the exact Download label and exposes the unavailable state', async () => {
  const e = await run([]);
  assert.equal(e['download-label'].textContent, 'Download');
  assert.equal(e.download.href, undefined);
  assert.equal(e.download.attributes['aria-disabled'], 'true');
  assert.equal(e['download-status'].textContent, 'Download unavailable');
});
test('only a published complete uploaded installer enables Download', async () => {
  const e = await run([
    { draft: true, assets: [installer] },
    { assets: [{ ...installer, state: 'new' }] },
    { name: 'Public beta 0.1.7', assets: [installer] },
  ]);
  assert.equal(e.download.href, installer.browser_download_url);
  assert.equal(e.download.attributes['aria-disabled'], undefined);
  assert.equal(e['download-status'].textContent, 'Download SOURCE:MVM');
});
test('foreign hosts, private source URLs, and non-installer assets never become download links', async () => {
  for (const url of ['https://example.com/setup.exe',
    'https://github.com/Wockstajit/Source-MVM/releases/download/v0.1.7/setup.exe',
    'https://github.com/Wockstajit/SourceMVM-evil/releases/download/v0.1.7/setup.exe']) {
    const e = await run([{ assets: [{ ...installer, browser_download_url: url }] }]);
    assert.equal(e.download.href, undefined);
  }
  const e = await run([{ assets: [{ ...installer, name: 'latest.yml' }] }]);
  assert.equal(e.download.href, undefined);
});
test('API failure falls back to the public releases page', async () => {
  const e = await run(null, { ok: false });
  assert.equal(e.download.href, 'https://github.com/Wockstajit/SourceMVM/releases');
  assert.equal(e['download-status'].textContent, 'Download SOURCE:MVM');
});

test('the published HTML download remains direct when the releases API is rate limited', async () => {
  const fallback = installer.browser_download_url;
  assert.equal((await run(null, { ok: false, fallback })).download.href, fallback);
});
test('VirusTotal is shown only for a supplied report matching the exact downloaded installer', async () => {
  const hash = 'a'.repeat(64), url = `https://www.virustotal.com/gui/file/${hash}/detection`;
  const e = await run([{ assets: [{ ...installer, digest: 'sha256:' + hash }] }], { report: { sha256: hash, url } });
  assert.equal(e['virustotal-report'].href, url);
  assert.equal(e['virustotal-container'].hidden, false);
});
test('missing, stale and foreign VirusTotal links cannot imply a completed scan', async () => {
  const hash = 'a'.repeat(64);
  for (const report of [{}, { sha256: 'b'.repeat(64), url: `https://www.virustotal.com/gui/file/${hash}/detection` },
    { sha256: hash, url: `https://example.com/gui/file/${hash}/detection` },
    { sha256: hash, url: `https://www.virustotal.com/gui/file/${'b'.repeat(64)}/detection` }]) {
    const e = await run([{ assets: [{ ...installer, digest: 'sha256:' + hash }] }], { report });
    assert.equal(e['virustotal-container'].hidden, true);
    assert.equal(e['virustotal-report'].href, undefined);
  }
});
test('the installed executable report uses its normal name and binds to the correct installer', async () => {
  const hash = 'a'.repeat(64), launcherHash = 'b'.repeat(64);
  const report = { fileName: 'SourceMovieMaker.exe', sha256: launcherHash, installerSha256: hash,
    url: `https://www.virustotal.com/gui/file/${launcherHash}` };
  const releases = [{ assets: [{ ...installer, digest: 'sha256:' + hash }] }];
  const e = await run(releases, { report });
  assert.equal(e['virustotal-container'].hidden, false);
  assert.equal(e['virustotal-report'].textContent, 'VirusTotal report');
  assert.equal(e['virustotal-report'].href, report.url);
  for (const patch of [{ installerSha256: 'c'.repeat(64) }, { fileName: 'other.exe' },
    { sha256: 'c'.repeat(64) }]) {
    assert.equal((await run(releases, { report: { ...report, ...patch } }))['virustotal-container'].hidden, true);
  }
});
