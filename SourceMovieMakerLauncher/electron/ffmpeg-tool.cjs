// FFmpeg: find it, offer to install it, and make the hook able to resolve it.
//
// Without ffmpeg the Render workspace cannot encode anything and the NVENC probe reports the
// encoder as unavailable, so this is not an optional extra -- it is the difference between the
// render feature working and silently producing nothing.
//
// How the hook finds ffmpeg (AfxHookSource2/Filmmaker/Render/RenderEncoderDetect.cpp, matching
// Shared/AfxOutStreams.cpp):
//
//   1. <runtime>\ffmpeg\ffmpeg.ini  ->  [Ffmpeg] Path=<absolute path to ffmpeg.exe>
//   2. otherwise <runtime>\ffmpeg\bin\ffmpeg.exe
//
// where <runtime> is the hook DLL's directory with the trailing "x64" stripped. It never consults
// PATH. So putting ffmpeg on PATH alone does NOT make rendering work -- writing the ini does, and
// that is why installedFor()/writeIniFor() exist.
//
// Complete installers carry a verified FFmpeg build with its license and upstream source notice.
// Thin/portable builds can still fetch it on request when no usable local copy is available.
//
// This module owns ffmpeg end to end and nothing else. It does not launch CS2 and does not touch
// the runtime/FX payload beyond writing that one ini file.

const { app, net } = require('electron');
const crypto = require('node:crypto');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { execFile } = require('node:child_process');

// gyan.dev is one of the two Windows build sources ffmpeg.org itself links to, and it publishes a
// matching .sha256 next to each archive, so the download can be verified rather than trusted.
// "essentials" carries libx264/libx265 and the NVENC encoders the render pipeline probes for.
const DOWNLOAD_URL = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip';
const CHECKSUM_URL = `${DOWNLOAD_URL}.sha256`;

function installRoot() {
  // Beside the runtime payload, NOT inside it: installPayload() replaces <payload>/runtime
  // wholesale on every hook update, which would throw away a ~150 MB ffmpeg download each time.
  return path.join(app.getPath('userData'), 'payload', 'ffmpeg');
}

function installedExe() {
  return path.join(installRoot(), 'bin', 'ffmpeg.exe');
}

function runCapture(exe, args) {
  return new Promise((resolve) => {
    execFile(exe, args, { windowsHide: true, timeout: 10000 }, (error, stdout, stderr) => {
      resolve({ ok: !error, output: `${stdout || ''}${stderr || ''}` });
    });
  });
}

/** First line of `ffmpeg -version`, trimmed to something a status line can show. */
async function probeVersion(exe) {
  const result = await runCapture(exe, ['-hide_banner', '-version']);
  if (!result.ok) return '';
  const match = /^ffmpeg version (\S+)/m.exec(result.output);
  return match ? match[1] : '';
}

function pathDirectories() {
  return (process.env.PATH || '').split(';').map((entry) => entry.trim()).filter(Boolean);
}

/**
 * Everywhere ffmpeg plausibly already is, best first. Someone who installed it via winget or
 * chocolatey should never be asked to download it again. A manually chosen path (the Advanced
 * drawer's SELECT) outranks everything: the user pointing at a specific exe is the one signal
 * that is not a guess.
 */
function candidateExes(configuredExe) {
  const candidates = [];
  if (configuredExe) candidates.push(configuredExe);
  // Complete builds carry FFmpeg in the installed runtime, ahead of machine-wide guesses.
  const payload = require('./runtime-payload.cjs');
  const runtime = payload.installedRuntimeDir();
  if (runtime) candidates.push(path.join(runtime, 'ffmpeg', 'bin', 'ffmpeg.exe'));
  candidates.push(installedExe());
  for (const directory of pathDirectories()) {
    candidates.push(path.join(directory, 'ffmpeg.exe'));
  }
  const localAppData = process.env.LOCALAPPDATA || '';
  if (localAppData) {
    // winget's package layout nests the extracted build under a versioned folder, so this is a
    // shallow scan rather than a fixed path.
    const wingetRoot = path.join(localAppData, 'Microsoft', 'WinGet', 'Packages');
    try {
      for (const entry of fs.readdirSync(wingetRoot)) {
        if (!/ffmpeg/i.test(entry)) continue;
        const packageDir = path.join(wingetRoot, entry);
        for (const inner of fs.readdirSync(packageDir)) {
          candidates.push(path.join(packageDir, inner, 'bin', 'ffmpeg.exe'));
        }
      }
    } catch {
      // No winget packages directory: nothing to contribute.
    }
  }
  candidates.push('C:\\ProgramData\\chocolatey\\bin\\ffmpeg.exe');
  candidates.push('C:\\ffmpeg\\bin\\ffmpeg.exe');
  return [...new Set(candidates)];
}

