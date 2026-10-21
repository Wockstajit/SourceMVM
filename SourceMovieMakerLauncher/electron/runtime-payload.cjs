// Game-side runtime payload: fetch, verify, and install the pieces the launcher injects.
//
// The launcher ships the UI and the injection host. Everything that goes INTO CS2 -- the
// AfxHookSource2 hook, the injector, the shader/snippet resources, and the particle FX pack --
// is delivered separately as release assets on the public release repo, because the FX pack
// alone is ~230 MB and must not be re-downloaded every time the UI changes.
//
// This module owns that job end to end and nothing else. It does not launch CS2, does not touch
// electron-updater (which updates the APP, not the payload), and is not called from the launch
// path except through installedRuntimeDir()/installedFxDir(), which are pure lookups.
//
// Install layout, all under the launcher's own userData directory so an uninstall takes it with
// it and no admin rights are ever needed:
//
//   <userData>/payload/runtime/x64/AfxHookSource2.dll
//   <userData>/payload/runtime/x64/injector.exe
//   <userData>/payload/runtime/resources/...
//   <userData>/payload/fx/source_mvm_fx/...
//   <userData>/payload/fx/source_mvm_fx/cfg/...   written by CS2 itself, kept across updates
//   <userData>/payload/installed.json
//
// THE SELF-CONTAINED BUILD. A launcher packaged with `npm run package:bundled` carries the whole
// payload inside the installer (resources/bundle/{runtime,fx}, described by bundle.json). It never
// downloads a payload: the first start (and the first start after an update) copies the bundled
// components into the same <userData>/payload layout, so the launch path is unchanged. Without
// this, an older payload on the release repo would replace the newer one the installer shipped.

const { app, net } = require('electron');
const crypto = require('node:crypto');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { execFile } = require('node:child_process');

const { HOTFIX_ASSET, pickHotfix, bundleRuntimeWanted } = require('./runtime-hotfix.cjs');
const { FX_SAVED_SETTINGS, recoverInterrupted, replaceDirectory } = require('./payload-swap.cjs');

const RELEASE_OWNER = 'Wockstajit';
const RELEASE_REPO = 'SourceMVM';
const RELEASES_URL = `https://api.github.com/repos/${RELEASE_OWNER}/${RELEASE_REPO}/releases?per_page=20`;
const MANIFEST_ASSET = 'payload-manifest.json';
const MANIFEST_SCHEMA = 1;

// Components are declared here rather than trusted from the manifest, so a manifest cannot ask
// the launcher to unpack an arbitrary name into an arbitrary place.
const COMPONENTS = {
  runtime: {
    label: 'Hook runtime',
    directory: 'runtime',
    // Sanity probe: an archive that unpacks without this is not a runtime payload.
    probe: path.join('x64', 'AfxHookSource2.dll'),
  },
  fx: {
    label: 'FX pack',
    directory: 'fx',
    probe: 'source_mvm_fx',
    // CS2 saves its video, sound and other settings into this folder (payload-swap.cjs). A new
    // FX pack must not reset them.
    keep: FX_SAVED_SETTINGS,
  },
};

let inFlight = null;

// ---- bundled payload ---------------------------------------------------------------------------
function bundleRoot() {
  if (!app.isPackaged) return '';
  const root = path.join(process.resourcesPath, 'bundle');
  return fs.existsSync(path.join(root, 'bundle.json')) ? root : '';
}

function bundleInfo() {
  const root = bundleRoot();
  if (!root) return null;
  try {
    const info = JSON.parse(fs.readFileSync(path.join(root, 'bundle.json'), 'utf8'));
    return info && typeof info.stamp === 'string' ? { ...info, root } : null;
  } catch {
    return null;
  }
}

/** True when this launcher carries its own payload (and so never downloads one). */
function isBundled() {
  return Boolean(bundleInfo());
}

/**
 * Put right a component replace that a launcher closed part way through (payload-swap.cjs): the
 * component may be sitting in <folder>.previous, and a bundle the installer moved in may be
 * sitting in <folder>.incoming. Cheap when there is nothing to do, so every start runs it, bundled
 * or not and with or without the network, before anything looks at what is installed.
 */
