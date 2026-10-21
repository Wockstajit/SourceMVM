const {
  app,
  BrowserWindow,
  dialog,
  ipcMain,
  protocol,
  net,
  screen,
  shell,
} = require('electron');
const { autoUpdater } = require('electron-updater');
const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { execFileSync } = require('node:child_process');
const runtimePayload = require('./runtime-payload.cjs');
const ffmpegTool = require('./ffmpeg-tool.cjs');
const dlssTool = require('./dlss-tool.cjs');
const { createUpdateService } = require('./update-service.cjs');
const { findCs2Processes, installBlocker } = require('./cs2-process.cjs');
const { runLaunchHost } = require('./launch-host.cjs');
const { resetSavesIfNeeded, SAVE_EPOCH } = require('./save-reset.cjs');

const APP_NAME = 'SourceMovieMaker';
const STEAM_REGISTRY_KEY = 'HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam';

let mainWindow = null;
let settings = null;
let launchedCs2ProcessId = 0;
let startupWatchPending = false;
let launchInProgress = false;
let updateService = null;

protocol.registerSchemesAsPrivileged([
  {
    scheme: 'smvm-media',
    privileges: {
      standard: true,
      secure: true,
      supportFetchAPI: true,
      stream: true,
      bypassCSP: true,
    },
  },
]);

app.setAppUserModelId('com.sourcemvm.launcher');
app.setName(APP_NAME);

// ---- Portable build --------------------------------------------------------------------------
// Two ways to run portable. electron-builder's portable target sets PORTABLE_EXECUTABLE_DIR to the
// folder the single-file exe was run from. The portable zip (scripts/build-portable-zip.ps1) is
// the unpacked app plus an empty portable.flag beside the exe. Either way we keep settings and the
// downloaded runtime next to the exe instead of in %APPDATA%, so the whole thing really does
// travel on a USB stick -- which is the only reason to ship a portable build at all.
function portableFolder() {
  if (process.env.PORTABLE_EXECUTABLE_DIR) return process.env.PORTABLE_EXECUTABLE_DIR;
  if (!app.isPackaged) return '';
  const folder = path.dirname(process.execPath);
  return fs.existsSync(path.join(folder, 'portable.flag')) ? folder : '';
}
const portableDir = portableFolder();
const isPortable = Boolean(portableDir);
if (isPortable) {
  // app.setPath is documented to throw for a missing folder, and a fresh zip extraction has none.
  const dataDir = path.join(portableDir, 'SourceMovieMaker-data');
  fs.mkdirSync(dataDir, { recursive: true });
  app.setPath('userData', dataDir);
}

function defaultSettings() {
  return {
    cs2Exe: detectCs2Exe() || '',
    runtimeRoot: '',
    hookDllPath: '',
    injectorPath: '',
    mmcfgEnabled: false,
    mmcfg: '',
    devMode: false,
    customLaunchOptions: '-console +sv_lan 1',
    // A manually chosen ffmpeg.exe. Empty means "detect": managed install, PATH, winget, choco.
    ffmpegPath: '',
    // DLSS 5 through ReShade, started from the launcher's private CS2 copy (dlss-tool.cjs). Off
    // means Steam's own cs2.exe, which never has ReShade beside it.
    dlss5Enabled: false,
    // The first-launch "install DLSS 5?" question has been answered (yes or no), so it is not
    // asked again. A self-contained build asks it; the downloadable one has nothing to install.
    dlssPromptAnswered: false,
    // Check the update server on every start (launcher update, runtime hotfix, payload). Off by
    // default: the launcher contacts nothing on its own, and GO works without internet. The
    // UPDATES button still checks once when pressed.
    autoUpdate: false,
  };
}

// ---- Workshop Tools ("dev mode") -------------------------------------------------------------
// Dev mode launches CS2 the way Steam's own "Counter-Strike 2 Workshop Tools" entry does, so the
// tools/asset pipeline is available in the session. Two flags, both load-bearing:
//
//   -tools  what actually starts the toolframework.
//   -dx11   engine2 picks between rendersystemdx11 / rendersystemvulkan / rendersystemempty, and
//           Steam's own tools entry passes -vulkan. Vulkan would bypass EVERY D3D11 hook this
//           project owns, so dev mode pins DX11. Do not "modernise" this to -vulkan.
//
// Kept identical to Automation/launch/launch-cs2-tools.ps1, the agent-facing entry point for the
// same mode. If one changes, change both.
const WORKSHOP_TOOLS_APP_ID = 2279721;

// The DLC installs into the SAME directory as the base game, so there is no separate folder to
// look for -- whether the tool binaries are on disk is the only reliable local check.
function workshopToolsProbePaths(cs2Exe) {
  if (!cs2Exe) return [];
  // cs2.exe lives at <install>/game/bin/win64/cs2.exe
  const installDir = path.resolve(path.dirname(cs2Exe), '..', '..', '..');
  return [
    path.join(installDir, 'game', 'bin', 'win64', 'tools', 'hammer.dll'),
    path.join(installDir, 'game', 'bin', 'win64', 'csgocfg.exe'),
  ];
}