function describeSource(exe, configuredExe) {
  if (configuredExe && exe === configuredExe) return 'chosen manually';
  if (exe === installedExe()) return 'installed by SourceMovieMaker';
  if (/WinGet/i.test(exe)) return 'winget';
  if (/chocolatey/i.test(exe)) return 'chocolatey';
  if (pathDirectories().some((directory) => path.join(directory, 'ffmpeg.exe') === exe)) return 'on PATH';
  return 'found on disk';
}

/** Where ffmpeg is, or that it is missing. Runs the binary, so a broken copy reports as missing. */
async function detect(configuredExe) {
  for (const candidate of candidateExes(configuredExe)) {
    if (!fs.existsSync(candidate)) continue;
    const version = await probeVersion(candidate);
    if (!version) continue;
    return {
      found: true,
      path: candidate,
      version,
      source: describeSource(candidate, configuredExe),
      managed: candidate === installedExe(),
      onPath: pathDirectories().some((d) => fs.existsSync(path.join(d, 'ffmpeg.exe'))),
    };
  }
  return { found: false, path: '', version: '', source: '', managed: false, onPath: false };
}

// ---- install ---------------------------------------------------------------------------------

async function fetchText(url) {
  const response = await net.fetch(url, {
    headers: { 'User-Agent': `SourceMovieMaker/${app.getVersion()}` },
  });
  if (!response.ok) throw new Error(`${url} responded ${response.status} ${response.statusText}.`);
  return response.text();
}

async function downloadTo(url, destination, onProgress) {
  const response = await net.fetch(url, {
    headers: { 'User-Agent': `SourceMovieMaker/${app.getVersion()}` },
  });
  if (!response.ok) throw new Error(`Download failed: ${response.status} ${response.statusText}.`);
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
  return hash.digest('hex');
}

// Same reasoning as runtime-payload.cjs: Windows ships bsdtar as tar.exe and it reads .zip, so no
// archive dependency is needed. System32 is spelled out so Git's GNU tar cannot win on PATH.
function systemTar() {
  const system32 = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe');
  return fs.existsSync(system32) ? system32 : 'tar';
}

function extractZip(archive, destination) {
  return new Promise((resolve, reject) => {
    execFile(systemTar(), ['-x', '-f', archive, '-C', destination],
      { windowsHide: true, maxBuffer: 8 * 1024 * 1024 }, (error, _stdout, stderr) => {
        if (error) reject(new Error(`Could not unpack ffmpeg: ${stderr || error.message}`));
        else resolve();
      });
  });
}

/**
 * Move a staged directory into place. rename() is atomic but fails with EXDEV when the staging
 * temp dir and the destination sit on different drives -- which is exactly the portable build
 * run from a USB stick or a second disk, where userData lives beside the exe while %TEMP% stays
 * on C:. Fall back to copy+delete for that case only; other failures stay loud.
 */
async function moveDirectory(source, target) {
  try {
    await fsp.rename(source, target);
  } catch (error) {
    if (error.code !== 'EXDEV') throw error;
    await fsp.cp(source, target, { recursive: true });
    await fsp.rm(source, { recursive: true, force: true }).catch(() => {});
  }
}

/** The archive nests everything under ffmpeg-<version>-essentials_build/; find that bin/. */
async function findExtractedBin(root) {
  const entries = await fsp.readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    if (!entry.isDirectory()) continue;
    const candidate = path.join(root, entry.name, 'bin', 'ffmpeg.exe');
    if (fs.existsSync(candidate)) return path.join(root, entry.name);
  }
  if (fs.existsSync(path.join(root, 'bin', 'ffmpeg.exe'))) return root;
  return '';
}

let installing = null;

/**
 * Download, verify and install ffmpeg into the launcher's own data directory. No admin rights
 * needed, and uninstalling the launcher takes it with it.
 */
