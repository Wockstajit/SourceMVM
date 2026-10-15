// Verify what an installed 0.1.0 launcher will actually see now that 0.1.1 is published.
// Pure HTTP against the live repo -- launches nothing.
let failures = 0;
const check = (label, ok, detail = '') => {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? ` — ${detail}` : ''}`);
  if (!ok) failures += 1;
};

const api = async (url) => (await fetch(url, {
  headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'smvm-verify' },
})).json();

const expectedVersion = JSON.parse(await (await import('node:fs/promises')).readFile(
  new URL('../../SourceMovieMakerLauncher/package.json', import.meta.url), 'utf8')).version;
const releases = await api('https://api.github.com/repos/Wockstajit/SourceMVM/releases?per_page=20');

// 1. electron-updater with allowPrerelease takes the newest non-draft release and reads latest.yml.
const newest = releases.find((r) => !r.draft);
check(`newest release matches package.json (${expectedVersion})`,
  newest.tag_name === `v${expectedVersion}`, newest.tag_name);
check('newest release is a pre-release', newest.prerelease === true);

const latestYmlAsset = newest.assets.find((a) => a.name === 'latest.yml');
check('newest release carries latest.yml', Boolean(latestYmlAsset));

const yml = await (await fetch(latestYmlAsset.browser_download_url)).text();
const advertisedVersion = /version:\s*(\S+)/.exec(yml)?.[1];
const advertisedFile = /path:\s*(\S+)/.exec(yml)?.[1];
const advertisedSize = Number(/size:\s*(\d+)/.exec(yml)?.[1]);
check('latest.yml advertises the built version', advertisedVersion === expectedVersion, advertisedVersion);

const installer = newest.assets.find((a) => a.name === advertisedFile);
check('advertised installer exists on the release', Boolean(installer), advertisedFile);
check('advertised size matches the uploaded file', installer.size === advertisedSize,
  `${advertisedSize} vs ${installer.size}`);

// 2. The payload half: this release has no manifest, so the launcher must fall through to 0.1.0's.
const manifestRelease = releases.find((r) => !r.draft && r.assets.some((a) => a.name === 'payload-manifest.json'));
check('a payload manifest resolves', Boolean(manifestRelease), manifestRelease?.tag_name);
// Whichever release supplies it, the manifest must list EVERY component -- a manifest missing
// `fx` would leave a fresh install with no particle pack at all.
const manifest = await (await fetch(
  manifestRelease.assets.find((a) => a.name === 'payload-manifest.json').browser_download_url,
)).json();
check('manifest lists both components',
  ['runtime', 'fx'].every((k) => Boolean(manifest?.components?.[k])),
  Object.keys(manifest?.components ?? {}).join(', '));

check('manifest schema is 1', manifest.schema === 1, String(manifest.schema));

// Every component URL the manifest points at must actually be downloadable.
for (const [name, entry] of Object.entries(manifest.components)) {
  const head = await fetch(entry.url, { method: 'HEAD', redirect: 'follow' });
  const size = Number(head.headers.get('content-length'));
  check(`payload "${name}" URL resolves`, head.ok, `${head.status}`);
  check(`payload "${name}" size matches manifest`, size === entry.bytes, `${entry.bytes} vs ${size}`);
}

console.log(`\n${failures === 0 ? 'ALL CHECKS PASSED' : `${failures} CHECK(S) FAILED`}`);
process.exit(failures === 0 ? 0 : 1);
