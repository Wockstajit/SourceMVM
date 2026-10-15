// Runtime hotfixes: a new hook DLL without a new installer.
//
// The complete installer carries the runtime (hook DLL, injector, resources) and the FX pack.
// Re-shipping all of it for a hook-only fix made every user download the whole installer again.
// A hotfix is instead two assets added to the CURRENT app release (tag v<appVersion>):
//
//   runtime-hotfix.json   { schema, appVersion, serial, runtime: { asset, url, bytes, sha256 } }
//   source-mvm-runtime-<appVersion>-hotfix<serial>.zip
//
// No new app version and no latest.yml, so electron-updater never offers an installer for it.
// A full release (the EXE changed) is a new version: its bundle is newer than any hotfix of the
// previous version, and the serial starts again at 0.
//
// Pure rules only (no Electron, no network), so Automation/tests can run them.

const HOTFIX_ASSET = 'runtime-hotfix.json';
const HOTFIX_SCHEMA = 1;

/**
 * The hotfix to install, or null when there is nothing newer for this app version.
 * Throws on a malformed manifest (the caller reports it and keeps the installed runtime).
 */
function pickHotfix(manifest, appVersion, installedSerial) {
  if (!manifest || typeof manifest !== 'object') throw new Error('The runtime hotfix manifest is not an object.');
  if (manifest.schema !== HOTFIX_SCHEMA) {
    throw new Error(`The runtime hotfix is schema ${manifest.schema}; this launcher understands ${HOTFIX_SCHEMA}.`);
  }
  // A hotfix is built against one app version: never apply it to another.
  if (manifest.appVersion !== appVersion) return null;
  const serial = manifest.serial;
  if (!Number.isInteger(serial) || serial < 1) throw new Error('The runtime hotfix has no valid serial.');
  if (serial <= (Number.isInteger(installedSerial) ? installedSerial : 0)) return null;
  const runtime = manifest.runtime || {};
  if (typeof runtime.url !== 'string' || !runtime.url.startsWith('https://')) {
    throw new Error('The runtime hotfix has no https download URL.');
  }
  if (typeof runtime.sha256 !== 'string' || !/^[0-9a-f]{64}$/i.test(runtime.sha256)) {
    throw new Error('The runtime hotfix has no valid sha256.');
  }
  return { serial, url: runtime.url, sha256: runtime.sha256.toLowerCase(), bytes: runtime.bytes || 0,
    version: `${appVersion}-hotfix${serial}` };
}

/** Whether the bundled runtime must be (re)installed: a different bundle, or nothing installed. */
function bundleRuntimeWanted(installedEntry, bundleStamp, present) {
  return !present || !installedEntry || installedEntry.bundleStamp !== bundleStamp;
}

module.exports = { HOTFIX_ASSET, HOTFIX_SCHEMA, pickHotfix, bundleRuntimeWanted };
