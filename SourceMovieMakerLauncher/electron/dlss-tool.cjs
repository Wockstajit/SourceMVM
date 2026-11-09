// DLSS 5 (ReShade + RenoDX) for SOURCE:MVM sessions ONLY.
//
// ReShade loads as a proxy d3d11.dll from the folder of the cs2.exe that starts. Put it beside
// Steam's cs2.exe and it loads in EVERY CS2 session, online matches included -- which is how
// DLSS ended up "working outside the launcher", and what gets an account banned. So nothing here
// ever writes to Steam's CS2 folder. Instead:
//
//   * A PRIVATE copy of game/bin lives in the launcher's data folder (dlss5-cs2/game/bin). The
//     rest of game/ (csgo, core, ... the many-GB assets) are junctions back into Steam, so the
//     copy costs only the binaries. ReShade, the RenoDX add-on and the NVIDIA runtime go there.
//   * The launcher starts THAT cs2.exe when DLSS 5 is switched on. Steam, a desktop shortcut or
//     the launcher with DLSS off all start Steam's cs2.exe, which has no proxy beside it.
//   * Steam updates replace Steam's binaries, not ours, so every DLSS launch first re-syncs the
//     private binaries from Steam (changed files only; our own files are never overwritten).
//   * Leftover proxies from the old shared install (d3d11.dll, ReShade.ini, the add-on, ...) in
//     Steam's folder are reported, and moved -- never deleted -- to a recoverable backup.
//
// The payload is local only (no downloads): the user's "dlss 5" folder, checked against the
// SHA-256 pins in dlss/payload-versions.json, plus ReShade with add-on support (ReShade64.dll, or
// the official ReShade_Setup_*_Addon.exe, which is a zip archive the DLL is read out of).
//
// A self-contained build carries that whole folder, ReShade included, in resources/bundle/dlss5,
// and picks the runtime from the GPU (detectVariant): RTX 50-series gets NVIDIA's original signed
// runtime, RTX 20/30/40 the compatible one. Nothing has to be chosen by hand.

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');

const ASSET_DIR = path.join(__dirname, 'dlss');
const VERSIONS = JSON.parse(fs.readFileSync(path.join(ASSET_DIR, 'payload-versions.json'), 'utf8'));

// Names Windows will load from the exe folder in place of a system DLL. CS2 ships none of them
// beside cs2.exe, so any of these in Steam's folder is a graphics injector.
const PROXY_NAMES = ['d3d11.dll', 'dxgi.dll', 'd3d12.dll', 'version.dll', 'dinput8.dll', 'winmm.dll', 'opengl32.dll'];
// The rest of a ReShade / DLSS install that has no business beside Steam's cs2.exe either.
const LEFTOVER_PATTERNS = [/^reshade.*\.(ini|log)$/i, /\.addon64$/i, /^nvngx_dlss/i, /^sl\.[a-z_]+\.dll$/i];

const STREAMLINE_FILES = [
  'nvngx_dlss.dll', 'nvngx_dlssg.dll',
  'sl.common.dll', 'sl.deepdvc.dll', 'sl.dlss_d.dll', 'sl.dlss_g.dll',
  'sl.dlss_nr.dll', 'sl.dlss.dll', 'sl.interposer.dll', 'sl.nis.dll',
  'sl.pcl.dll', 'sl.reflex.dll',
  'nvngx_dlss.license.txt', 'nis.license.txt', 'reflex.license.txt',
];
const VARIANTS = ['NVIDIA original', 'RTX 40, 30, 20 compatible'];
const SKIP_DIRS = /^(mvm_.*|automation)$/i;
const SKIP_FILES = /^mvm_.*\.(bin|log|json|png|txt)$/i;

// Everything WE put in the private bin. The Steam re-sync must never overwrite or delete these.
const OWNED_FILES = new Set([
  ...STREAMLINE_FILES, 'nvngx_dlssnr.dll', 'renodx-dlss.addon64', 'mvm_reshade_state.addon64',
  'd3d11.dll', 'reshade.ini', 'reshade2.ini', 'reshade.log', 'reshade2.log',
].map((n) => n.toLowerCase()));

// ---- bundled payload and GPU --------------------------------------------------------------------
function bundledPayloadDir() {
  try {
    const { app } = require('electron');
    if (!app || !app.isPackaged) return '';
  } catch { return ''; }
  const dir = path.join(process.resourcesPath, 'bundle', 'dlss5');
  return fs.existsSync(path.join(dir, 'streamline')) ? dir : '';
}