function workshopToolsStatus(cs2Exe) {
  const probes = workshopToolsProbePaths(cs2Exe);
  // No cs2.exe yet: report "not installed" but flag it as unknown, so the UI can say "pick CS2
  // first" instead of accusing the user of missing a DLC we never actually looked for.
  if (probes.length === 0) {
    return { installed: false, unknown: true, missing: [], appId: WORKSHOP_TOOLS_APP_ID };
  }
  const missing = probes.filter((candidate) => !fs.existsSync(candidate));
  return {
    installed: missing.length === 0,
    unknown: false,
    missing,
    appId: WORKSHOP_TOOLS_APP_ID,
  };
}

function isProcessRunning(processId) {
  if (!processId) return false;
  try {
    process.kill(processId, 0);
    return true;
  } catch {
    if (launchedCs2ProcessId === processId) launchedCs2ProcessId = 0;
    return false;
  }
}

function configPath() {
  return path.join(app.getPath('userData'), 'launcher.json');
}

function loadSettings() {
  const defaults = defaultSettings();
  try {
    const saved = JSON.parse(fs.readFileSync(configPath(), 'utf8'));
    // Only current keys survive, so settings a newer launcher dropped are not carried forever.
    const current = { ...defaults };
    for (const key of Object.keys(defaults)) {
      if (Object.prototype.hasOwnProperty.call(saved, key)) current[key] = saved[key];
    }
    return current;
  } catch {
    return defaults;
  }
}

function saveSettings(next) {
  const defaults = defaultSettings();
  settings = { ...defaults };
  for (const key of Object.keys(defaults)) {
    if (next && Object.prototype.hasOwnProperty.call(next, key)) settings[key] = next[key];
  }
  fs.mkdirSync(path.dirname(configPath()), { recursive: true });
  fs.writeFileSync(configPath(), `${JSON.stringify(settings, null, 2)}\n`, 'utf8');
  return settings;
}

function readSteamInstallPath() {
  try {
    const output = execFileSync(
      'reg.exe',
      ['query', STEAM_REGISTRY_KEY, '/v', 'InstallPath'],
      { encoding: 'utf8', windowsHide: true },
    );
    const match = output.match(/InstallPath\s+REG_SZ\s+(.+)/i);
    if (match) return match[1].trim();
  } catch {
    // Steam may be installed outside the registry location.
  }
  return 'C:\\Program Files (x86)\\Steam';
}

function steamLibraries(steamPath) {
  const result = [steamPath];
  const vdfPath = path.join(steamPath, 'steamapps', 'libraryfolders.vdf');
  try {
    const vdf = fs.readFileSync(vdfPath, 'utf8');
    for (const match of vdf.matchAll(/"path"\s+"([^"]+)"/gi)) {
      const library = match[1].replaceAll('\\\\', '\\');
      if (!result.includes(library)) result.push(library);
    }
  } catch {
    // A missing libraryfolders.vdf is normal on a fresh Steam install.
  }
  return result;
}

function detectCs2Exe() {
  const candidates = [];
  for (const library of steamLibraries(readSteamInstallPath())) {
    candidates.push(path.join(
      library,
      'steamapps',
      'common',
      'Counter-Strike Global Offensive',
      'game',
      'bin',
      'win64',
      'cs2.exe',
    ));
  }
  return candidates.find((candidate) => fs.existsSync(candidate)) || '';
}

// The launcher runs exactly what an end user runs: the runtime CHECK FOR UPDATES installed under
// userData. It never searches a repository build tree. A developer who wants a local build in the
// launcher sets RUNTIME FOLDER explicitly; automation passes --hook/--injector itself.
function runtimeCandidate(relativePath, configuredRoot) {
  const payloadRuntime = runtimePayload.installedRuntimeDir();
  const candidates = [
    ...(configuredRoot ? [path.join(configuredRoot, relativePath)] : []),
    ...(payloadRuntime ? [path.join(payloadRuntime, relativePath)] : []),
  ];
  return candidates.find((candidate) => fs.existsSync(candidate))
    || candidates[0]
    || path.join(runtimePayload.payloadRoot(), 'runtime', relativePath);
}

function startupMountCandidates(current) {
  const candidates = [];
  if (current.mmcfgEnabled && current.mmcfg) candidates.push(current.mmcfg);
  if (current.runtimeRoot) {
    candidates.push(path.resolve(current.runtimeRoot, '..', 'fx', 'source_mvm_fx'));
    candidates.push(path.join(current.runtimeRoot, 'fx', 'source_mvm_fx'));
  }
  // The FX pack installed by CHECK FOR UPDATES.
  const payloadFx = runtimePayload.installedFxDir();
  if (payloadFx) candidates.push(payloadFx);
  return [...new Set(candidates)];
}

function startupMountPath(current) {
  const candidates = startupMountCandidates(current);
  return candidates.find((candidate) => fs.existsSync(candidate)) || '';
}

function hostPath() {
  const candidates = [
    path.join(process.resourcesPath, 'host', 'SourceMovieMakerHost.exe'),
    path.resolve(__dirname, '..', 'host', 'build', 'SourceMovieMakerHost.exe'),
  ];
  return candidates.find((candidate) => fs.existsSync(candidate)) || candidates[0];
}

function introVideoPath() {
  const candidates = [
    path.join(process.resourcesPath, 'media', 'source-mvm-intro.mp4'),
    path.resolve(__dirname, '..', 'public', 'source-mvm-intro.mp4'),
    path.resolve(__dirname, '..', 'renderer-dist', 'source-mvm-intro.mp4'),
  ];
  return candidates.find((candidate) => fs.existsSync(candidate)) || '';
}