async function recoverInterruptedInstalls() {
  for (const name of Object.keys(COMPONENTS)) await recoverInterrupted(componentDir(name));
}

/**
 * Copy the bundled components into <userData>/payload when they are not already the installed
 * ones. Cheap when current (one JSON read), so it runs on every start.
 */
//
// Start-up and the Launch button can both ask for this at once. Two installs racing over the same
// target folder made one of them fail (and say so) while the other succeeded, so every caller
// shares the one in-flight install.
let bundledInFlight = null;

function installBundled(report = () => {}) {
  if (!bundledInFlight) {
    bundledInFlight = installBundledOnce(report).finally(() => { bundledInFlight = null; });
  }
  return bundledInFlight;
}

// Each component carries its own stamp (scripts/build-bundle.ps1), so an update re-installs only
// what changed -- a new hook DLL does not re-copy the FX pack. Older bundles had one stamp.
function componentStamp(info, name) {
  const own = info.components && info.components[name];
  return `bundle:${own || info.stamp}`;
}

async function installBundledOnce(report) {
  // Before the bundle is checked: a cut-short install can be holding it in <folder>.incoming.
  await recoverInterruptedInstalls();
  const info = bundleInfo();
  if (!info) return { state: 'not-bundled' };
  const installed = readInstalledState();
  // Compared on the bundle stamp, not the installed hash: a runtime hotfix replaces the DLL (and
  // its hash) but must survive the next start, until a newer bundle arrives with an app update.
  const wanted = Object.keys(COMPONENTS).filter((name) => {
    const present = name === 'runtime' ? installedRuntimeDir() : installedFxDir();
    return bundleRuntimeWanted(installed[name], componentStamp(info, name), present);
  });
  if (!wanted.length) return { state: 'current', version: info.version, local: localStatus() };
  for (const name of wanted) {
    const component = COMPONENTS[name];
    const source = path.join(info.root, component.directory);
    const stamp = componentStamp(info, name);
    const committed = { version: info.version, sha256: stamp, bundleStamp: stamp, hotfix: 0, bytes: 0 };
    if (!fs.existsSync(path.join(source, component.probe))) {
      // The last start moved this bundle into place, then closed before recording it: the bundle
      // is gone from the installer because it IS the installed folder. Record it now.
      const present = name === 'runtime' ? installedRuntimeDir() : installedFxDir();
      if (present && installed[name] && installed[name].pending === stamp) {
        installed[name] = committed;
        writeInstalledState(installed);
        continue;
      }
      throw new Error(`The installer's ${component.label} is incomplete (${component.probe} missing). Reinstall SourceMovieMaker.`);
    }
    report({ state: 'installing', component: name, label: component.label, percent: 0 });
    // Noted before the swap: a move takes the bundle out of the installer, and a start that closes
    // before the line below records the install needs this to recognise it.
    installed[name] = { ...(installed[name] || {}), pending: stamp };
    writeInstalledState(installed);
    // The old folder stays until the new one is in place (CS2's saved settings are carried over),
    // and comes back if anything fails.
    await replaceDirectory(componentDir(name), async (target) => {
      // MOVE when the installer and the payload share a drive (the default per-user install): it
      // is instant and the installer's copy does not sit on disk twice. A later app update brings
      // the bundle back. Anything else (another drive, a read-only Program Files) copies, with
      // progress.
      try {
        await fsp.rename(source, target);
        // A failed install gives the bundle back, so the next start can try again.
        return () => fsp.rename(target, source);
      } catch {
        await copyWithProgress(source, target, (percent) => {
          report({ state: 'installing', component: name, label: component.label, percent });
        });
        return null;
      }
    }, component.keep, { movedFrom: source });
    installed[name] = committed;
    writeInstalledState(installed);
    report({ state: 'installing', component: name, label: component.label, percent: 100 });
  }
  report({ state: 'installed', version: info.version });
  return { state: 'installed', version: info.version, local: localStatus() };
}

