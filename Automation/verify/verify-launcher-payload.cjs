// End-to-end check of the launcher's runtime payload against the LIVE release repo.
//
// Exercises the real thing: GitHub release lookup, manifest fetch, download, SHA-256 verify,
// unpack, and the path lookups the launch path uses. Runs headless in Electron's main process
// with a throwaway userData directory, so it never touches a real install, never builds a DLL,
// and never starts CS2.
//
//   SourceMovieMakerLauncher\node_modules\.bin\electron Automation\verify\verify-launcher-payload.cjs
//
// Exits 0 when every check passes. Downloads ~70 MB.
const { app } = require('electron');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const sandbox = fs.mkdtempSync(path.join(os.tmpdir(), 'smvm-verify-'));
app.setPath('userData', sandbox);

const payload = require(path.resolve(
  __dirname, '..', '..', 'SourceMovieMakerLauncher', 'electron', 'runtime-payload.cjs',
));

function log(label, value) {
  process.stdout.write(`${label}: ${typeof value === 'string' ? value : JSON.stringify(value)}\n`);
}

app.whenReady().then(async () => {
  let failures = 0;
  const check = (label, condition, detail = '') => {
    process.stdout.write(`${condition ? 'PASS' : 'FAIL'}  ${label}${detail ? ` — ${detail}` : ''}\n`);
    if (!condition) failures += 1;
  };

  try {
    log('sandbox', sandbox);

    // 1. Nothing installed yet.
    const before = payload.localStatus();
    check('fresh install reports no runtime', before.components.runtime.present === false);
    check('fresh install reports no fx', before.components.fx.present === false);

    // 2. Live check against the release repo.
    const remote = await payload.checkForPayload();
    log('check state', remote.state);
    log('check version', remote.version);
    log('prerelease', remote.prerelease);
    log('outdated', remote.outdated);
    check('finds the published payload', remote.state === 'outdated', `state=${remote.state}`);
    check('resolves a PRE-release', remote.prerelease === true);
    check('both components flagged', (remote.outdated || []).length === 2);

    // 3. Real download + sha256 verify + extract.
    let lastPercent = -1;
    const result = await payload.installPayload((progress) => {
      if (progress.state === 'downloading') {
        const percent = Math.floor((progress.percent || 0) / 25) * 25;
        if (percent !== lastPercent) {
          lastPercent = percent;
          process.stdout.write(`      ${progress.label} ${percent}%\n`);
        }
      } else {
        lastPercent = -1;
        process.stdout.write(`      ${progress.state} ${progress.label || ''}\n`);
      }
    });
    check('install reports success', result.state === 'installed', `state=${result.state}${result.message ? ` ${result.message}` : ''}`);

    // 4. The files the launch path actually looks for.
    const runtimeDir = payload.installedRuntimeDir();
    const fxDir = payload.installedFxDir();
    check('installedRuntimeDir resolves', Boolean(runtimeDir), runtimeDir);
    check('installedFxDir resolves', Boolean(fxDir), fxDir);
    check('hook DLL present', fs.existsSync(path.join(runtimeDir, 'x64', 'AfxHookSource2.dll')));
    check('injector present', fs.existsSync(path.join(runtimeDir, 'x64', 'injector.exe')));
    check('shader resources present', fs.existsSync(path.join(runtimeDir, 'resources', 'shaders')));
    // The hook resolves these relative to its own module directory, so the exact nesting matters:
    // <runtime>\x64\<Tool>\<Tool>.exe. Without them the demo browser has no scoreboards and
    // legacy demos play unrepaired -- a payload can install cleanly and still be useless.
    check('demo info helper present',
      fs.existsSync(path.join(runtimeDir, 'x64', 'FilmmakerDemoInfo', 'FilmmakerDemoInfo.exe')));
    check('demo repair helper present',
      fs.existsSync(path.join(runtimeDir, 'x64', 'FilmmakerDemoRepair', 'FilmmakerDemoRepair.exe')));
    // A shipped ffmpeg.ini would carry a developer-local Path= and suppress the bundled fallback.
    check('no developer ffmpeg.ini shipped',
      !fs.existsSync(path.join(runtimeDir, 'ffmpeg', 'ffmpeg.ini')));
    check('fx gameinfo present', fs.existsSync(path.join(fxDir, 'gameinfo.gi')));
    check('fx particles present', fs.existsSync(path.join(fxDir, 'particles')));
    check('no pdb shipped', !fs.existsSync(path.join(runtimeDir, 'x64', 'AfxHookSource2.pdb')));

    const dllBytes = fs.statSync(path.join(runtimeDir, 'x64', 'AfxHookSource2.dll')).size;
    check('hook DLL is a real binary', dllBytes > 1000000, `${dllBytes} bytes`);

    // 5. Second check must now be a no-op.
    const after = await payload.checkForPayload();
    check('re-check reports current', after.state === 'current', `state=${after.state}`);
    check('nothing left outdated', (after.outdated || []).length === 0);

    // 6. Idempotent install.
    const again = await payload.installPayload(() => {});
    check('re-install is a no-op', again.state === 'current', `state=${again.state}`);
  } catch (error) {
    process.stdout.write(`FAIL  threw: ${error.stack}\n`);
    failures += 1;
  }

  process.stdout.write(`\n${failures === 0 ? 'ALL CHECKS PASSED' : `${failures} CHECK(S) FAILED`}\n`);
  // Best-effort: Electron can still hold handles under the sandbox at this point, and a failed
  // cleanup of a temp directory must not turn a passing verification into a failing one.
  try {
    fs.rmSync(sandbox, { recursive: true, force: true });
  } catch {
    process.stdout.write(`(left ${sandbox} behind)\n`);
  }
  app.exit(failures === 0 ? 0 : 1);
});