function introVideoUrl() {
  return 'smvm-media://intro';
}

function registerMediaProtocol() {
  protocol.handle('smvm-media', async () => {
    const filePath = introVideoPath();
    if (!filePath) {
      return new Response('Intro video missing', { status: 404 });
    }
    return net.fetch(pathToFileURL(filePath).href);
  });
}

function buildCommandLine(current) {
  let commandLine = '-steam -insecure';
  // Dev mode first, so it reads like Steam's own tools entry and so anything the user puts in
  // customLaunchOptions still lands after it (their override wins -- but the launcher itself
  // never chooses Vulkan; see WORKSHOP_TOOLS_APP_ID above for why).
  if (current.devMode) commandLine += ' -tools -dx11';
  if (current.mmcfgEnabled) commandLine += ' -afxDisableSteamStorage';
  if (current.customLaunchOptions) commandLine += ` ${current.customLaunchOptions.trim()}`;
  if (!/(?:^|\s)-netconport(?:\s|$)/i.test(commandLine)) commandLine += ' -netconport 29010';
  return commandLine;
}

function environmentFor(current) {
  const env = { ...process.env };
  const steamPath = readSteamInstallPath();
  env.SteamPath = steamPath;
  env.SteamClientLaunch = '1';
  env.SteamGameId = '730';
  env.SteamAppId = '730';
  env.SteamOverlayGameId = '730';
  const startupMount = startupMountPath(current);
  if (startupMount) env.USRLOCALCSGO = startupMount;
  else delete env.USRLOCALCSGO;
  return env;
}

function argValue(argv, ...names) {
  for (let index = 0; index < argv.length; index += 1) {
    if (names.includes(argv[index])) return argv[index + 1] || '';
  }
  return '';
}

function cliSettings(argv) {
  const current = { ...settings };
  const cs2Exe = argValue(argv, '--cs2', '--cs2-exe', '-cs2Exe', '-programPath');
  const hook = argValue(argv, '--hook', '--hook-dll', '-hookDllPath');
  const injector = argValue(argv, '--injector', '-injectorPath');
  const commandLine = argValue(argv, '--cmd-line', '-cmdLine');
  const mmcfg = argValue(argv, '--mmcfg', '-mmcfg');
  const envValues = [];
  for (let index = 0; index < argv.length; index += 1) {
    if (argv[index] === '--env' || argv[index] === '-addEnv') {
      if (argv[index + 1]) envValues.push(argv[index + 1]);
      index += 1;
    }
  }
  // Explicit both ways, so a headless run can force dev mode ON for a tools session or OFF for a
  // normal render without depending on whatever the GUI last saved.
  if (argv.includes('--dev') || argv.includes('--dev-mode')) current.devMode = true;
  if (argv.includes('--no-dev') || argv.includes('--no-dev-mode')) current.devMode = false;
  if (cs2Exe) current.cs2Exe = cs2Exe;
  if (hook) current.hookDllPath = hook;
  if (injector) current.injectorPath = injector;
  if (mmcfg) {
    current.mmcfgEnabled = true;
    current.mmcfg = mmcfg;
  }
  return { current, commandLine, envValues };
}

// ---- Where things actually are -----------------------------------------------------------
// The UI shows "Automatic" for anything not explicitly overridden, which is honest but useless
// when you want to know WHICH file is about to be injected. This reports the path each setting
// really resolves to, so the drawer can show it and offer to open it.

function pathEntry(target, kind, overridden) {
  return {
    path: target || '',
    kind,
    overridden: Boolean(overridden),
    exists: Boolean(target) && fs.existsSync(target),
  };
}

function resolvedPaths(current) {
  const active = current || settings || defaultSettings();
  const hook = active.hookDllPath
    || runtimeCandidate(path.join('x64', 'AfxHookSource2.dll'), active.runtimeRoot);
  const injector = active.injectorPath
    || runtimeCandidate(path.join('x64', 'injector.exe'), active.runtimeRoot);
  // The runtime root is the folder two levels above the hook (<root>/x64/AfxHookSource2.dll),
  // which is what the user would actually want opened.
  const runtimeRoot = active.runtimeRoot
    || (hook ? path.dirname(path.dirname(hook)) : '');

  return {
    cs2Exe: pathEntry(active.cs2Exe, 'file', Boolean(active.cs2Exe)),
    runtimeRoot: pathEntry(runtimeRoot, 'directory', Boolean(active.runtimeRoot)),
    hookDll: pathEntry(hook, 'file', Boolean(active.hookDllPath)),
    injector: pathEntry(injector, 'file', Boolean(active.injectorPath)),
    fxPack: pathEntry(startupMountPath(active), 'directory', Boolean(active.mmcfgEnabled && active.mmcfg)),
    installRoot: pathEntry(runtimePayload.payloadRoot(), 'directory', false),
    appData: pathEntry(app.getPath('userData'), 'directory', false),
    data: dataLocations(),
  };
}

// Where the hook saves things. AfxHookSource2 roots all of it at %APPDATA%\HLAE (hlaeFolder.cpp,
// GetHlaeRoamingAppDataFolderW); a portable launcher does not move it, because the hook decides.
function hlaeDataRoot() {
  return path.join(app.getPath('appData'), 'HLAE');
}