// Display adapter names. PowerShell's CIM query fails on some machines (blocked, slow WMI, a
// broken repository), which reported a present RTX 5070 Ti as "No NVIDIA GPU was found", so the
// display-class registry keys and nvidia-smi are asked too.
function adapterNames() {
  const run = (file, args) => {
    try { return execFileSync(file, args, { encoding: 'utf8', windowsHide: true, timeout: 15000 }); } catch { return ''; }
  };
  const names = [];
  run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
    '(Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name) -join "|"'])
    .split('|').forEach((n) => names.push(n.trim()));
  if (!names.some((n) => /nvidia/i.test(n))) {
    run('reg.exe', ['query', 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}',
      '/s', '/v', 'DriverDesc']).split(/\r?\n/).forEach((line) => {
      const m = /DriverDesc\s+REG_SZ\s+(.+)$/i.exec(line.trim());
      if (m) names.push(m[1].trim());
    });
  }
  if (!names.some((n) => /nvidia/i.test(n))) {
    run('nvidia-smi.exe', ['-L']).split(/\r?\n/).forEach((line) => {
      const m = /^GPU \d+:\s*(.+?)\s*\(UUID/i.exec(line.trim());
      if (m) names.push(/nvidia/i.test(m[1]) ? m[1] : `NVIDIA ${m[1]}`);
    });
  }
  return [...new Set(names.filter(Boolean))];
}

// The NVIDIA GPU, from Windows' own adapter list. RTX 50-series (Blackwell) runs NVIDIA's signed
// runtime; RTX 20/30/40 need the compatible build. Anything else cannot run DLSS 5.
function detectVariant() {
  const names = adapterNames();
  const nvidia = names.filter((n) => /nvidia/i.test(n));
  const rtx = (n) => { const m = /RTX\s*(?:PRO\s*)?(\d{2})\d{2}/i.exec(n); return m ? Number(m[1]) : 0; };
  const series = nvidia.map(rtx).filter(Boolean).sort((a, b) => b - a);
  const gpu = nvidia[0] || names[0] || '';
  if (series[0] >= 50) return { gpu, variant: 'NVIDIA original', supported: true };
  if (series[0] >= 20) return { gpu, variant: 'RTX 40, 30, 20 compatible', supported: true };
  return { gpu, variant: '', supported: false,
    reason: gpu ? `${gpu} cannot run DLSS 5: it needs an NVIDIA RTX 20-series or newer.` : 'No NVIDIA GPU was found.' };
}

let dataRoot = '';
function init(userDataDir) { dataRoot = userDataDir; }
function privateRoot() { return path.join(dataRoot, 'dlss5-cs2'); }
function privateBin() { return path.join(privateRoot(), 'game', 'bin', 'win64'); }
function privateExe() { return path.join(privateBin(), 'cs2.exe'); }
function manifestPath() { return path.join(privateRoot(), 'dlss5-files.json'); }
function markerPath() { return path.join(privateRoot(), 'source-mvm-isolation.json'); }

function installRootOf(cs2Exe) { return path.resolve(path.dirname(cs2Exe), '..', '..', '..'); }
function sha256(file) { return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex').toUpperCase(); }
function readJson(file) { try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; } }
function isReparse(p) { try { return fs.lstatSync(p).isSymbolicLink(); } catch { return false; } }
function sameFile(a, b) {
  try {
    const sa = fs.statSync(a); const sb = fs.statSync(b);
    return sa.size === sb.size && Math.abs(sa.mtimeMs - sb.mtimeMs) < 2000;
  } catch { return false; }
}

// ---- Steam's folder: what must not be there ---------------------------------------------------
function steamLeftovers(cs2Exe) {
  if (!cs2Exe || !fs.existsSync(cs2Exe)) return [];
  const bin = path.dirname(cs2Exe);
  if (path.resolve(bin).toLowerCase() === path.resolve(privateBin()).toLowerCase()) return [];
  let names = [];
  try { names = fs.readdirSync(bin); } catch { return []; }
  return names.filter((name) => PROXY_NAMES.includes(name.toLowerCase())
    || LEFTOVER_PATTERNS.some((re) => re.test(name)));
}

function cleanSteam(cs2Exe) {
  const found = steamLeftovers(cs2Exe);
  if (!found.length) return { ok: true, moved: [], backup: '' };
  const backup = path.join(dataRoot, 'dlss5-steam-backups', new Date().toISOString().replace(/[:.]/g, '-'));
  fs.mkdirSync(backup, { recursive: true });
  const bin = path.dirname(cs2Exe);
  for (const name of found) {
    const from = path.join(bin, name);
    fs.copyFileSync(from, path.join(backup, name));
    fs.rmSync(from, { force: true });
  }
  return { ok: true, moved: found, backup };
}