async function install(report) {
  if (installing) throw new Error('FFmpeg is already installing.');
  installing = (async () => {
    const staging = await fsp.mkdtemp(path.join(os.tmpdir(), 'smvm-ffmpeg-'));
    const archive = path.join(staging, 'ffmpeg.zip');
    const unpacked = path.join(staging, 'unpacked');

    try {
      report({ state: 'checking' });
      // Published as "<hex> *<filename>" or bare hex depending on the build; take the hex.
      let expected = '';
      try {
        const text = await fetchText(CHECKSUM_URL);
        expected = ((/[0-9a-f]{64}/i.exec(text) || [''])[0] || '').toLowerCase();
      } catch {
        // Checksum unavailable. Not fatal -- the install is still gated on ffmpeg actually
        // running below -- but say so rather than implying it was verified.
        report({ state: 'unverified' });
      }

      report({ state: 'downloading', percent: 0 });
      const digest = await downloadTo(DOWNLOAD_URL, archive, (received, declared) => {
        report({
          state: 'downloading',
          percent: declared ? Math.min(100, (received / declared) * 100) : 0,
          received,
          total: declared,
        });
      });

      if (expected && digest.toLowerCase() !== expected) {
        throw new Error(`The FFmpeg download failed its checksum (expected ${expected}).`);
      }

      report({ state: 'installing' });
      await fsp.mkdir(unpacked, { recursive: true });
      await extractZip(archive, unpacked);

      const buildRoot = await findExtractedBin(unpacked);
      if (!buildRoot) throw new Error('The FFmpeg archive did not contain bin\\ffmpeg.exe.');

      const target = installRoot();
      try {
        await fsp.rm(target, { recursive: true, force: true });
      } catch (error) {
        throw new Error(
          `Could not replace ${target}.\nClose anything using FFmpeg and try again.\n${error.message}`,
        );
      }
      await fsp.mkdir(path.dirname(target), { recursive: true });
      await moveDirectory(buildRoot, target);

      // Gate on the binary actually running. A checksum only proves we downloaded what was
      // published; this proves the thing we installed executes on this machine.
      const version = await probeVersion(installedExe());
      if (!version) throw new Error('FFmpeg installed but would not run.');

      report({ state: 'installed', version });
      return { ok: true, path: installedExe(), version };
    } finally {
      await fsp.rm(staging, { recursive: true, force: true }).catch(() => {});
    }
  })();

  try {
    return await installing;
  } finally {
    installing = null;
  }
}

// ---- making the hook able to find it ---------------------------------------------------------

/**
 * Point a runtime install at an ffmpeg.exe by writing <runtime>\ffmpeg\ffmpeg.ini.
 *
 * Must be re-run after every runtime payload install: that replaces <runtime> wholesale, taking
 * the ini with it. runtime-payload.cjs does not call this itself -- main.cjs sequences the two,
 * so neither module has to know about the other.
 */
async function writeIniFor(runtimeDir, ffmpegExe) {
  if (!runtimeDir || !ffmpegExe) return { ok: false, message: 'Nothing to point at.' };
  const iniDir = path.join(runtimeDir, 'ffmpeg');
  const ini = path.join(iniDir, 'ffmpeg.ini');
  await fsp.mkdir(iniDir, { recursive: true });
  // GetPrivateProfileStringW reads this, so it must be a plain INI. No quoting around the path:
  // the Win32 profile API takes the rest of the line verbatim, and quotes would become part of
  // the value.
  await fsp.writeFile(ini, `[Ffmpeg]\r\nPath=${ffmpegExe}\r\n`, 'utf8');
  return { ok: true, ini, path: ffmpegExe };
}

/**
 * Append our bin directory to the USER PATH (HKCU\Environment), never the machine one -- that
 * would need admin and would affect other accounts.
 *
 * Purely a convenience for using ffmpeg from a terminal. The hook does not read PATH, so this is
 * never what makes rendering work; writeIniFor() is.
 */
function addToUserPath(binDir) {
  return new Promise((resolve) => {
    if (!binDir) {
      resolve({ ok: false, message: 'Nothing to add.' });
      return;
    }
    // setx truncates at 1024 characters and would silently maim a long PATH, so this goes through
    // PowerShell's environment API instead, which does not.
    const script = `
$ErrorActionPreference = 'Stop'
$dir = ${JSON.stringify(binDir)}
$current = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($null -eq $current) { $current = '' }
$parts = $current -split ';' | Where-Object { $_ -ne '' }
if ($parts -contains $dir) { Write-Output 'already-present'; exit 0 }
$next = (@($parts) + $dir) -join ';'
[Environment]::SetEnvironmentVariable('Path', $next, 'User')
Write-Output 'added'`;
    execFile('powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
      { windowsHide: true, timeout: 20000 }, (error, stdout, stderr) => {
        if (error) {
          resolve({ ok: false, message: (stderr || error.message).trim() });
          return;
        }
        const already = /already-present/.test(stdout);
        // Only new processes inherit the change, so this process keeps its own PATH in step --
        // otherwise detect() would not see it until the launcher restarts.
        if (!already && !pathDirectories().includes(binDir)) {
          process.env.PATH = `${process.env.PATH || ''};${binDir}`;
        }
        resolve({ ok: true, already, dir: binDir });
      });
  });
}

module.exports = {
  DOWNLOAD_URL,
  installRoot,
  installedExe,
  detect,
  probeVersion,
  install,
  writeIniFor,
  addToUserPath,
};