const DATA_LOCATIONS = [
  { id: 'hlae', label: 'All MVM data', detail: 'Root of everything below', parts: [], kind: 'directory' },
  { id: 'projects', label: 'Demo projects', detail: 'project.mvm per demo (camera paths, shots), repaired and keyframed demo copies', parts: ['source_mvm', 'demos'], kind: 'directory' },
  { id: 'effects', label: 'Effects settings', detail: 'Particle FX modes and swap rules (filmmaker_fx.json)', parts: ['filmmaker_fx.json'], kind: 'file' },
  { id: 'settings', label: 'Filmmaker settings', detail: 'In-game settings (filmmaker_settings.json)', parts: ['filmmaker_settings.json'], kind: 'file' },
  { id: 'environment', label: 'Environment', detail: 'Environment presets per map; its size includes the map data cache', parts: ['source_mvm', 'environment'], kind: 'directory' },
  { id: 'mapdata', label: 'Map data cache', detail: 'Lighting and shadow geometry, rebuilt automatically', parts: ['source_mvm', 'environment', 'occlusion'], kind: 'directory' },
  { id: 'lights', label: 'Relight lights', detail: 'Lights placed in the light editor', parts: ['source_mvm', 'relight'], kind: 'directory' },
  { id: 'props', label: 'Props', detail: 'Props placed per map', parts: ['source_mvm', 'props'], kind: 'directory' },
  { id: 'skyboxes', label: 'Skyboxes', detail: 'Imported skybox library', parts: ['filmmaker', 'skyboxes'], kind: 'directory' },
  { id: 'shaders', label: 'Shader cache', detail: 'Compiled lighting shaders, rebuilt automatically', parts: ['source_mvm', 'shader-cache'], kind: 'directory' },
  { id: 'logs', label: 'Debug logs', detail: 'mvm_debug logs, safe to delete', parts: ['debuglogs'], kind: 'directory' },
];

function dataLocations() {
  const root = hlaeDataRoot();
  return DATA_LOCATIONS.map(({ id, label, detail, parts, kind }) => ({
    id,
    label,
    detail,
    ...pathEntry(path.join(root, ...parts), kind, false),
  }));
}

// Bytes on disk under `target`. Async and best effort: an unreadable entry counts as zero.
async function pathSize(target) {
  let stat;
  try { stat = await fs.promises.stat(target); } catch { return 0; }
  if (!stat.isDirectory()) return stat.size;
  let total = 0;
  const pending = [target];
  while (pending.length) {
    const directory = pending.pop();
    let entries = [];
    try { entries = await fs.promises.readdir(directory, { withFileTypes: true }); } catch { continue; }
    for (const entry of entries) {
      const full = path.join(directory, entry.name);
      if (entry.isDirectory()) pending.push(full);
      else if (entry.isFile()) {
        try { total += (await fs.promises.stat(full)).size; } catch { /* vanished mid-walk */ }
      }
    }
  }
  return total;
}

// Only paths the launcher itself reports may be measured; the renderer cannot walk arbitrary disks.
async function pathSizes(targets) {
  const allowed = new Set([
    ...dataLocations().map((entry) => entry.path),
    runtimePayload.payloadRoot(),
    app.getPath('userData'),
  ]);
  const result = {};
  for (const target of Array.isArray(targets) ? targets : []) {
    if (typeof target === 'string' && allowed.has(target)) result[target] = await pathSize(target);
  }
  return result;
}

async function revealPath(target) {
  if (!target) return { ok: false, message: 'Nothing to open yet.' };
  try {
    if (fs.existsSync(target)) {
      if (fs.statSync(target).isDirectory()) {
        const failure = await shell.openPath(target);
        return failure ? { ok: false, message: failure } : { ok: true };
      }
      shell.showItemInFolder(target);
      return { ok: true };
    }
    // Not there yet -- the runtime folder before the first download, say. Open the nearest
    // ancestor that does exist rather than failing outright, so the button still goes somewhere.
    let ancestor = path.dirname(target);
    while (!fs.existsSync(ancestor) && path.dirname(ancestor) !== ancestor) {
      ancestor = path.dirname(ancestor);
    }
    if (!fs.existsSync(ancestor)) return { ok: false, message: 'That path does not exist yet.' };
    const failure = await shell.openPath(ancestor);
    return failure ? { ok: false, message: failure } : { ok: true, fellBack: true };
  } catch (error) {
    return { ok: false, message: error.message };
  }
}

// ---- Advanced drawer sizing ----------------------------------------------------------------
// The launch strip is intentionally a 600x300 window, which leaves the drawer about 250px of
// body -- not enough for the settings without scrolling through them. Grow the window while the
// drawer is open and put it back afterwards.
const DRAWER_HEIGHT = 600;
let boundsBeforeDrawer = null;