async function listFiles(root, relative = '', out = []) {
  for (const entry of await fsp.readdir(path.join(root, relative), { withFileTypes: true })) {
    const rel = path.join(relative, entry.name);
    if (entry.isDirectory()) await listFiles(root, rel, out);
    else if (entry.isFile()) out.push({ rel, size: (await fsp.stat(path.join(root, rel))).size });
  }
  return out;
}

/** Copy a folder file by file, reporting whole-percent progress by bytes. */
async function copyWithProgress(source, target, onPercent) {
  const files = await listFiles(source);
  const total = files.reduce((sum, file) => sum + file.size, 0) || 1;
  let done = 0;
  let last = -1;
  for (const file of files) {
    const to = path.join(target, file.rel);
    await fsp.mkdir(path.dirname(to), { recursive: true });
    await fsp.copyFile(path.join(source, file.rel), to);
    done += file.size;
    const percent = Math.floor((done / total) * 100);
    if (percent !== last) {
      last = percent;
      onPercent(percent);
    }
  }
}

function payloadRoot() {
  return path.join(app.getPath('userData'), 'payload');
}

function componentDir(name) {
  return path.join(payloadRoot(), COMPONENTS[name].directory);
}

function installedStatePath() {
  return path.join(payloadRoot(), 'installed.json');
}

/** Where the launch path should look for the hook/injector, or '' when nothing is installed. */
function installedRuntimeDir() {
  const directory = componentDir('runtime');
  return fs.existsSync(path.join(directory, COMPONENTS.runtime.probe)) ? directory : '';
}

/** Where the launch path should point USRLOCALCSGO, or '' when no FX pack is installed. */
function installedFxDir() {
  const directory = path.join(componentDir('fx'), 'source_mvm_fx');
  return fs.existsSync(directory) ? directory : '';
}

function readInstalledState() {
  try {
    const state = JSON.parse(fs.readFileSync(installedStatePath(), 'utf8'));
    return state && typeof state === 'object' ? state : {};
  } catch {
    return {};
  }
}

function writeInstalledState(state) {
  fs.mkdirSync(payloadRoot(), { recursive: true });
  // Written beside the old file and renamed over it: a crash, power loss or full disk part way
  // through leaves the previous state, never a cut-short file. installBundledOnce depends on its
  // pending stamp to recognise a bundled install that finished just before a crash.
  const file = installedStatePath();
  const temporary = `${file}.tmp`;
  fs.writeFileSync(temporary, `${JSON.stringify(state, null, 2)}\n`, { encoding: 'utf8', flush: true });
  fs.renameSync(temporary, file);
}

async function fetchJson(url) {
  const response = await net.fetch(url, {
    headers: {
      Accept: 'application/vnd.github+json',
      'User-Agent': `SourceMovieMaker/${app.getVersion()}`,
    },
  });
  if (!response.ok) {
    throw new Error(`${url} responded ${response.status} ${response.statusText}.`);
  }
  return response.json();
}

/**
 * Newest release carrying a payload manifest. Pre-releases count: the beta ships as a GitHub
 * pre-release so the repo's public "Latest release" stays empty until 1.0, and the payload has
 * to track the same releases the app updater tracks.
 */
async function findManifestRelease() {
  const releases = await fetchJson(RELEASES_URL);
  if (!Array.isArray(releases) || releases.length === 0) return null;
  for (const release of releases) {
    if (release.draft) continue;
    const asset = (release.assets || []).find((candidate) => candidate.name === MANIFEST_ASSET);
    if (asset) return { release, manifestUrl: asset.browser_download_url };
  }
  return null;
}

function validateManifest(manifest) {
  if (!manifest || typeof manifest !== 'object') {
    throw new Error('The payload manifest was not an object.');
  }
  if (manifest.schema !== MANIFEST_SCHEMA) {
    throw new Error(
      `The payload manifest is schema ${manifest.schema}, but this launcher understands `
      + `schema ${MANIFEST_SCHEMA}. Update SourceMovieMaker itself first.`,
    );
  }
  const components = manifest.components && typeof manifest.components === 'object'
    ? manifest.components
    : {};
  for (const [name, entry] of Object.entries(components)) {
    if (!COMPONENTS[name]) continue;
    if (typeof entry.url !== 'string' || !entry.url.startsWith('https://')) {
      throw new Error(`Payload component "${name}" has no https download URL.`);
    }
    if (typeof entry.sha256 !== 'string' || !/^[0-9a-f]{64}$/i.test(entry.sha256)) {
      throw new Error(`Payload component "${name}" has no valid sha256.`);
    }
  }
  return components;
}

