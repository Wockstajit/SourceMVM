// One owner for startup checks, downloads, restart, and permission to launch CS2.
// Electron and HTTP boundaries are injected so failure/race cases run without installing anything.
//
// The network is optional. automaticUpdates() is the user's setting (off on a fresh install): an
// automatic check (startup, headless, the setting being turned on) touches the update server only
// when it is on, while a manual check (the UPDATES button) always does. Either way an update
// server that cannot be reached is logged once and the installed runtime launches as it is.
//
// An update is never forced. A check that finds one readies the runtime and reports it as
// pending (`update` on the ready status), so the window can ask "new update, do you want it?".
// Only install() downloads it and restarts, and a declined or failed install leaves the current
// app launchable: the UPDATES button downloads it when the user is ready.
function createUpdateService({ updater, packaged, portable, loadReleases, prepareRuntime,
  canInstall, automaticUpdates = () => true, report = () => {}, log = () => {} }) {
  // canInstall() is true when nothing blocks writing the app or runtime. Otherwise it is false or
  // a sentence naming the blocker (which process, or why running processes could not be listed).
  function assertInstallable(action) {
    const verdict = canInstall();
    if (verdict === true) return;
    throw new Error(typeof verdict === 'string' && verdict ? verdict : `Close CS2 before ${action}.`);
  }
  let status = { state: 'checking', launchAllowed: false };
  let inFlight = null;
  let lastError = null;
  // The update the last check found: { version, installable }. Cleared by a check that finds none.
  let pendingUpdate = null;

  function publish(next) {
    status = { ...next, launchAllowed: next.state === 'ready' };
    report({ ...status });
    return { ...status };
  }
  // Ready, with the pending update (if any) attached so the window can offer it.
  function ready(fields) {
    return publish(pendingUpdate ? { state: 'ready', ...fields, update: { ...pendingUpdate } } : { state: 'ready', ...fields });
  }
  function fail(error) {
    lastError = error instanceof Error ? error : new Error(String(error));
    return publish({ state: 'error', message: `${lastError.message} Retry UPDATES to continue.` });
  }
  function assertCanLaunch() {
    if (!status.launchAllowed) {
      throw new Error(status.message || 'Wait for SOURCE:MVM updates to finish before launching.');
    }
  }
  async function run({ manual }) {
    lastError = null;
    publish({ state: 'checking' });
    try {
      // A source checkout has no installed app to replace. Preserve developer launch overrides.
      if (!packaged) return publish({ state: 'ready', message: 'DEVELOPMENT BUILD' });
      const online = manual || automaticUpdates() === true;
      let message = 'UPDATES OFF';
      let skipped = false;
      let releases = null;
      if (online) {
        try {
          releases = await loadReleases();
        } catch (error) {
          // No internet, or the update server is down. Say so once and launch what is installed.
          skipped = true;
          message = 'OFFLINE / UPDATE CHECK SKIPPED';
          log(`Update check skipped: ${error instanceof Error ? error.message : error}`);
        }
      }
      if (releases !== null) {
        if (!Array.isArray(releases)) throw new Error('The update server returned an invalid release list.');
        const published = releases.filter(release => !release.draft);
        message = published.length ? 'UP TO DATE' : 'NO UPDATES PUBLISHED';
        if (published.length) {
          // A public release must be complete before leaving draft. Never mistake a missing feed
          // for "current" or fall back to an older build during a partially published update.
          if (!published[0].assets?.some(asset => asset.name === 'latest.yml')) {
            throw new Error('The published update is missing latest.yml.');
          }
          const result = await updater.checkForUpdates();
          if (lastError) throw lastError;
          if (!result || typeof result.isUpdateAvailable !== 'boolean') {
            throw new Error('The launcher update check did not complete.');
          }
          if (result.isUpdateAvailable) {
            const version = result.updateInfo?.version || '';
            // A portable exe cannot replace itself; the user installs the setup exe by hand.
            pendingUpdate = { version, installable: !portable };
            message = version ? `UPDATE ${version} AVAILABLE` : 'UPDATE AVAILABLE';
            if (portable) message += ' / INSTALL THE NEW SETUP EXE';
          } else {
            pendingUpdate = null;
          }
        }
      }
      assertInstallable('checking or installing the runtime');
      // online: the runtime side (hotfix, payload manifest) may use the network too. Off or
      // unreachable, it installs only what the app already carries.
      await prepareRuntime({ online: online && !skipped });
      if (lastError) throw lastError;
      return ready(skipped ? { message, skipped } : { message });
    } catch (error) {
      return fail(error);
    }
  }
  // Download and install the pending update, then restart. GO is locked from the first byte
  // until the restarted app is ready, because a half-installed app must not launch anything.
  async function runInstall() {
    lastError = null;
    const update = pendingUpdate;
    try {
      if (!update) throw new Error('No update is known. Press UPDATES to check.');
      if (!update.installable) throw new Error('Portable builds cannot replace themselves. Install the new setup exe or download the new portable zip.');
      assertInstallable('updating SOURCE:MVM');
      publish({ state: 'available', version: update.version });
      const files = await updater.downloadUpdate();
      if (lastError) throw lastError;
      if (!Array.isArray(files) || !files.length) throw new Error('The launcher update was not downloaded.');
      publish({ state: 'downloaded', version: update.version });
      assertInstallable('installing the downloaded update');
      publish({ state: 'installing', version: update.version });
      // The updater verifies its SHA-512/signature policy before downloadUpdate resolves.
      // Do not permit this older process to launch after download: only the restarted app
      // can install its matching bundled runtime and become ready.
      updater.quitAndInstall(false, true);
      return { ...status };
    } catch (error) {
      // Nothing on disk changed, so the current app stays launchable. The update stays pending
      // and the UPDATES button tries again.
      lastError = null;
      const reason = error instanceof Error ? error.message : String(error);
      log(`Update not installed: ${reason}`);
      return ready({ message: `UPDATE NOT INSTALLED: ${reason} PRESS UPDATES TO TRY AGAIN.` });
    }
  }
  function start(operation) {
    if (status.state === 'installing') return Promise.resolve({ ...status });
    if (!inFlight) {
      // Publish synchronously: a launch IPC cannot slip in before the async work starts.
      publish({ state: 'checking' });
      inFlight = Promise.resolve().then(operation).finally(() => { inFlight = null; });
    }
    return inFlight;
  }
  // manual: the user pressed UPDATES, which checks once whatever the setting says.
  function check({ manual = false } = {}) {
    return start(() => run({ manual }));
  }
  // The user said yes to the pending update (the prompt, or UPDATES while one is pending).
  function install() {
    return start(runInstall);
  }
  function progress(percent) {
    if (status.state === 'available' || status.state === 'downloading') {
      publish({ state: 'downloading', version: status.version, percent });
    }
  }
  return { check, install, fail, progress, assertCanLaunch, getStatus: () => ({ ...status }) };
}

module.exports = { createUpdateService };