function setDrawerOpen(open) {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (open) {
    const bounds = mainWindow.getBounds();
    if (bounds.height >= DRAWER_HEIGHT) return;
    const { workArea } = screen.getDisplayMatching(bounds);
    const height = Math.min(DRAWER_HEIGHT, workArea.height);
    // Keep the grown window on-screen: without this it grows straight off the bottom edge when
    // the strip is sitting low.
    const y = Math.max(workArea.y, Math.min(bounds.y, workArea.y + workArea.height - height));
    boundsBeforeDrawer = bounds;
    mainWindow.setBounds({ ...bounds, y, height }, true);
    return;
  }
  if (!boundsBeforeDrawer) return;
  // Only undo our own change. If the user resized the window while the drawer was open, that is
  // the size they chose and we leave it alone.
  if (Math.abs(mainWindow.getBounds().height - DRAWER_HEIGHT) < 2) {
    mainWindow.setBounds(boundsBeforeDrawer, true);
  }
  boundsBeforeDrawer = null;
}

function sendUpdateStatus(payload) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('update-status', payload);
  }
}

function sendPayloadStatus(payload) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('payload-status', payload);
  }
}

function sendFfmpegStatus(payload) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('ffmpeg-status', payload);
  }
}

/**
 * Point the installed runtime at whatever ffmpeg this machine has.
 *
 * Sequenced here rather than inside either module: runtime-payload owns the hook, ffmpeg-tool
 * owns ffmpeg, and neither should have to know the other exists. Must run after EVERY runtime
 * install -- that replaces <runtime> wholesale, deleting the ini with it.
 */
async function linkFfmpegToRuntime() {
  const runtimeDir = runtimePayload.installedRuntimeDir();
  if (!runtimeDir) return { ok: false, message: 'No runtime installed yet.' };
  const found = await ffmpegTool.detect(settings?.ffmpegPath || '');
  if (!found.found) return { ok: false, message: 'No FFmpeg found to link.' };
  return ffmpegTool.writeIniFor(runtimeDir, found.path);
}

function configureUpdater() {
  autoUpdater.autoDownload = false;
  // Install only through the guarded restart; closing during a failed update must not bypass it.
  autoUpdater.autoInstallOnAppQuit = false;
  autoUpdater.allowPrerelease = true;
  autoUpdater.allowDowngrade = false;
  autoUpdater.setFeedURL({ provider: 'github', owner: runtimePayload.RELEASE_OWNER,
    repo: runtimePayload.RELEASE_REPO });
  autoUpdater.on('download-progress', progress => updateService.progress(progress.percent));
  autoUpdater.on('error', error => updateService.fail(error));
  updateService = createUpdateService({
    updater: autoUpdater,
    packaged: app.isPackaged,
    portable: isPortable,
    report: sendUpdateStatus,
    automaticUpdates: () => Boolean(settings && settings.autoUpdate),
    log: message => console.warn(message),
    canInstall: () => installBlocker({
      launching: launchInProgress,
      launchedPid: isProcessRunning(launchedCs2ProcessId) ? launchedCs2ProcessId : 0,
      found: launchInProgress ? {} : findCs2Processes(execFileSync),
    }),
    loadReleases: async () => {
      const url = `https://api.github.com/repos/${runtimePayload.RELEASE_OWNER}/${runtimePayload.RELEASE_REPO}/releases?per_page=20`;
      const response = await net.fetch(url, { signal: AbortSignal.timeout(15000), headers: {
        Accept: 'application/vnd.github+json', 'User-Agent': `SourceMovieMaker/${app.getVersion()}`,
      } });
      if (!response.ok) throw new Error(`Update server responded ${response.status}.`);
      return response.json();
    },
    // online is false when automatic updates are off or the update server could not be reached:
    // then only local work happens (the bundle the installer carries, the FFmpeg link).
    prepareRuntime: async ({ online }) => {
      // A save epoch older than this release: delete SOURCE:MVM's saved state once (save-reset.cjs).
      // Runs under the same CS2-closed guard as the runtime install.
      const saves = await resetSavesIfNeeded(path.join(app.getPath('appData'), 'HLAE'));
      if (saves.reset) console.log(`SOURCE:MVM saves reset (epoch ${SAVE_EPOCH}): ${saves.removed.join(', ') || 'nothing to remove'}`);
      // A runtime or FX update the launcher closed part way through is put right first, bundled
      // or thin and online or not, so the checks below see the install that is really there.
      await runtimePayload.recoverInterruptedInstalls();
      if (runtimePayload.isBundled()) {
        await runtimePayload.installBundled(sendPayloadStatus);
        // Hook-only fixes ship as a runtime hotfix on this version's release, not a new installer.
        // Never fatal: without it the installed runtime still works.
        if (online) {
          try {
            await runtimePayload.applyRuntimeHotfix(sendPayloadStatus);
          } catch (error) {
            console.warn('runtime hotfix check failed:', error);
            sendPayloadStatus({ state: 'hotfix-failed', message: `Runtime hotfix not installed: ${error.message}` });
          }
        }
      } else if (online) {
        // A thin installer downloads its runtime. Offline with one already installed, that one
        // is used; with none, the check below names the button that downloads it.
        try {
          const check = await runtimePayload.checkForPayload();
          if (check.state === 'outdated') await runtimePayload.installPayload(sendPayloadStatus);
        } catch (error) {
          console.warn('runtime payload check failed:', error);
        }
      }
      await linkFfmpegToRuntime();
      const hook = settings.hookDllPath
        || runtimeCandidate(path.join('x64', 'AfxHookSource2.dll'), settings.runtimeRoot);
      const injector = settings.injectorPath
        || runtimeCandidate(path.join('x64', 'injector.exe'), settings.runtimeRoot);
      if (!fs.existsSync(hook) || !fs.existsSync(injector)) {
        throw new Error('No runtime is installed yet. Press INSTALL RUNTIME to download it (internet needed), or install the complete setup exe.');
      }
    },
  });
}
// How long the host keeps watching CS2 after the launch has already been reported as started.
// Startup to the main menu fits well inside this; a later exit is the user quitting.
const STARTUP_WATCH_MS = 30000;

