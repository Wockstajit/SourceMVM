// One owner for startup checks, downloads, restart, and permission to launch CS2.
// Electron and HTTP boundaries are injected so failure/race cases run without installing anything.
function createUpdateService({ updater, packaged, portable, loadReleases, prepareRuntime,
  canInstall, report = () => {} }) {
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

  function publish(next) {
    status = { ...next, launchAllowed: next.state === 'ready' };
    report({ ...status });
    return { ...status };
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
  async function run() {
    lastError = null;
    publish({ state: 'checking' });
    try {
      // A source checkout has no installed app to replace. Preserve developer launch overrides.
      if (!packaged) return publish({ state: 'ready', message: 'DEVELOPMENT BUILD' });
      const releases = await loadReleases();
      if (!Array.isArray(releases)) throw new Error('The update server returned an invalid release list.');
      const published = releases.filter(release => !release.draft);
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
          const version = result.updateInfo?.version;
          if (portable) throw new Error('A launcher update is required. Install the new setup exe; portable builds cannot replace themselves.');
          assertInstallable('updating SOURCE:MVM');
          publish({ state: 'available', version });
          const files = await updater.downloadUpdate();
          if (lastError) throw lastError;
          if (!Array.isArray(files) || !files.length) throw new Error('The launcher update was not downloaded.');
          publish({ state: 'downloaded', version });
          assertInstallable('installing the downloaded update');
          publish({ state: 'installing', version });
          // The updater verifies its SHA-512/signature policy before downloadUpdate resolves.
          // Do not permit this older process to launch after download: only the restarted app
          // can install its matching bundled runtime and become ready.
          updater.quitAndInstall(false, true);
          return { ...status };
        }
      }
      assertInstallable('checking or installing the runtime');
      await prepareRuntime();
      if (lastError) throw lastError;
      return publish({ state: 'ready', message: published.length ? 'UP TO DATE' : 'NO UPDATES PUBLISHED' });
    } catch (error) {
      return fail(error);
    }
  }
  function check() {
    if (status.state === 'installing') return Promise.resolve({ ...status });
    if (!inFlight) {
      // Publish synchronously: a launch IPC cannot slip in before the async check starts.
      publish({ state: 'checking' });
      inFlight = Promise.resolve().then(run).finally(() => { inFlight = null; });
    }
    return inFlight;
  }
  function progress(percent) {
    if (status.state === 'available' || status.state === 'downloading') {
      publish({ state: 'downloading', version: status.version, percent });
    }
  }
  return { check, fail, progress, assertCanLaunch, getStatus: () => ({ ...status }) };
}

module.exports = { createUpdateService };
