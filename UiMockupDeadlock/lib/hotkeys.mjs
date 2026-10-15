// The hotkeys the Deadlock hook polls, read out of the C++ that owns them -- never written down
// here. AfxHookSource2/Deadlock/DeadlockHotkeys.cpp keeps one table, `g_bindings[]`, with a row per
// key: the virtual key, the command it runs (or the resolver that picks one at press time), whether
// the free cam's own input takes precedence, and whether the workspace must be open. The browser
// feeds the same rows to the same gate (app.js), so a key added or re-bound in the C++ shows up in
// the mockup on the next extraction, and a key that needs the workspace open needs it here too.
//
// Deliberately not a C++ parser: it looks for the exact row shape the table uses, skips lines that
// are commented out, and fails loudly if the table is missing -- a silently empty keymap would read
// as "hotkeys do nothing in the browser", which is a real bug report in game.

import fs from 'node:fs';

// { 'K', "mirv_filmmaker marker add", nullptr, false, false, "K place", false, 0, 0 },
// { VK_SPACE, nullptr, &PlayPauseCommand, false, true, "Space play", false, 0, 0 },
const ROW = /^\s*\{\s*('(?:[A-Z0-9])'|VK_[A-Z0-9_]+)\s*,\s*("(?:[^"\\]|\\.)*"|nullptr)\s*,\s*(&[A-Za-z_][A-Za-z0-9_]*|nullptr)\s*,\s*(true|false)\s*,\s*(true|false)\s*,\s*"((?:[^"\\]|\\.)*)"/;

export function parseBindings(cppPath) {
  const src = fs.readFileSync(cppPath, 'utf8');
  const start = src.indexOf('g_bindings[]');
  if (start < 0) throw new Error(`${cppPath}: no g_bindings[] table found`);
  const end = src.indexOf('};', start);
  if (end < 0) throw new Error(`${cppPath}: g_bindings[] never closes`);
  const rows = [];
  for (const line of src.slice(start, end).split(/\r?\n/)) {
    if (/^\s*\/\//.test(line)) continue;
    const m = ROW.exec(line);
    if (!m) continue;
    rows.push({
      vk: m[1].replace(/^'|'$/g, ''),
      command: m[2] === 'nullptr' ? null : JSON.parse(m[2]),
      resolve: m[3] === 'nullptr' ? null : m[3].slice(1),
      standsDownForFreeCam: m[4] === 'true',
      requiresWorkspace: m[5] === 'true',
      name: m[6],
    });
  }
  if (!rows.length) throw new Error(`${cppPath}: g_bindings[] has no rows this parser recognises`);
  return rows;
}

// Windows virtual-key names -> KeyboardEvent.key, for the keys the table uses. A letter binding is
// the letter itself; anything unknown is reported so it is added here rather than silently dead.
const VK_TO_KEY = {
  VK_SPACE: ' ', VK_LEFT: 'ArrowLeft', VK_RIGHT: 'ArrowRight', VK_UP: 'ArrowUp', VK_DOWN: 'ArrowDown',
  VK_ESCAPE: 'Escape', VK_RETURN: 'Enter', VK_BACK: 'Backspace', VK_DELETE: 'Delete', VK_TAB: 'Tab',
  VK_F1: 'F1', VK_F2: 'F2', VK_F3: 'F3', VK_F4: 'F4', VK_F5: 'F5', VK_F6: 'F6', VK_F7: 'F7',
  VK_F8: 'F8', VK_F9: 'F9', VK_F10: 'F10', VK_F11: 'F11', VK_F12: 'F12',
};
export function browserKey(vk) {
  if (/^[A-Z0-9]$/.test(vk)) return vk;
  if (VK_TO_KEY[vk]) return VK_TO_KEY[vk];
  throw new Error(`no browser key for ${vk} -- add it to VK_TO_KEY in lib/hotkeys.mjs`);
}

export function bindings(cppPath) {
  return parseBindings(cppPath).map((b) => ({ ...b, key: browserKey(b.vk) }));
}
