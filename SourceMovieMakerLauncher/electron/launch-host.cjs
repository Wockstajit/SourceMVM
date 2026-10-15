const fs = require('node:fs');
const { spawn } = require('node:child_process');

// Wait for the host's exit: its resumed message precedes the startup observation.
function runLaunchHost({ host, args, cwd, env, logPath }, start = spawn) {
  return new Promise((resolve, reject) => {
    let stdout = '';
    let stderr = '';
    let settled = false;
    const finish = (code, error) => {
      if (settled) return;
      settled = true;
      const match = stdout.match(/^Created CS2 process (\d+) suspended\.\r?$/m);
      if (!error && code === 0 && !match) error = new Error('The launcher host did not report a game process.');
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
      if (!error && code === 0) {
        resolve({ ok: true, output: stdout + stderr, processId: Number(match[1]) });
        return;
      }
      // Prefer stderr: stdout includes SteamPath and creation progress, not the failure.
      const message = error?.message || stderr.trim()
        || `Launcher host exited with code ${code}.`;
      reject(new Error(message + (logged ? `\nLaunch log: ${logPath}` : '')));
    };
    try {
      const child = start(host, args, { cwd, env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
      // Stream decoding preserves characters split across data chunks (including local paths).
      child.stdout.setEncoding('utf8');
      child.stderr.setEncoding('utf8');
      child.stdout.on('data', chunk => { stdout += chunk; });
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
