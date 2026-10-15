// The one place that knows how a Deadlock *Js.h header carries its Panorama JS, shared by the
// extractor (header -> browser) and the applier (browser -> header). Both directions walk the same
// tokens, which is what makes the round trip exact: whatever comes out can go back into the same
// slot, and a header that survives extract -> apply byte for byte is the proof.
//
// DEADLOCK IS SHAPED DIFFERENTLY FROM CS2, and that is why this is its own module rather than a
// parameter on the CS2 one. A CS2 surface is a HEADER that #includes its fragments, so the
// compiler concatenates them and the extraction unit is the file. Deadlock has no root header at
// all: every fragment is a named MACRO, and DeadlockWorkspaces.cpp pushes twelve of them through
// RunScript one after another into a single shared JavaScript scope. So the unit here is the
// #define, not the file -- DeadlockConfigRenderBodyJs.h holds two of them -- and the ORDER comes
// from the C++ rather than from an include graph.
//
//     #define DEADLOCK_CAMERA_BODY_JS R"CFJS(
//     ...js...
//     )CFJS"
//
// A long fragment is SPLIT, because MSVC caps one raw literal at 16380 bytes and silently
// truncates past it. The split is adjacent literals with the seam on a single line:
//
//     )CFJS" R"CFJS(  // more js
//
// Adjacent literals concatenate into one string, so the macro continues. The seam must stay on one
// line: a bare newline between them would end the #define and the rest of the macro would become
// stray C++.

import fs from 'node:fs';
import path from 'node:path';

const OPEN = 'R"CFJS(';
const CLOSE = ')CFJS"';

// Only spaces and tabs may separate two literals of the same macro. A newline there ends the
// #define, so an opener on the next line belongs to a different macro and must not be swallowed
// into this one -- which would silently merge two fragments into one and lose the boundary the
// applier needs to put them back.
const ADJACENT = /^[ \t]*$/;

function lineStart(src, index) {
  return src.lastIndexOf('\n', index - 1) + 1;
}

function inLineComment(src, index) {
  return src.slice(lineStart(src, index), index).includes('//');
}

// Every macro in one header, in source order:
//   { name, literals: [{ bodyStart, bodyEnd }], defineStart }
// Offsets are into the exact file text, so a caller can splice without re-reading.
export function tokenize(src, file = '?') {
  const macros = [];
  const define = /^[ \t]*#define[ \t]+(DEADLOCK_[A-Z0-9_]+_JS)[ \t]+R"CFJS\(/gm;
  let m;
  while ((m = define.exec(src))) {
    if (inLineComment(src, m.index)) continue;
    const literals = [];
    let at = m.index + m[0].length;
    for (;;) {
      const end = src.indexOf(CLOSE, at);
      if (end < 0) throw new Error(`${file}: unterminated R"CFJS( in ${m[1]}`);
      literals.push({ bodyStart: at, bodyEnd: end });
      const afterClose = end + CLOSE.length;
      const next = src.indexOf(OPEN, afterClose);
      if (next < 0 || !ADJACENT.test(src.slice(afterClose, next))) break;
      at = next + OPEN.length;
    }
    macros.push({ name: m[1], literals, defineStart: m.index });
    define.lastIndex = literals[literals.length - 1].bodyEnd;
  }
  if (!macros.length) throw new Error(`${file}: no #define DEADLOCK_*_JS R"CFJS( found`);
  return macros;
}

// The markers the extractor writes into the generated JS. They are JS comments, so the code runs
// exactly as it does in game, and they are the only structural thing the applier looks for.
// Literal n of a macro is its n-th raw-string body (0-based).
export const SEG_RE = /\/\* @mvm:seg ([A-Za-z0-9_]+Js\.h)#(DEADLOCK_[A-Z0-9_]+_JS)#(\d+) \*\/\n/g;
export const FRAG_OPEN_RE = /\/\* >>> ([A-Za-z0-9_]+Js\.h)#(DEADLOCK_[A-Z0-9_]+_JS) \*\/\n/g;
// No leading \n? on purpose. The trailer is appended straight after the final literal, so the
// newline in front of it is that literal's OWN last character -- eating it would hand the applier a
// body one newline shorter than the header's and the round trip would never close.
export const TRAILER_RE = /\/\/# sourceURL=mvm-deadlock\/[A-Za-z0-9_-]+\.js\n?$/;

export function segMarker(file, macro, n) {
  return `/* @mvm:seg ${file}#${macro}#${n} */\n`;
}

export function fragMarker(file, macro) {
  return `/* >>> ${file}#${macro} */\n`;
}

export function newlineOf(text) {
  return text.includes('\r\n') ? '\r\n' : '\n';
}

// One macro's JS, exactly as RunScript receives it, with a marker before each literal. Bodies are
// normalised to LF so the generated file has one convention no matter which headers it came from;
// the applier restores each file's own on the way back.
export function flattenMacro(panoDir, file, macroName) {
  const src = fs.readFileSync(path.join(panoDir, file), 'utf8');
  const macro = tokenize(src, file).find((x) => x.name === macroName);
  if (!macro) throw new Error(`${file}: no macro ${macroName}`);
  const out = [];
  macro.literals.forEach((lit, n) => {
    out.push(segMarker(file, macroName, n));
    out.push(src.slice(lit.bodyStart, lit.bodyEnd).replace(/\r\n/g, '\n'));
  });
  return out.join('');
}

// Split generated JS back into { 'File.h': { MACRO: [body0, body1, ...] } }. Everything between one
// marker and the next marker of any kind is that literal's body, verbatim.
export function unflatten(js) {
  const text = js.replace(TRAILER_RE, '');
  const marks = [];
  for (const re of [SEG_RE, FRAG_OPEN_RE]) {
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(text))) marks.push({ idx: m.index, len: m[0].length, re, m });
  }
  marks.sort((a, b) => a.idx - b.idx);
  const bodies = {};
  for (let k = 0; k < marks.length; k++) {
    const mk = marks[k];
    if (mk.re !== SEG_RE) continue;
    const [, file, macro, n] = mk.m;
    const start = mk.idx + mk.len;
    const end = k + 1 < marks.length ? marks[k + 1].idx : text.length;
    ((bodies[file] ||= {})[macro] ||= [])[Number(n)] = text.slice(start, end);
  }
  return bodies;
}