async function downloadToFile(url, destination, onProgress) {
  const response = await net.fetch(url, {
    headers: { 'User-Agent': `SourceMovieMaker/${app.getVersion()}` },
  });
  if (!response.ok) {
    throw new Error(`Download failed: ${response.status} ${response.statusText}.`);
  }
  const declared = Number(response.headers.get('content-length')) || 0;
  const hash = crypto.createHash('sha256');
  const handle = await fsp.open(destination, 'w');
  let received = 0;
  try {
    for await (const chunk of response.body) {
      const buffer = Buffer.from(chunk);
      hash.update(buffer);
      await handle.write(buffer);
      received += buffer.length;
      if (onProgress) onProgress(received, declared);
    }
  } finally {
    await handle.close();
  }
  return { sha256: hash.digest('hex'), bytes: received };
}

// Windows ships bsdtar as tar.exe (1803+), and it reads .zip. Using it keeps the launcher free
// of a native archive dependency. System32 is spelled out so a tar.exe earlier on PATH -- Git's
// GNU tar, for instance -- cannot be picked up instead.
function systemTar() {
  const system32 = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe');
  return fs.existsSync(system32) ? system32 : 'tar';
}

function extractZip(archive, destination) {
  return new Promise((resolve, reject) => {
    execFile(
      systemTar(),
      ['-x', '-f', archive, '-C', destination],
      { windowsHide: true, maxBuffer: 8 * 1024 * 1024 },
      (error, _stdout, stderr) => {
        if (error) {
          reject(new Error(`Could not unpack ${path.basename(archive)}: ${stderr || error.message}`));
          return;
        }
        resolve();
      },
    );
  });
}

async function installComponent(name, entry, report) {
  const component = COMPONENTS[name];
  const stagingRoot = await fsp.mkdtemp(path.join(os.tmpdir(), 'smvm-payload-'));
  const archive = path.join(stagingRoot, `${name}.zip`);
  const unpacked = path.join(stagingRoot, 'unpacked');

  try {
    report({ state: 'downloading', component: name, label: component.label, percent: 0 });
    const downloaded = await downloadToFile(entry.url, archive, (received, declared) => {
      report({
        state: 'downloading',
        component: name,
        label: component.label,
        percent: declared ? Math.min(100, (received / declared) * 100) : 0,
        received,
        total: declared,
      });
    });

    if (downloaded.sha256.toLowerCase() !== entry.sha256.toLowerCase()) {
      throw new Error(
        `${component.label} failed its checksum. Expected ${entry.sha256}, got ${downloaded.sha256}.`,
      );
    }

    report({ state: 'installing', component: name, label: component.label });
    await fsp.mkdir(unpacked, { recursive: true });
    await extractZip(archive, unpacked);

    if (!fs.existsSync(path.join(unpacked, component.probe))) {
      throw new Error(
        `${component.label} unpacked without ${component.probe}; the release asset looks wrong.`,
      );
    }

    // Swap last, so a failed download never leaves a half-replaced install behind. The old folder
    // stays until the new one is in place (CS2's saved settings are carried over), and comes back
    // if anything fails.
    await replaceDirectory(componentDir(name), async (target) => {
      try {
        await fsp.rename(unpacked, target);
      } catch (error) {
        // EXDEV: staging %TEMP% and userData are on different drives -- the portable build run
        // from a USB stick or second disk. rename() cannot cross volumes; copy+delete can.
        if (error.code !== 'EXDEV') throw error;
        await fsp.cp(unpacked, target, { recursive: true });
        await fsp.rm(unpacked, { recursive: true, force: true }).catch(() => {});
      }
    }, component.keep);

    return { version: entry.version || '', sha256: entry.sha256, bytes: downloaded.bytes };
  } finally {
    await fsp.rm(stagingRoot, { recursive: true, force: true }).catch(() => {});
  }
}

