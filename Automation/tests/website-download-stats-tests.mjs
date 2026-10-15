import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../../Website/download-stats.js', import.meta.url), 'utf8');
const { countReleaseDownloads, fetchTotalDownloads } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
const asset = (name, count, state = 'uploaded') => ({ name, download_count: count, state });
const response = (releases, next = '') => ({
  ok: true, json: async () => releases, headers: { get: () => next },
});

test('counts installers and packages across versions without metadata or drafts', () => {
  assert.equal(countReleaseDownloads([
    { assets: [asset('Setup.exe', 12), asset('runtime-hotfix.zip', 8), asset('latest.yml', 99), asset('Setup.exe.blockmap', 99), asset('SHA256SUMS.txt', 99)] },
    { prerelease: true, assets: [asset('Older.EXE', 5), asset('unfinished.zip', 500, 'new')] },
    { draft: true, assets: [asset('Draft.exe', 500)] },
  ]), 25);
});

test('reads every release page, including an empty first page', async () => {
  const next = 'https://api.github.com/repos/Wockstajit/SourceMVM/releases?page=2&per_page=100';
  const visited = [];
  const pages = [response([], `<${next}>; rel="next"`), response([{ assets: [asset('old.exe', 31)] }])];
  assert.equal(await fetchTotalDownloads(async url => { visited.push(url); return pages.shift(); }), 31);
  assert.equal(visited.length, 2);
  assert.equal(visited[1], next);
});

test('fails the complete refresh if a later page fails', async () => {
  let page = 0;
  await assert.rejects(fetchTotalDownloads(async () => ++page === 1
    ? response([{ assets: [asset('setup.exe', 10)] }], '<https://api.github.com/repos/Wockstajit/SourceMVM/releases?page=2>; rel="next"')
    : { ok: false, status: 403 }), /unavailable/);
});

test('rejects foreign pagination, cycles and malformed counts', async () => {
  await assert.rejects(fetchTotalDownloads(async () => response([], '<https://example.com/releases>; rel="next"')), /pagination/);
  await assert.rejects(fetchTotalDownloads(async url => response([], `<${url}>; rel="next"`)), /pagination/);
  for (const count of [-1, '2', null, Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(() => countReleaseDownloads([{ assets: [asset('setup.exe', count)] }]), /count/);
  }
});