// ---- The private binary tree --------------------------------------------------------------------
// Copies game/bin from Steam where size or timestamp differ, skipping the files we own. Called on
// install and before every DLSS launch, so a Steam update never leaves the private exe one build
// behind the shared assets.
function syncBinaries(cs2Exe) {
  const source = installRootOf(cs2Exe);
  const target = privateRoot();
  for (const dir of [target, path.join(target, 'game'), path.join(target, 'game', 'bin'), privateBin()]) {
    if (isReparse(dir)) throw new Error(`Refusing a redirected private folder: ${dir}`);
  }
  const marker = readJson(markerPath());
  if (fs.existsSync(target) && marker && marker.source && path.resolve(marker.source).toLowerCase() !== source.toLowerCase()) {
    throw new Error('The private DLSS copy belongs to a different CS2 install. Uninstall DLSS 5 and install it again.');
  }
  fs.mkdirSync(path.join(target, 'game'), { recursive: true });
  fs.writeFileSync(markerPath(), JSON.stringify({ source, syncedAt: new Date().toISOString(), sharedAssets: true }, null, 2));

  let copied = 0;
  const walk = (from, to) => {
    fs.mkdirSync(to, { recursive: true });
    for (const entry of fs.readdirSync(from, { withFileTypes: true })) {
      const src = path.join(from, entry.name);
      const dst = path.join(to, entry.name);
      if (entry.isSymbolicLink()) continue;
      // Our own debug captures and automation output that ended up in Steam's folder are not part
      // of CS2 (one machine had 133 GB of them); never drag them into the copy.
      if (entry.isDirectory() && SKIP_DIRS.test(entry.name)) continue;
      if (entry.isFile() && SKIP_FILES.test(entry.name)) continue;
      if (entry.isDirectory()) { walk(src, dst); continue; }
      if (to === privateBin() && OWNED_FILES.has(entry.name.toLowerCase())) continue;
      if (to === privateBin() && PROXY_NAMES.includes(entry.name.toLowerCase())) continue;
      if (sameFile(src, dst)) continue;
      fs.copyFileSync(src, dst);
      const st = fs.statSync(src);
      fs.utimesSync(dst, st.atime, st.mtime);
      copied += 1;
    }
  };
  walk(path.join(source, 'game', 'bin'), path.join(target, 'game', 'bin'));

  // Everything else under game/ is shared through a junction: no copy, no drift.
  for (const entry of fs.readdirSync(path.join(source, 'game'), { withFileTypes: true })) {
    if (entry.name.toLowerCase() === 'bin') continue;
    const src = path.join(source, 'game', entry.name);
    const dst = path.join(target, 'game', entry.name);
    if (entry.isDirectory()) {
      if (!fs.existsSync(dst)) fs.symlinkSync(src, dst, 'junction');
    } else if (!sameFile(src, dst)) {
      fs.copyFileSync(src, dst);
    }
  }
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    if (entry.isFile()) {
      const dst = path.join(target, entry.name);
      if (!sameFile(path.join(source, entry.name), dst)) fs.copyFileSync(path.join(source, entry.name), dst);
    }
  }
  return copied;
}

// ---- ReShade ------------------------------------------------------------------------------------
function looksLikeReShade(buffer) {
  return buffer.length > 1024 && buffer[0] === 0x4d && buffer[1] === 0x5a
    && (buffer.includes(Buffer.from('ReShade', 'utf16le')) || buffer.includes(Buffer.from('ReShade')));
}

// The official ReShade setup is a PE stub with a zip archive appended: find the archive's central
// directory from the end and inflate ReShade64.dll out of it.
function extractReShadeFromSetup(setupPath) {
  const data = fs.readFileSync(setupPath);
  const eocd = data.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  if (eocd < 0) throw new Error('That file is not a ReShade setup archive.');
  const entries = data.readUInt16LE(eocd + 10);
  let cd = data.readUInt32LE(eocd + 16);
  // An appended archive's offsets are relative to where the archive starts, not the file.
  const cdSize = data.readUInt32LE(eocd + 12);
  const shift = eocd - cdSize - cd;
  cd += shift;
  for (let i = 0; i < entries; i += 1) {
    if (data.readUInt32LE(cd) !== 0x02014b50) break;
    const method = data.readUInt16LE(cd + 10);
    const compressed = data.readUInt32LE(cd + 20);
    const nameLen = data.readUInt16LE(cd + 28);
    const extraLen = data.readUInt16LE(cd + 30);
    const commentLen = data.readUInt16LE(cd + 32);
    const local = data.readUInt32LE(cd + 42) + shift;
    const name = data.toString('utf8', cd + 46, cd + 46 + nameLen);
    if (/(^|\/)ReShade64\.dll$/i.test(name)) {
      const lNameLen = data.readUInt16LE(local + 26);
      const lExtraLen = data.readUInt16LE(local + 28);
      const start = local + 30 + lNameLen + lExtraLen;
      const raw = data.subarray(start, start + compressed);
      return method === 0 ? Buffer.from(raw) : zlib.inflateRawSync(raw);
    }
    cd += 46 + nameLen + extraLen + commentLen;
  }
  throw new Error('ReShade64.dll was not found inside that setup file. Use the "with full add-on support" setup.');
}

