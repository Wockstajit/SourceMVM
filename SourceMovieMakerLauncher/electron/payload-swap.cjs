// Replacing an installed payload component without losing what CS2 saved inside it.
//
// The launcher points USRLOCALCSGO at <userData>/payload/fx/source_mvm_fx, and the hook mounts that
// folder first under USRLOCAL, so CS2 writes its own settings into it: cfg\cs2_video.txt (video),
// cfg\cs2_machine_convars.vcfg (volume and the other snd_* settings), cfg\cs2_user_convars_*.vcfg and
// cfg\cs2_user_keys_*.vcfg (settings and binds), their .bak and _lastclouded copies, and
// ui_override\ServerConfig.vdf (bot settings). Updates used to delete the folder and put the new
// pack in its place, so every FX update reset the player's CS2 settings.
//
// replaceDirectory moves the old folder aside, puts the new one in place, copies the kept paths
// across, and only then deletes the old folder. Any failure puts the old folder back as it was.
//
// No Electron here, so Automation/tests can run it on temporary folders.

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');

// Relative to the FX component folder (<userData>/payload/fx).
const FX_SAVED_SETTINGS = [
  path.join('source_mvm_fx', 'cfg'),
  path.join('source_mvm_fx', 'ui_override', 'ServerConfig.vdf'),
];

// CS2's own settings files. When the new package has a file of the same name, the player's copy of
// one of these wins; any other kept file gives way to the package's.
const CS2_SETTING = /^(?:cs2_video\.txt|cs2_[^\\/]*\.vcfg|ServerConfig\.vdf)/i;

function isCs2Setting(name) {
  return CS2_SETTING.test(name);
}

function previousDir(target) {
  return `${target}.previous`;
}

// Windows can refuse a folder move for a moment while a virus scanner or the search indexer still
// has a file in it open. Retry briefly (about two seconds) before giving up.
async function moveFolder(from, to) {
  for (let attempt = 1; ; attempt += 1) {
    try {
      await fsp.rename(from, to);
      return;
    } catch (error) {
      if (attempt >= 10 || !['EPERM', 'EACCES', 'EBUSY'].includes(error.code)) throw error;
      await new Promise((resolve) => setTimeout(resolve, 200));
    }
  }
}

function lockedError(target, error) {
  // The overwhelmingly likely cause on Windows is CS2 still holding a file in the folder.
  return new Error(
    `Could not replace ${target}.\n`
    + 'Close Counter-Strike 2 and try again.\n'
    + error.message,
  );
}

/**
 * Copy the file or folder `from` to `to`, file by file. Where `to` already has a file, it is
 * replaced only when wins(fileName) says so. Returns how many files were copied.
 */
async function copyKept(from, to, wins) {
  let stat;
  try {
    stat = await fsp.stat(from);
  } catch (error) {
    if (error.code === 'ENOENT') return 0;
    throw error;
  }
  if (stat.isFile()) {
    if (fs.existsSync(to) && !wins(path.basename(from))) return 0;
    await fsp.mkdir(path.dirname(to), { recursive: true });
    await fsp.copyFile(from, to);
    return 1;
  }
  if (!stat.isDirectory()) return 0;
  let copied = 0;
  for (const entry of await fsp.readdir(from)) {
    copied += await copyKept(path.join(from, entry), path.join(to, entry), wins);
  }
  return copied;
}

async function carryOver(from, to, keep, wins) {
  let copied = 0;
  for (const relative of keep) {
    copied += await copyKept(path.join(from, relative), path.join(to, relative), wins);
  }
  return copied;
}

// A replace that was cut short (the launcher closed or crashed part way) leaves <target>.previous
// behind. With no target it is the last good install, so it goes back. Beside a target it can still
// hold settings the target lacks: those are copied over (the target's own files are newer and
// win), then it is removed.
async function recoverInterrupted(target, keep) {
  const previous = previousDir(target);
  if (!fs.existsSync(previous)) return;
  if (!fs.existsSync(target)) {
    await moveFolder(previous, target);
    return;
  }
  await carryOver(previous, target, keep, () => false);
  await fsp.rm(previous, { recursive: true, force: true });
}

/**
 * Replace the folder `target` with a new one. `install(target)` creates it (by a move or a copy)
 * and may return an undo function, which runs if a later step fails (a move gives its source back).
 * `keep` lists paths relative to `target` that are copied over from the old folder. Returns
 * { kept }, the number of files carried over.
 */
async function replaceDirectory(target, install, keep = []) {
  const previous = previousDir(target);
  try {
    await recoverInterrupted(target, keep);
  } catch (error) {
    throw lockedError(target, error);
  }
  const hadPrevious = fs.existsSync(target);
  if (hadPrevious) {
    // A folder move either works or changes nothing. Deleting first could stop part way on a file
    // CS2 still holds and leave neither the old install nor the new one.
    try {
      await moveFolder(target, previous);
    } catch (error) {
      throw lockedError(target, error);
    }
  }
  let undo = null;
  let kept = 0;
  try {
    await fsp.mkdir(path.dirname(target), { recursive: true });
    undo = await install(target);
    if (hadPrevious) kept = await carryOver(previous, target, keep, isCs2Setting);
  } catch (error) {
    if (typeof undo === 'function') await Promise.resolve().then(undo).catch(() => {});
    await fsp.rm(target, { recursive: true, force: true }).catch(() => {});
    if (!hadPrevious) throw new Error(`Could not install ${target}.\n${error.message}`);
    const restored = await moveFolder(previous, target).then(() => true, () => false);
    throw new Error(restored
      ? `Could not install ${target} (the previous install was kept).\n${error.message}`
      : `Could not install ${target}. The previous install was left in ${previous}, and the next try uses it.\n${error.message}`);
  }
  await fsp.rm(previous, { recursive: true, force: true }).catch(() => {});
  return { kept };
}

module.exports = { FX_SAVED_SETTINGS, isCs2Setting, replaceDirectory };
