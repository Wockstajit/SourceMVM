// Is CS2 running, and if so which process? The update service must not write the app or the
// runtime while cs2.exe has them loaded, and a refusal has to say why: users saw only
// "CLOSE CS2 BEFORE CHECKING OR INSTALLING THE RUNTI..." with no CS2 open, because a failed
// or slow process listing was reported as "CS2 is running" (2026-10-13).
// `exec(file, args, options)` is execFileSync, injected so tests run without real processes.

// `tasklist /FO CSV /NH` rows: "cs2.exe","1234","Console","1","1,234,567 K". A filter with no match
// prints a localized INFO line instead, which matches nothing here.
function parseTasklistCsv(text) {
  const ids = [];
  for (const line of String(text).split(/\r?\n/)) {
    const match = /^"cs2\.exe","(\d+)"/i.exec(line.trim());
    if (match) ids.push(Number(match[1]));
  }
  return ids;
}

// { ids } with every cs2.exe process id (possibly empty), or { error } when neither method could
// list processes. tasklist first; PowerShell when tasklist is missing, blocked or too slow.
function findCs2Processes(exec) {
  const failures = [];
  try {
    const out = exec('tasklist.exe', ['/FI', 'IMAGENAME eq cs2.exe', '/FO', 'CSV', '/NH'],
      { encoding: 'utf8', windowsHide: true, timeout: 8000 });
    return { ids: parseTasklistCsv(out) };
  } catch (error) {
    failures.push(`tasklist: ${error?.message || error}`);
  }
  try {
    const out = exec('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
      '(Get-Process -Name cs2 -ErrorAction SilentlyContinue).Id'],
      { encoding: 'utf8', windowsHide: true, timeout: 15000 });
    return { ids: String(out).split(/\s+/).filter(Boolean).map(Number).filter(Number.isInteger) };
  } catch (error) {
    failures.push(`PowerShell: ${error?.message || error}`);
  }
  return { error: failures.join('; ') };
}

// true when nothing blocks installing; otherwise the sentence the launcher shows.
function installBlocker({ launching, launchedPid, found }) {
  if (launching) return 'CS2 is being launched. Close it once it has started.';
  const ids = found.ids || [];
  if (launchedPid && !ids.includes(launchedPid)) ids.unshift(launchedPid);
  if (ids.length) {
    const which = ids.length === 1 ? `cs2.exe (PID ${ids[0]}) is` : `cs2.exe (PIDs ${ids.join(', ')}) are`;
    return `${which} still running. Close CS2; if it is stuck after a crash, end cs2.exe in Task Manager > Details.`;
  }
  if (found.error) {
    return `Could not check whether CS2 is running (${found.error}). If CS2 is closed, restart the launcher.`;
  }
  return true;
}

module.exports = { parseTasklistCsv, findCs2Processes, installBlocker };