/** What is installed locally, without touching the network. */
function localStatus() {
  const installed = readInstalledState();
  return {
    root: payloadRoot(),
    components: Object.fromEntries(Object.keys(COMPONENTS).map((name) => {
      const present = name === 'runtime'
        ? Boolean(installedRuntimeDir())
        : Boolean(installedFxDir());
      return [name, {
        label: COMPONENTS[name].label,
        present,
        version: (installed[name] && installed[name].version) || '',
        sha256: (installed[name] && installed[name].sha256) || '',
      }];
    })),
  };
}

/** Ask the release repo what payload is current and compare it against what is installed. */
async function checkForPayload() {
  // A self-contained build only ever uses what it shipped with.
  const bundled = bundleInfo();
  if (bundled) {
    const local = localStatus();
    const ready = local.components.runtime.present && local.components.fx.present;
    return { state: ready ? 'current' : 'outdated', bundled: true, version: bundled.version,
      outdated: ready ? [] : Object.keys(COMPONENTS), local };
  }
  const found = await findManifestRelease();
  if (!found) {
    return {
      state: 'none-published',
      message: 'No SOURCE:MVM runtime has been published yet.',
      local: localStatus(),
    };
  }
  const manifest = await fetchJson(found.manifestUrl);
  const components = validateManifest(manifest);
  const installed = readInstalledState();
  const outdated = Object.entries(components)
    .filter(([name]) => COMPONENTS[name])
    .filter(([name, entry]) => {
      const present = name === 'runtime' ? installedRuntimeDir() : installedFxDir();
      if (!present) return true;
      return (installed[name] || {}).sha256 !== entry.sha256;
    })
    .map(([name]) => name);

  return {
    state: outdated.length ? 'outdated' : 'current',
    version: manifest.version || found.release.tag_name || '',
    prerelease: Boolean(found.release.prerelease),
    outdated,
    manifest: { version: manifest.version || '', components },
    local: localStatus(),
  };
}

/**
 * Install every component the release repo has that this launcher does not already have.
 * `only` narrows it to specific component names; omit to take whatever checkForPayload flagged.
 */
async function installPayload(report, only) {
  if (inFlight) throw new Error('A runtime install is already running.');
  inFlight = (async () => {
    if (isBundled()) return installBundled(report);
    const check = await checkForPayload();
    if (check.state === 'none-published') return check;

    const wanted = only && only.length
      ? only.filter((name) => COMPONENTS[name])
      : check.outdated;
    if (!wanted.length) {
      return { state: 'current', version: check.version, local: localStatus() };
    }

    const installed = readInstalledState();
    for (const name of wanted) {
      const entry = check.manifest.components[name];
      if (!entry) continue;
      installed[name] = await installComponent(name, entry, report);
      writeInstalledState(installed);
    }

    report({ state: 'installed', version: check.version });
    return { state: 'installed', version: check.version, local: localStatus() };
  })();

  try {
    return await inFlight;
  } finally {
    inFlight = null;
  }
}

/**
 * Lay a hotfix zip OVER the installed runtime: only the files it carries (normally just
 * x64/AfxHookSource2.dll, ~14 MB) are replaced -- the runtime folder is ~3 GB, so the full-component
 * swap installComponent does would re-ship all of it. Every replaced file is backed up first and
 * put back if any step fails, so a broken download never leaves a half-patched runtime.
 */