function loadReShade(payloadDir, reshadePath) {
  const candidates = [reshadePath, path.join(payloadDir, 'ReShade64.dll'), path.join(payloadDir, 'd3d11.dll')].filter(Boolean);
  for (const candidate of candidates) {
    if (!fs.existsSync(candidate)) continue;
    const buffer = /\.exe$/i.test(candidate) ? extractReShadeFromSetup(candidate) : fs.readFileSync(candidate);
    if (looksLikeReShade(buffer)) return buffer;
    throw new Error(`${path.basename(candidate)} is not ReShade.`);
  }
  return null;
}

// ---- ReShade.ini / ReShade2.ini -----------------------------------------------------------------
// CS2 makes two swapchains, so ReShade attaches two runtimes and each reads its own ini: both need
// the add-on load line and start with neural rendering off. Existing inis are repaired in place,
// so overlay tuning (intensity, model, keys) survives: the load line and the hook
// are forced, keys from older add-on builds are dropped, and missing template keys are added.
const FORCED_KEYS = { ADDON: ['LoadFromDllMain'], 'RENODX-DLSS': ['DirectNeuralRenderingHookStage'] };
// Settings the 2026-10-04 add-on no longer reads (its hook is HookStage, Require DLSS is gone).
const OBSOLETE_KEYS = { 'RENODX-DLSS': ['DirectNeuralRenderingHookPoint', 'DirectNeuralRenderingRequireDlss', 'DirectNeuralRenderingForceNgxCore'] };
// Search paths the template adds to whatever the user set. ReShade itself seeds `.\` when it writes
// its ini, which does not reach the reshade-shaders folder its own installer lays out, so an install
// copied into the private folder compiled nothing until the user found the setting.
const LIST_KEYS = { GENERAL: ['EffectSearchPaths', 'TextureSearchPaths'] };

function parseIni(text) {
  const sections = []; // [{ name, lines: [raw] }], name '' = before the first header
  let current = { name: '', lines: [] };
  sections.push(current);
  for (const line of text.split(/\r?\n/)) {
    const header = /^\s*\[([^\]]+)\]\s*$/.exec(line);
    if (header) { current = { name: header[1], lines: [] }; sections.push(current); continue; }
    current.lines.push(line);
  }
  return sections;
}
function keyOf(line) { const m = /^\s*([^;=\s][^=]*?)\s*=/.exec(line); return m ? m[1] : null; }

function listOf(line) {
  return line.slice(line.indexOf('=') + 1).split(',').map((v) => v.trim()).filter(Boolean);
}
function mergeList(have, add) {
  const out = [...have];
  for (const value of add) if (!out.some((v) => v.toLowerCase() === value.toLowerCase())) out.push(value);
  return out;
}

function mergeIni(existing, template) {
  const out = parseIni(existing);
  for (const tSection of parseIni(template)) {
    if (!tSection.name) continue;
    let section = out.find((s) => s.name.toLowerCase() === tSection.name.toLowerCase());
    if (!section) {
      while (out.length && out[out.length - 1].lines.length && !out[out.length - 1].lines[out[out.length - 1].lines.length - 1].trim()) out[out.length - 1].lines.pop();
      if (out[out.length - 1].lines.length || out.length > 1) out[out.length - 1].lines.push('');
      section = { name: tSection.name, lines: [] };
      out.push(section);
    }
    const obsolete = (OBSOLETE_KEYS[tSection.name] || []).map((k) => k.toLowerCase());
    section.lines = section.lines.filter((line) => !obsolete.includes((keyOf(line) || '').toLowerCase()));
    const forced = (FORCED_KEYS[tSection.name] || []).map((k) => k.toLowerCase());
    for (const tLine of tSection.lines) {
      const key = keyOf(tLine);
      if (!key) continue;
      const at = section.lines.findIndex((line) => (keyOf(line) || '').toLowerCase() === key.toLowerCase());
      if (at < 0) {
        let end = section.lines.length;
        while (end > 0 && !section.lines[end - 1].trim()) end -= 1;
        section.lines.splice(end, 0, tLine);
      } else if (forced.includes(key.toLowerCase())) {
        section.lines[at] = tLine;
      } else if ((LIST_KEYS[tSection.name] || []).map((k) => k.toLowerCase()).includes(key.toLowerCase())) {
        section.lines[at] = `${keyOf(section.lines[at])}=${mergeList(listOf(section.lines[at]), listOf(tLine)).join(',')}`;
      }
    }
  }
  return out.map((s) => (s.name ? [`[${s.name}]`, ...s.lines] : s.lines).join('\r\n'))
    .filter((block, i) => i > 0 || block.length).join('\r\n');
}

