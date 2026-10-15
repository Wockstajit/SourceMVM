// One-time wipe of SOURCE:MVM's saved state, keyed by a "save epoch".
//
// Saved state could keep a demo unopenable: objects placed during a crash, or a lighting setup that
// froze the GPU, were restored on every load of that demo. This release starts everyone clean, and a
// later one can do it again by raising SAVE_EPOCH. Per the repo rule (replace, do not migrate), old
// saves are deleted, not converted.
//
// Everything lives under %APPDATA%\HLAE. Kept on purpose: bugreports\ and debuglogs\ (diagnostics,
// not settings) and osu\ (bundled game content).

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const { randomUUID } = require('node:crypto');

const SAVE_EPOCH = 1;
const EPOCH_FILE = 'source_mvm.save-epoch';
// Relative to %APPDATA%\HLAE.
const WIPED_DIRECTORIES = ['source_mvm'];
const WIPED_FILES = [
  'filmmaker_settings.json',
  'filmmaker_fx.json',
  'cosmetic_profiles.json',
  'easter_eggs.json',
  'filmmaker_demo_folders.txt',
  'filmmaker_demo_organizer.txt',
];

/** The epoch recorded on disk (0 when none). Pure parse, for tests. */
function parseEpoch(text) {
  const value = Number.parseInt(String(text || '').trim(), 10);
  return Number.isInteger(value) && value > 0 ? value : 0;
}

/** What a reset deletes, relative to the HLAE folder; empty when this machine is current. */
function planReset(recordedEpoch) {
  if (recordedEpoch >= SAVE_EPOCH) return [];
  return [...WIPED_DIRECTORIES, ...WIPED_FILES];
}

// These are Valve's original files, not resettable settings. Earlier hooks relocated them.
// Recover BEFORE any delete; a failure leaves all saves and the reset epoch untouched.
async function restoreMovedMatchInfo(hlaeRoot) {
  const demosRoot = path.join(hlaeRoot, 'source_mvm', 'demos');
  const cs2Root = path.join(demosRoot, 'cs2');
  let folders;
  try { folders = await fsp.readdir(cs2Root, { withFileTypes: true }); }
  catch (error) { if (error.code === 'ENOENT') return; throw error; }
  const archives = [];
  for (const folder of folders) {
    if (!folder.isDirectory()) continue;
    const archived = path.join(cs2Root, folder.name, 'demo.info');
    if (fs.existsSync(archived)) archives.push({ folder: folder.name, archived });
  }
  if (!archives.length) return;
  let index;
  try { index = JSON.parse(await fsp.readFile(path.join(demosRoot, 'index.json'), 'utf8')); }
  catch { throw new Error('Could not recover saved CS2 match metadata. Your saves have been kept.'); }
  for (const archive of archives) {
    const entry = (Array.isArray(index.entries) ? index.entries : []).find(row =>
      row.game === 'cs2' && row.folder === archive.folder &&
      typeof row.demoPath === 'string' && path.isAbsolute(row.demoPath) &&
      path.extname(row.demoPath).toLowerCase() === '.dem');
    if (!entry || !(await fsp.stat(entry.demoPath).catch(() => null))?.isFile())
      throw new Error('Could not recover saved CS2 match metadata. Your saves have been kept.');
    const target = entry.demoPath + '.info';
    if ((await fsp.stat(target).catch(() => null))?.isFile()) continue;
    const temporary = `${target}.mvm-recover-${randomUUID()}.tmp`;
    try {
      await fsp.copyFile(archive.archived, temporary, fs.constants.COPYFILE_EXCL);
      // An exclusive hard link publishes the complete same-volume copy atomically. Existing
      // files win even if CS2 wrote one between the initial check and publication.
      await fsp.link(temporary, target);
    } catch (error) {
      if (error.code !== 'EEXIST' || !(await fsp.stat(target).catch(() => null))?.isFile())
        throw new Error('Could not restore CS2 match metadata beside its demo. Your saves have been kept.');
    } finally {
      await fsp.unlink(temporary).catch(() => {});
    }
  }
}

/**
 * Runs the reset when this machine's saves predate SAVE_EPOCH. The caller holds the CS2-closed
 * guard (the hook would rewrite these files, and some are open while it runs).
 * Returns { reset: boolean, removed: string[] }.
 */
async function resetSavesIfNeeded(hlaeRoot) {
  const epochPath = path.join(hlaeRoot, EPOCH_FILE);
  let recorded = 0;
  try { recorded = parseEpoch(await fsp.readFile(epochPath, 'utf8')); } catch { recorded = 0; }
  const plan = planReset(recorded);
  if (!plan.length) return { reset: false, removed: [] };
  await restoreMovedMatchInfo(hlaeRoot);
  const removed = [];
  for (const relative of plan) {
    const target = path.join(hlaeRoot, relative);
    if (!fs.existsSync(target)) continue;
    await fsp.rm(target, { recursive: true, force: true });
    removed.push(relative);
  }
  await fsp.mkdir(hlaeRoot, { recursive: true });
  await fsp.writeFile(epochPath, `${SAVE_EPOCH}\n`, 'utf8');
  return { reset: true, removed };
}

module.exports = { SAVE_EPOCH, EPOCH_FILE, WIPED_DIRECTORIES, WIPED_FILES, parseEpoch, planReset, resetSavesIfNeeded };