function reportStartupExit(message, processId) {
  if (launchedCs2ProcessId === processId) launchedCs2ProcessId = 0;
  mainWindow?.webContents.send('cs2-startup-exit', { message });
}

// watchStartup: keep the host watching for an early exit and report it to the window.
async function launchWithSettings(current, commandLineOverride, extraEnv = [], watchStartup = false) {
  if (!current.cs2Exe || !fs.existsSync(current.cs2Exe)) {
    throw new Error('Select a valid cs2.exe before launching.');
  }

  const hook = current.hookDllPath
    || runtimeCandidate(path.join('x64', 'AfxHookSource2.dll'), current.runtimeRoot);
  const injector = current.injectorPath
    || runtimeCandidate(path.join('x64', 'injector.exe'), current.runtimeRoot);
  const host = hostPath();
  // The hook and injector arrive as a downloaded payload, not in the installer, so "missing" is
  // the normal state on a fresh install rather than a broken one. Say what to press.
  if (!fs.existsSync(hook) || !fs.existsSync(injector)) {
    // A self-contained build installs its own payload; make sure that has happened.
    if (runtimePayload.isBundled()) await runtimePayload.installBundled(sendPayloadStatus);
    const local = runtimePayload.localStatus();
    if (!local.components.runtime.present) {
      throw new Error(
        'The SOURCE:MVM runtime is not installed yet.\n'
        + 'Press CHECK FOR UPDATES to download the hook and FX pack.',
      );
    }
  }
  for (const required of [
    ['launcher host', host],
    ['AfxHookSource2.dll', hook],
    ['x64 injector', injector],
  ]) {
    if (!fs.existsSync(required[1])) {
      throw new Error(`${required[0]} was not found:\n${required[1]}`);
    }
  }

  // Dev mode without the DLC starts CS2 and then fails somewhere far less legible, so it is
  // caught here with the name of the thing to install. Checked at LAUNCH, not only in the UI:
  // the headless/CLI path never sees the toggle's warning.
  if (current.devMode) {
    const tools = workshopToolsStatus(current.cs2Exe);
    if (!tools.installed) {
      throw new Error(
        'Dev mode needs the Counter-Strike 2 Workshop Tools, which are not installed.\n'
        + 'In Steam: Library > Counter-Strike 2 > Manage > Properties > DLC,\n'
        + `or open  steam://install/${tools.appId}\n\n`
        + `Missing: ${tools.missing.join('\n         ')}`,
      );
    }
  }

  // DLSS 5 starts the private copy (ReShade beside it); everything else starts Steam's cs2.exe,
  // which never has ReShade beside it. The private binaries are re-synced from Steam first, so a
  // CS2 update never leaves them a build behind the shared assets.
  const cs2Exe = current.dlss5Enabled ? dlssTool.prepareLaunch(current.cs2Exe) : current.cs2Exe;

  const args = [
    '--cs2',
    cs2Exe,
    '--hook',
    hook,
    '--injector',
    injector,
    // REQUIRED by the host: it validates the Steam directory before it will create the process,
    // and refuses with "Steam install directory was not found:" (with nothing after the colon)
    // when this is missing. The host has taken this argument since babc126b -- the commit that
    // added the Steam launch contract -- but the launcher was never given the matching line, so
    // every launch through the app failed while the PowerShell launchers, which do not use the
    // host at all, kept working. Passing it here is what makes launch.bat work again.
    '--steam-path',
    readSteamInstallPath(),
    '--cmd-line',
    commandLineOverride || buildCommandLine(current),
  ];
  if (watchStartup) args.push('--watch-startup-ms', String(STARTUP_WATCH_MS));
  const env = environmentFor(current);
  for (const entry of extraEnv) {
    const separator = entry.indexOf('=');
    if (separator > 0) {
      env[entry.slice(0, separator)] = entry.slice(separator + 1);
    }
  }

  const result = await runLaunchHost({
    host, args, cwd: path.dirname(cs2Exe), env,
    logPath: path.join(app.getPath('userData'), 'launch.log'),
    onStartupExit: reportStartupExit,
  });
  launchedCs2ProcessId = result.processId;
  // CS2 stops answering as "running" while it tears down, before the host's wait returns its exit
  // code. Holding the running state until then keeps READY from flashing ahead of the error.
  startupWatchPending = true;
  result.hostExited.then(() => { startupWatchPending = false; });
  return { ok: result.ok, output: result.output, processId: result.processId };
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 600,
    height: 300,
    minWidth: 560,
    minHeight: 280,
    frame: false,
    resizable: true,
    backgroundColor: '#0c0e12',
    title: 'SOURCE:MVM',
    // Unpackaged only. A packaged build takes its taskbar/Alt-Tab icon from the .ico
    // electron-builder compiles into the exe, and build-resources is not inside the asar --
    // pointing at it there just logs "Failed to load image from path".
    ...(app.isPackaged ? {} : { icon: path.resolve(__dirname, '..', 'build-resources', 'icon.png') }),
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      preload: path.join(__dirname, 'preload.cjs'),
    },
  });

  const rendererUrl = process.env.SMVM_RENDERER_URL;
  if (rendererUrl) {
    mainWindow.loadURL(rendererUrl);
  } else {
    mainWindow.loadFile(path.join(__dirname, '..', 'renderer-dist', 'index.html'));
  }
  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

