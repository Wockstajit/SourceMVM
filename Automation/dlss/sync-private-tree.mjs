// Brings the launcher's private ReShade copy of CS2 (%APPDATA%\SourceMovieMaker\dlss5-cs2) up to date
// with Steam's CS2, exactly as the launcher does before every DLSS launch (dlss-tool.cjs
// prepareLaunch): changed game/bin files are copied from Steam (the ReShade and DLSS files are never
// touched), the SOURCE:MVM ReShade add-on is refreshed and ReShade.ini keeps its search paths.
//
// The automation launchers (launch-cs2-netcon.ps1, and the test and demo launchers through it) run
// this whenever -Cs2Dir is that copy. Before they did, a CS2 update left the copy on the old engine
// and game binaries while the shared game content moved on, so it could not be used for testing.
//
//   node Automation/dlss/sync-private-tree.mjs [<private root>]
//
// Exit code 0 when the copy matches Steam, 1 when it could not be synced (CS2 running from it, no
// marker naming the Steam install, DLSS 5 not installed there).
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';

const require = createRequire(import.meta.url);
// dlss-tool asks Electron whether it runs packaged. Outside the launcher it is not, and loading the
// electron package from plain node starts its binary download, so it is answered here instead.
const Module = require('node:module');
const load = Module._load;
Module._load = function (request, ...rest) { return request === 'electron' ? {} : load.call(this, request, ...rest); };
const dlss = require('../../SourceMovieMakerLauncher/electron/dlss-tool.cjs');

const privateRoot = path.resolve(process.argv[2] || path.join(process.env.APPDATA || '', 'SourceMovieMaker', 'dlss5-cs2'));
const fail = (message) => { console.error(`sync-private-tree: ${message}`); process.exit(1); };

let marker;
try { marker = JSON.parse(fs.readFileSync(path.join(privateRoot, 'source-mvm-isolation.json'), 'utf8')); } catch { marker = null; }
if (!marker || !marker.source) fail(`${privateRoot} is not the launcher's private copy (no source-mvm-isolation.json naming Steam's CS2).`);
const steamExe = path.join(marker.source, 'game', 'bin', 'win64', 'cs2.exe');
if (!fs.existsSync(steamExe)) fail(`Steam's CS2 is not where the copy says it came from: ${steamExe}`);

const binaries = ['engine2.dll', 'client.dll', 'rendersystemdx11.dll', 'cs2.exe'];
const stamp = (root) => binaries.map((name) => {
  try { const s = fs.statSync(path.join(root, 'game', 'bin', 'win64', name)); return `${s.size}:${Math.round(s.mtimeMs / 2000)}`; }
  catch { return 'missing'; }
});
const before = stamp(privateRoot);

dlss.init(path.dirname(privateRoot));
try {
  dlss.prepareLaunch(steamExe);
} catch (error) {
  fail(`${error.message} (is CS2 running from ${privateRoot}?)`);
}

const steam = stamp(marker.source), after = stamp(privateRoot);
const stale = binaries.filter((_, i) => steam[i] !== after[i]);
if (stale.length) fail(`still differs from Steam after the sync: ${stale.join(', ')}`);
const updated = binaries.filter((_, i) => before[i] !== after[i]);
console.log(updated.length
  ? `sync-private-tree: updated from Steam (${updated.join(', ')} and any other changed binaries); ReShade files kept.`
  : 'sync-private-tree: already matches Steam.');