function ensureReShadeConfig() {
  const bin = privateBin();
  const template = fs.readFileSync(path.join(ASSET_DIR, 'ReShade.isolated.ini'), 'utf8').split('{{BINDIR}}').join(bin);
  for (const ini of ['ReShade.ini', 'ReShade2.ini']) {
    const dst = path.join(bin, ini);
    const before = fs.existsSync(dst) ? fs.readFileSync(dst, 'utf8') : '';
    const after = before ? mergeIni(before, template) : template;
    if (after !== before) fs.writeFileSync(dst, after);
  }
}

// ---- The user's own ReShade setup ---------------------------------------------------------------
// Presets and shaders have to sit beside the private cs2.exe, not Steam's. A setup that already lives
// in another game's folder or in Steam's CS2 folder comes across through importReShade.
const SHADER_DIRS = ['reshade-shaders', 'Shaders', 'Textures'];
const PRESET_MARKER = /^\s*(Techniques|TechniqueSorting)\s*=/m;

function iniValue(file, section, key) {
  try {
    const found = parseIni(fs.readFileSync(file, 'utf8')).find((s) => s.name.toLowerCase() === section.toLowerCase());
    const line = found && found.lines.find((l) => (keyOf(l) || '').toLowerCase() === key.toLowerCase());
    return line ? line.slice(line.indexOf('=') + 1).trim() : '';
  } catch { return ''; }
}

function setIniValue(file, section, key, value) {
  if (!fs.existsSync(file)) return;
  const sections = parseIni(fs.readFileSync(file, 'utf8'));
  let found = sections.find((s) => s.name.toLowerCase() === section.toLowerCase());
  if (!found) { found = { name: section, lines: [] }; sections.push(found); }
  const at = found.lines.findIndex((l) => (keyOf(l) || '').toLowerCase() === key.toLowerCase());
  if (at >= 0) found.lines[at] = `${key}=${value}`; else found.lines.unshift(`${key}=${value}`);
  fs.writeFileSync(file, sections.map((s) => (s.name ? [`[${s.name}]`, ...s.lines] : s.lines).join('\r\n'))
    .filter((block, i) => i > 0 || block.length).join('\r\n'));
}

function isPreset(file) {
  try { return fs.statSync(file).size < 4 * 1024 * 1024 && PRESET_MARKER.test(fs.readFileSync(file, 'utf8')); } catch { return false; }
}

function reshadeInfo() {
  const bin = privateBin();
  let effects = 0;
  const walk = (dir) => {
    let entries = [];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      if (entry.isDirectory()) walk(path.join(dir, entry.name));
      else if (/\.fx$/i.test(entry.name)) effects += 1;
    }
  };
  const dirs = new Set(SHADER_DIRS.map((name) => path.join(bin, name)));
  for (const value of iniValue(path.join(bin, 'ReShade.ini'), 'GENERAL', 'EffectSearchPaths').split(',')) {
    const base = value.trim().replace(/[\\/]\*\*$/, '');
    if (base && !path.isAbsolute(base) && path.resolve(bin, base) !== path.resolve(bin)) dirs.add(path.resolve(bin, base));
  }
  // A nested folder is listed both on its own and under its parent: count each file once.
  const roots = [...dirs].sort((a, b) => a.length - b.length)
    .filter((dir, i, all) => !all.slice(0, i).some((p) => dir.toLowerCase().startsWith(`${p.toLowerCase()}${path.sep}`)));
  roots.forEach(walk);
  try { effects += fs.readdirSync(bin).filter((n) => /\.fx$/i.test(n)).length; } catch { /* not installed */ }
  let presets = [];
  try { presets = fs.readdirSync(bin).filter((n) => /\.ini$/i.test(n) && !/^reshade\d*\.ini$/i.test(n) && isPreset(path.join(bin, n))); } catch { /* not installed */ }
  const active = path.basename(iniValue(path.join(bin, 'ReShade.ini'), 'GENERAL', 'PresetPath') || '');
  return { folder: bin, effects, presets, activePreset: presets.includes(active) ? active : '' };
}