function registerIpc() {
  ipcMain.on('window:minimize', () => mainWindow?.minimize());
  ipcMain.on('window:close', () => mainWindow?.close());
  ipcMain.on('window:advanced', (_event, open) => setDrawerOpen(Boolean(open)));
  ipcMain.handle('paths:resolved', (_event, candidate) => resolvedPaths(candidate));
  ipcMain.handle('paths:sizes', (_event, targets) => pathSizes(targets));
  ipcMain.handle('path:reveal', (_event, target) => revealPath(target));
  // Fixed URL: the renderer never supplies one.
  ipcMain.handle('link:hctracker', () => shell.openExternal('https://www.hctracker.com/'));
  ipcMain.handle('settings:get', () => settings);
  ipcMain.handle('settings:save', (_event, next) => {
    const wasAutomatic = Boolean(settings && settings.autoUpdate);
    const saved = saveSettings(next);
    // Turning automatic updates on runs the usual startup check right away.
    if (saved.autoUpdate && !wasAutomatic) void updateService.check();
    return saved;
  });
  ipcMain.handle('cs2:detect', () => detectCs2Exe());
  ipcMain.handle('cs2:running', () => startupWatchPending || isProcessRunning(launchedCs2ProcessId));
  ipcMain.handle('cs2:workshop-tools', (_event, cs2Exe) => (
    workshopToolsStatus(cs2Exe || (settings && settings.cs2Exe) || '')
  ));
  ipcMain.handle('cs2:install-workshop-tools', async () => {
    // Hand off to Steam rather than trying to install anything ourselves.
    await shell.openExternal(`steam://install/${WORKSHOP_TOOLS_APP_ID}`);
    return { ok: true, appId: WORKSHOP_TOOLS_APP_ID };
  });
  ipcMain.handle('cs2:validate', (_event, candidate) => {
    if (!candidate || path.basename(candidate).toLowerCase() !== 'cs2.exe') return false;
    try {
      return fs.statSync(candidate).isFile();
    } catch {
      return false;
    }
  });
  ipcMain.handle('intro:video-url', () => introVideoUrl());
  ipcMain.handle('file:choose', async (_event, kind = 'game') => {
    const result = await dialog.showOpenDialog({
      properties: ['openFile'],
      filters: kind === 'hook'
        ? [{ name: 'SOURCE:MVM hook', extensions: ['dll'] }]
        : [{ name: 'Counter-Strike 2', extensions: ['exe'] }],
    });
    return result.canceled ? '' : result.filePaths[0];
  });
  ipcMain.handle('folder:choose', async () => {
    const result = await dialog.showOpenDialog({
      properties: ['openDirectory', 'createDirectory'],
    });
    return result.canceled ? '' : result.filePaths[0];
  });
  ipcMain.handle('dlss:status', () => dlssTool.status(settings && settings.cs2Exe));
  // The GPU and the runtime it gets (RTX 50 -> NVIDIA original, RTX 20/30/40 -> compatible).
  ipcMain.handle('dlss:detect', () => dlssTool.detectVariant());
  ipcMain.handle('dlss:choose-payload', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose the "dlss 5" folder (streamline, addons and the runtime folders)',
      properties: ['openDirectory'],
    });
    return result.canceled ? '' : result.filePaths[0];
  });
  ipcMain.handle('dlss:choose-reshade', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose ReShade64.dll or the ReShade setup with add-on support',
      properties: ['openFile'],
      filters: [{ name: 'ReShade', extensions: ['dll', 'exe'] }],
    });
    return result.canceled ? '' : result.filePaths[0];
  });
  ipcMain.handle('dlss:install', async (_event, options) => {
    if (isProcessRunning(launchedCs2ProcessId)) throw new Error('Close CS2 before installing DLSS 5.');
    return dlssTool.install({
      cs2Exe: settings.cs2Exe,
      payloadDir: options && options.payloadDir,
      reshadePath: options && options.reshadePath,
      // Empty = read it off the GPU (dlssTool.detectVariant).
      variant: (options && options.variant) || '',
      onProgress: (message) => mainWindow?.webContents.send('dlss-status', { message }),
    }).catch((error) => ({ error: error.message, code: error.code || '' }));
  });
  ipcMain.handle('dlss:uninstall', () => {
    if (isProcessRunning(launchedCs2ProcessId)) throw new Error('Close CS2 before uninstalling DLSS 5.');
    dlssTool.uninstall();
    settings = saveSettings({ ...settings, dlss5Enabled: false });
    return dlssTool.status(settings.cs2Exe);
  });
  ipcMain.handle('dlss:clean-steam', () => {
    const result = dlssTool.cleanSteam(settings.cs2Exe);
    return { ...result, status: dlssTool.status(settings.cs2Exe) };
  });
  ipcMain.handle('cs2:launch', async (_event, next) => {
    updateService.assertCanLaunch();
    launchInProgress = true;
    try {
      settings = saveSettings(next);
      return await launchWithSettings(settings, undefined, [], true);
    } finally { launchInProgress = false; }
  });
  ipcMain.handle('update:status', () => updateService.getStatus());
  // The UPDATES button: one check now, whatever the automatic-updates setting says.
  ipcMain.handle('update:check', () => updateService.check({ manual: true }));
  // The user said yes to a pending update: download it, install it and restart.
  ipcMain.handle('update:download', () => updateService.install());
  ipcMain.handle('update:install', () => updateService.install());

  // FFmpeg. Its own lifecycle: the render pipeline cannot encode without it, but it is a
  // third-party binary the user may already have, so it is detected before it is ever downloaded.
  ipcMain.handle('ffmpeg:detect', () => ffmpegTool.detect(settings?.ffmpegPath || ''));
  // Point the launcher at an ffmpeg.exe the user already has, wherever it is. Validated by
  // actually running it -- a picked file that does not execute is refused, not stored.
  ipcMain.handle('ffmpeg:choose', async () => {
    const result = await dialog.showOpenDialog({
      properties: ['openFile'],
      filters: [{ name: 'FFmpeg', extensions: ['exe'] }],
    });
    if (result.canceled || !result.filePaths[0]) return { ok: false, canceled: true };
    const chosen = result.filePaths[0];
    const version = await ffmpegTool.probeVersion(chosen);
    if (!version) {
      return { ok: false, message: `${path.basename(chosen)} did not run as FFmpeg. Pick ffmpeg.exe itself.` };
    }
    saveSettings({ ...settings, ffmpegPath: chosen });
    await linkFfmpegToRuntime();
    return { ok: true, path: chosen, version };
  });
  ipcMain.handle('ffmpeg:install', async (_event, options) => {
    try {
      const result = await ffmpegTool.install(sendFfmpegStatus);
      await linkFfmpegToRuntime();
      if (options && options.addToPath) {
        const added = await ffmpegTool.addToUserPath(path.dirname(result.path));
        return { ...result, pathAdded: added.ok, pathAlready: added.already };
      }
      return result;
    } catch (error) {
      sendFfmpegStatus({ state: 'error', message: error.message });
      return { ok: false, message: error.message };
    }
  });
  // Adopt an ffmpeg the user installed themselves: re-detect and point the hook at it.
  ipcMain.handle('ffmpeg:relink', async () => {
    const found = await ffmpegTool.detect(settings?.ffmpegPath || '');
    if (!found.found) return { ok: false, message: 'Still no FFmpeg found.' };
    await linkFfmpegToRuntime();
    return { ok: true, path: found.path, version: found.version, source: found.source };
  });

  // Runtime payload (hook DLL + injector + FX pack). Separate from the app updater above: the
  // app and the game-side runtime version independently and are downloaded separately.
  ipcMain.handle('payload:status', () => runtimePayload.localStatus());
  ipcMain.handle('payload:check', async () => {
    try {
      return await runtimePayload.checkForPayload();
    } catch (error) {
      return { state: 'error', message: error.message, local: runtimePayload.localStatus() };
    }
  });
  ipcMain.handle('payload:install', async (_event, only) => {
    try {
      const result = await runtimePayload.installPayload(sendPayloadStatus, only);
      // Installing the runtime replaces <runtime> wholesale, which deletes ffmpeg.ini along with
      // it. Without this the hook silently loses ffmpeg after every hook update.
      await linkFfmpegToRuntime();
      return result;
    } catch (error) {
      sendPayloadStatus({ state: 'error', message: error.message });
      return { state: 'error', message: error.message, local: runtimePayload.localStatus() };
    }
  });
}

