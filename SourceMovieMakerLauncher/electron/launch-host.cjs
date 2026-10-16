const fs = require('node:fs');
const { spawn } = require('node:child_process');

// Resolves as soon as the host reports the resumed game, so the launch never waits on the
// optional startup watch. A host failure after that is the game exiting early and goes to
// onStartupExit(message, processId) instead of the already-settled promise.
function runLaunchHost({ host, args, cwd, env, logPath, onStartupExit }, start = spawn) {
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    let settled = false;
    let closed = false;
    let hostClosed;
    const hostExited = new Promise(done => { hostClosed = done; });
    const processId = () => {
      const match = stdout.match(/^Created CS2 process (\d+) suspended\.\r?$/m);
      return match ? Number(match[1]) : 0;
    };
    const resumed = () => /^Hook injected; CS2 resumed\.\r?$/m.test(stdout) && processId() > 0;
    const succeed = () => {
      settled = true;
      resolve({ ok: true, output: stdout + stderr, processId: processId(), hostExited });
    };
    const finish = (code, error) => {
      if (closed) return;
      closed = true;
      if (!error && code === 0 && !processId()) error = new Error('The launcher host did not report a game process.');
      let logged = false;
      try {
        fs.appendFileSync(logPath, `\n[${new Date().toISOString()}] host exited ${code}\n`
          + `Host: ${host}\nArguments: ${JSON.stringify(args)}\n`
          + `Working directory: ${cwd}\nstdout:\n${stdout}\nstderr:\n${stderr}\n`
          + (error ? `Host error: ${error.message}\n` : ''));
        logged = true;
      } catch {
        // A read-only/full log destination must not hide the original result.
      }
      const failed = Boolean(error) || code !== 0;
      if (!failed) {
        if (!settled) succeed();
        hostClosed();
        return;
      }
      // Prefer stderr: stdout includes SteamPath and creation progress, not the failure.
      const message = (error?.message || stderr.trim() || `Launcher host exited with code ${code}.`)
        + (logged ? `\nLaunch log: ${logPath}` : '');
      if (!settled) {
        settled = true;
        reject(new Error(message));
      } else if (onStartupExit) {
        onStartupExit(message, processId());
      }
      hostClosed();
    };
    try {
      const child = start(host, args, { cwd, env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
      // Stream decoding preserves characters split across data chunks (including local paths).
      child.stdout.setEncoding('utf8');
      child.stderr.setEncoding('utf8');
      child.stdout.on('data', chunk => {
        stdout += chunk;
        if (!settled && resumed()) succeed();
      });
      child.stderr.on('data', chunk => { stderr += chunk; });
      child.on('error', error => {
        const denied = error.code === 'EACCES' || error.code === 'EPERM';
        finish(null, new Error(denied
          ? `Windows blocked the launcher host. Check its file permissions and Windows Security Protection history.\nHost: ${host}\n${error.message}`
          : error.message));
      });
      child.on('close', code => finish(code));
    } catch (error) {
      finish(null, error);
    }
  });
}

module.exports = { runLaunchHost };
