// Shared reader for the localization catalog: finds every T('...') and TN(n, '...', '...') call in
// the Panorama script headers and returns the English keys they use.
//
// The scripts are ES5 inside C++ raw string literals, so this is a small scanner rather than a
// parser: it walks the text, skips comments and string/regex bodies, and reads the literal
// arguments of each call. A call whose text argument is not a plain string literal is reported as
// a problem, because the catalog (and so every translation) is built from these literals.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
export const LOCALIZATION_DIR = path.join(REPO, 'AfxHookSource2', 'Filmmaker', 'Localization');
export const LANG_DIR = path.join(LOCALIZATION_DIR, 'lang');

// CS2 only for now. Deadlock has its own UI scripts and is not localized yet.
export const SCRIPT_DIRS = [
  'AfxHookSource2/Filmmaker/Panorama',
  'AfxHookSource2/Filmmaker/PlayMenu',
  'AfxHookSource2/Filmmaker/Community',
];

// CS2's language names, as `cl_language` / $.Language() report them, with each language's own
// name for the picker. English is the source text and has no table.
export const LANGUAGES = [
  ['english', 'English', 1],
  ['brazilian', 'Português (Brasil)', 2],
  ['bulgarian', 'Български', 2],
  ['czech', 'Čeština', 3],
  ['danish', 'Dansk', 2],
  ['dutch', 'Nederlands', 2],
  ['finnish', 'Suomi', 2],
  ['french', 'Français', 2],
  ['german', 'Deutsch', 2],
  ['greek', 'Ελληνικά', 2],
  ['hungarian', 'Magyar', 2],
  ['indonesian', 'Bahasa Indonesia', 1],
  ['italian', 'Italiano', 2],
  ['japanese', '日本語', 1],
  ['koreana', '한국어', 1],
  ['latam', 'Español (Latinoamérica)', 2],
  ['norwegian', 'Norsk', 2],
  ['polish', 'Polski', 3],
  ['portuguese', 'Português', 2],
  ['romanian', 'Română', 3],
  ['russian', 'Русский', 3],
  ['schinese', '简体中文', 1],
  ['spanish', 'Español (España)', 2],
  ['swedish', 'Svenska', 2],
  ['tchinese', '繁體中文', 1],
  ['thai', 'ไทย', 1],
  ['turkish', 'Türkçe', 2],
  ['ukrainian', 'Українська', 3],
  ['vietnamese', 'Tiếng Việt', 1],
].map(([id, name, pluralForms]) => ({ id, name, pluralForms }));

export function scriptFiles() {
  const files = [];
  for (const dir of SCRIPT_DIRS) {
    const abs = path.join(REPO, dir);
    if (!fs.existsSync(abs)) continue;
    for (const f of fs.readdirSync(abs).sort()) {
      if (f.endsWith('Js.h')) files.push(path.join(dir, f).replace(/\\/g, '/'));
    }
  }
  return files;
}

function readStringLiteral(src, at) {
  const quote = src[at];
  let out = '';
  let i = at + 1;
  for (; i < src.length; i++) {
    const ch = src[i];
    if (ch === quote) return { value: out, end: i + 1 };
    if (ch === '\n') return null;
    if (ch !== '\\') { out += ch; continue; }
    const next = src[++i];
    if (next === 'n') out += '\n';
    else if (next === 't') out += '\t';
    else if (next === 'u') { out += String.fromCharCode(parseInt(src.substr(i + 1, 4), 16)); i += 4; }
    else if (next === 'x') { out += String.fromCharCode(parseInt(src.substr(i + 1, 2), 16)); i += 2; }
    else out += next;
  }
  return null;
}

function skipSpace(src, i) {
  while (i < src.length && /\s/.test(src[i])) i++;
  return i;
}

// Reads `'a' + 'b'` as one literal, so a long sentence can be broken across lines in the source.
function readLiteralArgument(src, at) {
  let i = skipSpace(src, at);
  if (src[i] !== "'" && src[i] !== '"') return null;
  let value = '';
  for (;;) {
    const lit = readStringLiteral(src, i);
    if (!lit) return null;
    value += lit.value;
    i = skipSpace(src, lit.end);
    if (src[i] !== '+') break;
    const after = skipSpace(src, i + 1);
    if (src[after] !== "'" && src[after] !== '"') return null;
    i = after;
  }
  return { value, end: i };
}

// Skips one argument that is an arbitrary expression (TN's count), up to its top-level comma.
function skipExpression(src, at) {
  let depth = 0;
  for (let i = at; i < src.length; i++) {
    const ch = src[i];
    if (ch === "'" || ch === '"') { const lit = readStringLiteral(src, i); if (!lit) return -1; i = lit.end - 1; continue; }
    if (ch === '(' || ch === '[' || ch === '{') depth++;
    else if (ch === ')' || ch === ']' || ch === '}') { if (depth === 0) return -1; depth--; }
    else if (ch === ',' && depth === 0) return i;
  }
  return -1;
}

export function pluralKey(one, other) { return one + ' || ' + other; }

// -> { keys: Map(key -> { plural, files:Set }), problems: [{file, line, text}] }
export function collect(files = scriptFiles()) {
  const keys = new Map();
  const problems = [];
  for (const file of files) {
    const src = fs.readFileSync(path.join(REPO, file), 'utf8');
    const lineOf = (index) => src.slice(0, index).split('\n').length;
    const add = (key, plural) => {
      if (!keys.has(key)) keys.set(key, { plural, files: new Set() });
      keys.get(key).files.add(file);
    };
    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      if (ch === '/' && src[i + 1] === '/') { i = src.indexOf('\n', i); if (i < 0) break; continue; }
      if (ch === '/' && src[i + 1] === '*') { i = src.indexOf('*/', i) + 1; if (i <= 0) break; continue; }
      if (ch === "'" || ch === '"') { const lit = readStringLiteral(src, i); if (lit) i = lit.end - 1; continue; }
      if (ch !== 'T') continue;
      if (i > 0 && /[\w$.)\]]/.test(src[i - 1])) continue;   // a name, a member, or a regex group
      const isPlural = src[i + 1] === 'N' && src[i + 2] === '(';
      if (!isPlural && src[i + 1] !== '(') continue;
      const open = i + (isPlural ? 2 : 1);
      if (!isPlural) {
        const arg = readLiteralArgument(src, open + 1);
        if (!arg || !/[,)]/.test(src[arg.end])) {
          problems.push({ file, line: lineOf(i), text: 'T() needs a string literal as its first argument' });
          continue;
        }
        add(arg.value, false);
        i = arg.end - 1;
      } else {
        const comma = skipExpression(src, open + 1);
        const one = comma >= 0 ? readLiteralArgument(src, comma + 1) : null;
        const other = one && src[one.end] === ',' ? readLiteralArgument(src, one.end + 1) : null;
        if (!other || !/[,)]/.test(src[other.end])) {
          problems.push({ file, line: lineOf(i), text: 'TN() needs two string literals after the count' });
          continue;
        }
        add(pluralKey(one.value, other.value), true);
        i = other.end - 1;
      }
    }
  }
  return { keys, problems };
}

export function placeholders(text) {
  return [...new Set(String(text).match(/\{\d\}/g) || [])].sort();
}

export function readTable(lang) {
  const file = path.join(LANG_DIR, lang + '.json');
  return fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : null;
}