// The search paths a source's ini lists, as they should read in the private folder. A folder inside
// the source is copied across at the same relative path. One outside it stays where it is and is
// searched by its full path, so a shared shader library stays one library.
function importSearchPaths(sourceDir, values) {
  const bin = privateBin();
  const out = [];
  for (const value of values.split(',').map((v) => v.trim()).filter(Boolean)) {
    const recursive = /[\\/]\*\*$/.test(value);
    const base = value.replace(/[\\/]\*\*$/, '');
    const suffix = recursive ? '\\**' : '';
    if (path.isAbsolute(base)) { out.push(value); continue; }
    const from = path.resolve(sourceDir, base);
    const relative = path.relative(sourceDir, from);
    if (!relative) continue; // the source's own root: the template already searches ours
    if (relative.startsWith('..') || path.isAbsolute(relative)) { out.push(`${from}${suffix}`); continue; }
    if (!fs.existsSync(from) || !fs.statSync(from).isDirectory()) continue;
    fs.cpSync(from, path.join(bin, relative), { recursive: true, force: true, dereference: true });
    out.push(`.\\${relative}${suffix}`);
  }
  return out;
}

// Copies a ReShade setup (a game folder, or a folder of presets and shaders) into the private folder.
// Nothing at the source is changed.
function importReShade(sourceDir) {
  const bin = privateBin();
  if (!fs.existsSync(bin)) throw new Error('Install DLSS 5 first: ReShade lives in its private folder.');
  if (!sourceDir || !fs.existsSync(sourceDir)) throw new Error('That folder does not exist.');
  if (path.resolve(sourceDir).toLowerCase() === path.resolve(bin).toLowerCase()) {
    throw new Error('That is already the folder ReShade reads.');
  }
  let shaderFolders = 0;
  for (const name of SHADER_DIRS) {
    const from = path.join(sourceDir, name);
    if (fs.existsSync(from) && fs.statSync(from).isDirectory()) {
      fs.cpSync(from, path.join(bin, name), { recursive: true, force: true, dereference: true });
      shaderFolders += 1;
    }
  }
  const presets = [];
  let rootEffects = 0;
  for (const entry of fs.readdirSync(sourceDir, { withFileTypes: true })) {
    if (!entry.isFile()) continue;
    if (/\.fxh?$/i.test(entry.name)) {
      fs.copyFileSync(path.join(sourceDir, entry.name), path.join(bin, entry.name));
      rootEffects += 1;
      continue;
    }
    if (!/\.ini$/i.test(entry.name) || /^reshade\d*\.ini$/i.test(entry.name)) continue;
    const from = path.join(sourceDir, entry.name);
    if (!isPreset(from)) continue;
    fs.copyFileSync(from, path.join(bin, entry.name));
    presets.push(entry.name);
  }
  // The preset the source was using can sit in a subfolder or anywhere else on disk.
  const sourceIni = path.join(sourceDir, 'ReShade.ini');
  const configured = iniValue(sourceIni, 'GENERAL', 'PresetPath');
  const configuredFile = configured ? path.resolve(sourceDir, configured) : '';
  const configuredName = configured ? path.basename(configured) : '';
  if (configuredFile && isPreset(configuredFile)) {
    const known = presets.some((n) => n.toLowerCase() === configuredName.toLowerCase());
    // A top-level preset of the same name is not this one unless it is the same file.
    if (!known || path.resolve(sourceDir, configuredName).toLowerCase() !== configuredFile.toLowerCase()) {
      fs.copyFileSync(configuredFile, path.join(bin, configuredName));
    }
    if (!known) presets.push(configuredName);
  }
  if (!shaderFolders && !rootEffects && !presets.length) {
    throw new Error('No ReShade presets or shaders in that folder. Choose the folder that holds your preset .ini and reshade-shaders.');
  }
  const preset = presets.find((n) => n.toLowerCase() === configuredName.toLowerCase()) || presets[0] || '';
  // Root-level Shaders and Textures folders are copied above, and nothing else searches them.
  const found = {
    EffectSearchPaths: fs.existsSync(path.join(sourceDir, 'Shaders')) ? ['.\\Shaders\\**'] : [],
    TextureSearchPaths: fs.existsSync(path.join(sourceDir, 'Textures')) ? ['.\\Textures\\**'] : [],
  };
  for (const key of LIST_KEYS.GENERAL) found[key].push(...importSearchPaths(sourceDir, iniValue(sourceIni, 'GENERAL', key)));
  ensureReShadeConfig();
  writeReShadeSettings(preset ? `.\\${preset}` : '', found);
  return { presets, preset, shaderFolders, info: reshadeInfo() };
}

function writeReShadeSettings(presetPath, searchPaths) {
  const bin = privateBin();
  for (const ini of ['ReShade.ini', 'ReShade2.ini']) {
    const dst = path.join(bin, ini);
    if (presetPath) setIniValue(dst, 'GENERAL', 'PresetPath', presetPath);
    for (const key of LIST_KEYS.GENERAL) {
      if (searchPaths[key].length) setIniValue(dst, 'GENERAL', key, mergeList(iniValue(dst, 'GENERAL', key).split(',').filter(Boolean), searchPaths[key]).join(','));
    }
  }
}

