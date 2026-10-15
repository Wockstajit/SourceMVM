// The one place that knows how a *Js.h header is put together, shared by the extractor (header ->
// browser) and the applier (browser -> header). Both directions walking the same tokens is what
// makes the round trip exact: whatever the extractor pulls out, the applier can put back in the
// same slot, and a header that survives extract -> apply byte for byte is the proof.
//
// A header is C++ that carries its Panorama JS as adjacent raw-string literals:
//
//     inline const char* kConfigHudJs = R"CFJS(
//     ...js...
//     )CFJS"
//     #include "ConfigHudWidgetsJs.h"     <- another header's literals, spliced in by the compiler
//     R"CFJS(
//     ...more js...
//     )CFJS";
//
// tokenize() returns the literals and the includes in source order. A `//` earlier on the same line
// means the match is inside a comment (the headers talk about include paths and R"( a lot) and it is
// skipped.
import fs from 'node:fs';
import path from 'node:path';

const RAW_OPEN = /R"([A-Za-z0-9_]*)\(/g;
// A fragment is a *Js.h header or a generated *.inc data file. CameraEditorCosmeticsCatalog.inc is
// the catalog the Customize modal is built from; skipping it left the browser on the 5-item stub.
const INCLUDE = /^[ \t]*#include[ \t]+"([A-Za-z0-9_]+(?:Js\.h|\.inc))"[ \t]*\r?$/gm;

function inLineComment(src, idx) {
  const lineStart = src.lastIndexOf('\n', idx - 1) + 1;
  return src.slice(lineStart, idx).includes('//');
}

// Tokens: { type: 'raw', tag, bodyStart, bodyEnd } (body = src.slice(bodyStart, bodyEnd)) and
// { type: 'include', name }. Offsets are into the exact file text, so a caller can splice.
export function tokenize(src, file = '?') {
  const tokens = [];
  let i = 0;
  for (;;) {
    RAW_OPEN.lastIndex = i;
    let raw = RAW_OPEN.exec(src);
    while (raw && inLineComment(src, raw.index)) raw = RAW_OPEN.exec(src);

    INCLUDE.lastIndex = i;
    let inc = INCLUDE.exec(src);
    while (inc && inLineComment(src, inc.index)) inc = INCLUDE.exec(src);

    if (!raw && !inc) break;

    if (raw && (!inc || raw.index < inc.index)) {
      const closer = ')' + raw[1] + '"';
      const bodyStart = raw.index + raw[0].length;
      const bodyEnd = src.indexOf(closer, bodyStart);
      if (bodyEnd < 0) throw new Error(`unterminated raw string R"${raw[1]}(" in ${file}`);
      tokens.push({ type: 'raw', tag: raw[1], bodyStart, bodyEnd });
      i = bodyEnd + closer.length;
    } else {
      tokens.push({ type: 'include', name: inc[1] });
      i = inc.index + inc[0].length;
    }
  }
  return tokens;
}

// The markers the extractor writes into the generated JS. They are JS comments, so the code runs
// exactly as it would in game, and they are the only structural thing the applier looks for.
// Segment n of a file is its n-th raw-string literal (0-based, includes not counted).
export const SEG_RE = /\/\* @mvm:seg ([A-Za-z0-9_]+(?:Js\.h|\.inc))#(\d+) \*\/\n/g;
export const INC_OPEN_RE = /\/\* >>> ([A-Za-z0-9_]+(?:Js\.h|\.inc)) \*\//g;
export const INC_CLOSE_RE = /\/\* <<< ([A-Za-z0-9_]+(?:Js\.h|\.inc)) \*\//g;
export const TRAILER_RE = /\n?\/\/# sourceURL=mvm\/[A-Za-z0-9_-]+\.js\n?$/;

export function segMarker(file, n) { return `/* @mvm:seg ${file}#${n} */\n`; }

export function newlineOf(text) { return text.includes('\r\n') ? '\r\n' : '\n'; }

// Flatten one header (recursing into its includes) into the exact string the C++ hands to
// RunScript, with markers. Bodies are normalised to LF so the generated file has one convention no
// matter which headers it was stitched from; the applier restores each file's own on the way back.
export function flatten(panoDir, file, stack = []) {
  const full = path.join(panoDir, file);
  if (stack.includes(file)) throw new Error(`include cycle: ${[...stack, file].join(' -> ')}`);
  if (!fs.existsSync(full)) throw new Error(`missing header: ${full}`);
  const src = fs.readFileSync(full, 'utf8');
  const tokens = tokenize(src, file);
  const out = [];
  let n = 0;
  for (const t of tokens) {
    if (t.type === 'raw') {
      out.push(segMarker(file, n++));
      out.push(src.slice(t.bodyStart, t.bodyEnd).replace(/\r\n/g, '\n'));
    } else {
      out.push(`/* >>> ${t.name} */`);
      out.push(flatten(panoDir, t.name, [...stack, file]));
      out.push(`/* <<< ${t.name} */`);
    }
  }
  if (!n && !tokens.length) throw new Error(`no raw-string bodies found in ${file}`);
  return out.join('');
}

// Split generated JS back into { 'File.h': [body0, body1, ...] }. Everything between one marker
// and the next marker of any kind is that segment's body, verbatim.
export function unflatten(js) {
  const text = js.replace(TRAILER_RE, '');
  const marks = [];
  for (const re of [SEG_RE, INC_OPEN_RE, INC_CLOSE_RE]) {
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(text))) marks.push({ idx: m.index, len: m[0].length, re, m });
  }
  marks.sort((a, b) => a.idx - b.idx);
  const bodies = {};
  for (let k = 0; k < marks.length; k++) {
    const mk = marks[k];
    if (mk.re !== SEG_RE) continue;
    const file = mk.m[1], n = Number(mk.m[2]);
    const start = mk.idx + mk.len;
    const end = k + 1 < marks.length ? marks[k + 1].idx : text.length;
    (bodies[file] ||= [])[n] = text.slice(start, end);
  }
  return bodies;
}