async function installRuntimeOverlay(entry, report) {
  const target = installedRuntimeDir();
  if (!target) throw new Error('No runtime is installed to apply the hotfix to.');
  const stagingRoot = await fsp.mkdtemp(path.join(os.tmpdir(), 'smvm-hotfix-'));
  const archive = path.join(stagingRoot, 'runtime-hotfix.zip');
  const unpacked = path.join(stagingRoot, 'unpacked');
  const backup = path.join(stagingRoot, 'backup');
  const label = 'Hook hotfix';
  try {
    report({ state: 'downloading', component: 'runtime', label, percent: 0 });
    const downloaded = await downloadToFile(entry.url, archive, (received, declared) => {
      report({ state: 'downloading', component: 'runtime', label,
        percent: declared ? Math.min(100, (received / declared) * 100) : 0, received, total: declared });
    });
    if (downloaded.sha256.toLowerCase() !== entry.sha256) {
      throw new Error(`${label} failed its checksum. Expected ${entry.sha256}, got ${downloaded.sha256}.`);
    }
    report({ state: 'installing', component: 'runtime', label });
    await fsp.mkdir(unpacked, { recursive: true });
    await extractZip(archive, unpacked);
    const files = (await listFiles(unpacked)).map((file) => file.rel);
    if (!files.length) throw new Error(`${label} is empty.`);
    const replaced = [];
    try {
      for (const relative of files) {
        // Never outside the runtime folder, whatever the archive says.
        const destination = path.resolve(target, relative);
        if (!destination.startsWith(path.resolve(target) + path.sep)) throw new Error(`${label} has a bad path: ${relative}`);
        if (fs.existsSync(destination)) {
          const saved = path.join(backup, relative);
          await fsp.mkdir(path.dirname(saved), { recursive: true });
          await fsp.copyFile(destination, saved);
          replaced.push({ destination, saved });
        } else {
          replaced.push({ destination, saved: null });
        }
        await fsp.mkdir(path.dirname(destination), { recursive: true });
        await fsp.copyFile(path.join(unpacked, relative), destination);
      }
    } catch (error) {
      for (const { destination, saved } of replaced.reverse()) {
        if (saved) await fsp.copyFile(saved, destination).catch(() => {});
        else await fsp.rm(destination, { force: true }).catch(() => {});
      }
      throw new Error(`${label} could not be applied (the previous runtime was kept). Close Counter-Strike 2 and try again.\n${error.message}`);
    }
    return { sha256: entry.sha256, bytes: downloaded.bytes, files: files.length };
  } finally {
    await fsp.rm(stagingRoot, { recursive: true, force: true }).catch(() => {});
  }
}

/**
 * A bundled launcher's hook-only update (see runtime-hotfix.cjs): when this app version's release
 * carries a newer runtime-hotfix.json, download just the runtime zip, verify it, and swap it in.
 * Callers hold the CS2-closed guard. Returns { state: 'none' | 'current' | 'installed', ... }.
 */
async function applyRuntimeHotfix(report = () => {}) {
  if (!isBundled()) return { state: 'none' };
  const tag = `v${app.getVersion()}`;
  let release;
  try {
    release = await fetchJson(`https://api.github.com/repos/${RELEASE_OWNER}/${RELEASE_REPO}/releases/tags/${tag}`);
  } catch (error) {
    if (/ 404 /.test(error.message)) return { state: 'none' };
    throw error;
  }
  const asset = (release.assets || []).find((candidate) => candidate.name === HOTFIX_ASSET);
  if (!asset) return { state: 'none' };
  const manifest = await fetchJson(asset.browser_download_url);
  const installed = readInstalledState();
  const current = installed.runtime || {};
  const hotfix = pickHotfix(manifest, app.getVersion(), current.hotfix);
  if (!hotfix) return { state: 'current', hotfix: current.hotfix || 0 };
  const result = await installRuntimeOverlay(hotfix, report);
  installed.runtime = { ...current, ...result, version: hotfix.version, hotfix: hotfix.serial };
  writeInstalledState(installed);
  report({ state: 'installed', version: hotfix.version });
  return { state: 'installed', hotfix: hotfix.serial, version: hotfix.version };
}

module.exports = {
  applyRuntimeHotfix,
  RELEASE_OWNER,
  RELEASE_REPO,
  payloadRoot,
  installedRuntimeDir,
  installedFxDir,
  localStatus,
  checkForPayload,
  installPayload,
  installBundled,
  isBundled,
  bundleRoot,
  recoverInterruptedInstalls,
};