// A ReShade the user had beside Steam's cs2.exe: install moved its ini to a backup and syncBinaries
// brought its files across. This keeps what that ini pointed at, the preset and the shader folders.
// Returns the preset's path as written in the ini when it was kept.
function adoptMovedReShade(movedIniFile, steamBin) {
  const bin = privateBin();
  const searchPaths = {};
  for (const key of LIST_KEYS.GENERAL) searchPaths[key] = importSearchPaths(steamBin, iniValue(movedIniFile, 'GENERAL', key));
  const configured = iniValue(movedIniFile, 'GENERAL', 'PresetPath');
  // Kept as written: the sync copies a subfolder at the same relative path.
  const preset = configured && fs.existsSync(path.resolve(bin, configured)) ? configured : '';
  ensureReShadeConfig();
  writeReShadeSettings(preset, searchPaths);
  return preset;
}

// Keeps the add-on current on an existing install: it is what puts ReShade's effects into a render,
// and it has no other way to reach a machine that installed DLSS 5 before a launcher update.
function refreshStateAddon() {
  const from = path.join(ASSET_DIR, 'mvm_reshade_state.addon64');
  const to = path.join(privateBin(), 'mvm_reshade_state.addon64');
  try {
    if (!fs.existsSync(to) || sha256(from) !== sha256(to)) fs.copyFileSync(from, to);
  } catch (_) { /* in use or unreadable: the old add-on still loads, and the in-game note says so */ }
}

// ---- Status / install / uninstall ---------------------------------------------------------------
function status(cs2Exe) {
  const manifest = readJson(manifestPath());
  const installed = Boolean(manifest) && fs.existsSync(privateExe()) && fs.existsSync(path.join(privateBin(), 'd3d11.dll'));
  let outOfDate = false;
  if (installed && cs2Exe && fs.existsSync(cs2Exe)) {
    const bin = path.dirname(cs2Exe);
    outOfDate = ['cs2.exe', 'engine2.dll', path.join('..', '..', 'csgo', 'bin', 'win64', 'client.dll')]
      .some((name) => fs.existsSync(path.join(bin, name)) && !sameFile(path.join(bin, name), path.join(privateBin(), name)));
  }
  return {
    installed,
    outOfDate,
    variant: manifest ? manifest.runtimeVariant : '',
    installedAt: manifest ? manifest.installedAt : '',
    privateRoot: privateRoot(),
    privateExe: privateExe(),
    steamLeftovers: steamLeftovers(cs2Exe),
    variants: VARIANTS,
    bundled: Boolean(bundledPayloadDir()),
    reshade: installed ? reshadeInfo() : null,
  };
}

function authenticodeValid(file) {
  try {
    const out = execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
      `$s = Get-AuthenticodeSignature -LiteralPath '${file.replace(/'/g, "''")}'; "$($s.Status)|$($s.SignerCertificate.Subject)"`],
    { encoding: 'utf8', windowsHide: true });
    const [state, subject] = out.trim().split('|');
    return state === 'Valid' && /NVIDIA Corporation/i.test(subject || '');
  } catch { return false; }
}