async function runHeadless(argv) {
  const { current, commandLine, envValues } = cliSettings(argv);
  try {
    // A check only reports a pending update; nobody is there to say yes, so automation launches
    // the installed app. The installer owns exit/restart. Do not exit this process before its
    // quit callback runs.
    const update = await updateService.check();
    if (update.state === 'installing') return;
    updateService.assertCanLaunch();
    await launchWithSettings(current, commandLine, envValues);
    process.exit(0);
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exit(1);
  }
}

app.whenReady().then(async () => {
  registerMediaProtocol();
  settings = loadSettings();
  dlssTool.init(app.getPath('userData'));
  registerIpc();
  configureUpdater();
  const headless = process.argv.includes('--headless')
    || process.argv.includes('-noGui')
    || process.argv.includes('-autoStart');
  if (headless) {
    await runHeadless(process.argv.slice(1));
    return;
  }
  createWindow();
  // Startup checks and installs are owned by the main process, including complete builds. With
  // automatic updates off (the default) this touches no network and only readies the runtime.
  void updateService.check();
  // The install/launch counter is gone; its state file is left over from older launchers.
  fs.rm(path.join(app.getPath('userData'), 'usage-ping.json'), { force: true }, () => {});
  // The runtime may have arrived without an install pass having run -- a pre-seeded bundle, or a
  // hand-copied payload -- in which case nothing ever wrote ffmpeg.ini and renders silently have
  // no encoder even though ffmpeg is on the machine. Linking is idempotent and cheap, so do it
  // every startup rather than only after installs. Fire-and-forget: a miss just means the
  // renderer's own detect flow prompts as before.
  linkFfmpegToRuntime().catch(() => {});
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