async function install({ cs2Exe, payloadDir, reshadePath, variant, onProgress }) {
  const say = (message) => { if (onProgress) onProgress(message); };
  // A self-contained build supplies the folder and reads the runtime off the GPU.
  if (!payloadDir) payloadDir = bundledPayloadDir();
  if (!variant) {
    const detected = detectVariant();
    if (!detected.supported) throw new Error(detected.reason);
    variant = detected.variant;
    say(`${detected.gpu}: using the ${variant} runtime.`);
  }
  if (!cs2Exe || !fs.existsSync(cs2Exe)) throw new Error('Select a valid cs2.exe first.');
  if (!VARIANTS.includes(variant)) throw new Error(`Unknown DLSS runtime: ${variant}`);
  if (!payloadDir || !fs.existsSync(payloadDir)) throw new Error('Choose the "dlss 5" payload folder.');

  // 1. Validate everything before touching anything.
  const sources = {};
  for (const name of STREAMLINE_FILES) sources[name] = path.join(payloadDir, 'streamline', name);
  sources['nvngx_dlssnr.dll'] = path.join(payloadDir, variant, 'nvngx_dlssnr.dll');
  sources['renodx-dlss.addon64'] = path.join(payloadDir, 'addons', 'renodx-dlss.addon64');
  const missing = Object.values(sources).filter((f) => !fs.existsSync(f));
  if (missing.length) throw new Error(`The DLSS 5 folder is incomplete. Missing:\n${missing.join('\n')}`);
  say('Checking the payload...');
  if (sha256(sources['nvngx_dlssnr.dll']) !== VERSIONS.runtimes[variant].sha256) {
    throw new Error(`The ${variant} runtime failed its SHA-256 check. Nothing was installed.`);
  }
  if (sha256(sources['renodx-dlss.addon64']) !== VERSIONS.addon.sha256) {
    throw new Error('renodx-dlss.addon64 failed its SHA-256 check. Nothing was installed.');
  }
  if (variant === 'NVIDIA original' && !authenticodeValid(sources['nvngx_dlssnr.dll'])) {
    throw new Error('The NVIDIA original runtime is not validly signed by NVIDIA. Nothing was installed.');
  }
  const reshade = loadReShade(payloadDir, reshadePath);
  if (!reshade) {
    const err = new Error('ReShade with add-on support is needed. Choose ReShade64.dll or the official ReShade setup (.exe).');
    err.code = 'NEED_RESHADE';
    throw err;
  }

  // 2. Steam's folder first: a proxy left there is the ban risk this whole design exists to avoid.
  const cleaned = cleanSteam(cs2Exe);
  if (cleaned.moved.length) say(`Moved ${cleaned.moved.length} leftover file(s) out of Steam's CS2 folder.`);

  // 3. Private binaries, then our files.
  say('Copying CS2 binaries into the private DLSS folder (first time takes a minute)...');
  syncBinaries(cs2Exe);
  const bin = privateBin();
  say('Installing ReShade and DLSS 5...');
  fs.writeFileSync(path.join(bin, 'd3d11.dll'), reshade);
  for (const [name, from] of Object.entries(sources)) {
    if (!sameFile(from, path.join(bin, name))) fs.copyFileSync(from, path.join(bin, name));
  }
  fs.copyFileSync(path.join(ASSET_DIR, 'mvm_reshade_state.addon64'), path.join(bin, 'mvm_reshade_state.addon64'));
  fs.rmSync(path.join(bin, 'renodx-dlss5.addon64'), { force: true });
  ensureReShadeConfig();
  const movedIni = cleaned.moved.find((n) => /^reshade\.ini$/i.test(n));
  if (movedIni) {
    const movedPreset = adoptMovedReShade(path.join(cleaned.backup, movedIni), path.dirname(cs2Exe));
    if (movedPreset) say(`Kept your ReShade preset: ${path.basename(movedPreset)}.`);
  }
  fs.writeFileSync(manifestPath(), JSON.stringify({
    installedAt: new Date().toISOString(),
    sourceCs2: installRootOf(cs2Exe),
    runtimeVariant: variant,
    files: [...Object.keys(sources), 'd3d11.dll'].map((name) => ({ name, sha256: sha256(path.join(bin, name)) })),
  }, null, 2));
  return status(cs2Exe);
}

function uninstall() {
  const root = privateRoot();
  if (!fs.existsSync(root)) return { ok: true };
  if (!fs.existsSync(markerPath())) throw new Error(`Refusing to delete a folder the launcher did not create: ${root}`);
  // Junctions first, on their own: removing them never touches the Steam assets they point at.
  const game = path.join(root, 'game');
  if (fs.existsSync(game)) {
    for (const entry of fs.readdirSync(game)) {
      const p = path.join(game, entry);
      if (isReparse(p)) fs.unlinkSync(p);
    }
  }
  fs.rmSync(root, { recursive: true, force: true });
  return { ok: true };
}

// The exe a launch should start, refreshed from Steam first. Throws with a readable reason.
// ReShade rewrites ReShade.log every launch, and the RenoDX add-on logs on every DXGI factory the
// game creates -- several lines a frame, ~600 MB in a 45-minute session. NTFS compression on the
// file (kept when ReShade truncates it) stores that at about a third, and the log stays readable.
function compressReShadeLog() {
  const log = path.join(privateBin(), 'ReShade.log');
  try {
    if (!fs.existsSync(log)) fs.writeFileSync(log, '');
    execFileSync('compact.exe', ['/c', '/q', log], { windowsHide: true, stdio: 'ignore' });
  } catch (_) { /* best effort: an uncompressed log still works */ }
}

function prepareLaunch(cs2Exe) {
  const s = status(cs2Exe);
  if (!s.installed) throw new Error('DLSS 5 is switched on but not installed. Install it in ADVANCED > DLSS 5, or switch it off.');
  syncBinaries(cs2Exe);
  refreshStateAddon();
  ensureReShadeConfig();
  compressReShadeLog();
  return privateExe();
}

module.exports = {
  init, status, install, uninstall, cleanSteam, steamLeftovers, prepareLaunch, VARIANTS,
  detectVariant, bundledPayloadDir, extractReShadeFromSetup, mergeIni, importReShade, adoptMovedReShade, reshadeInfo,
};
