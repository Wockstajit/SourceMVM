// Localization catalog and language tables (Docs/panorama/localization.md).
//
// The English text written in the scripts as T('...') / TN(n, '...', '...') is the catalog. Every
// language table has to cover exactly that catalog, keep each {0} placeholder, and give plural
// entries the number of forms its language uses. These are static checks; they do not need CS2.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import {
  LANG_DIR, LANGUAGES, LOCALIZATION_DIR, REPO, collect, placeholders, readTable,
} from '../i18n/strings.mjs';

const catalog = collect();

test('every T() and TN() call has literal text the catalog can read', () => {
  const lines = catalog.problems.map((p) => `${p.file}:${p.line} ${p.text}`);
  assert.deepEqual(lines, []);
});

test('the runtime fragment can be spliced into several scripts of one file', () => {
  // Community's four scripts and the Play menu's three share a .cpp each. With an include guard
  // only the first got T(), and the rest threw "T is not defined" in game.
  const fragment = fs.readFileSync(path.join(REPO, 'AfxHookSource2', 'Filmmaker', 'Panorama', 'LocalizationJs.h'), 'utf8');
  assert.doesNotMatch(fragment, /^\s*#pragma once/m);
  assert.doesNotMatch(fragment, /^\s*#ifndef /m);
});

test('the script language list matches the C++ language list', () => {
  const header = fs.readFileSync(path.join(LOCALIZATION_DIR, 'LocalizationLanguages.h'), 'utf8');
  const inCpp = [...header.matchAll(/\{\s*"(\w+)",\s*"([^"]+)",\s*(\d)\s*\}/g)].map((m) => [m[1], m[2], +m[3]]);
  assert.deepEqual(inCpp, LANGUAGES.map((l) => [l.id, l.name, l.pluralForms]));
});

test('there is one table per language and no stray tables', () => {
  const onDisk = fs.readdirSync(LANG_DIR).filter((f) => f.endsWith('.json')).map((f) => f.slice(0, -5)).sort();
  assert.deepEqual(onDisk, LANGUAGES.map((l) => l.id).filter((id) => id !== 'english').sort());
});

for (const lang of LANGUAGES) {
  if (lang.id === 'english') continue;
  test(`${lang.id}: covers the catalog, keeps placeholders and plural forms`, () => {
    const table = readTable(lang.id);
    assert.ok(table, `missing ${lang.id}.json`);
    const problems = [];
    for (const [key, info] of catalog.keys) {
      const value = table[key];
      if (value === undefined) { problems.push(`missing: ${JSON.stringify(key)}`); continue; }
      const forms = info.plural ? value : [value];
      if (info.plural && (!Array.isArray(value) || value.length !== lang.pluralForms)) {
        problems.push(`needs ${lang.pluralForms} plural form(s): ${JSON.stringify(key)}`);
        continue;
      }
      if (!info.plural && typeof value !== 'string') { problems.push(`not text: ${JSON.stringify(key)}`); continue; }
      const sources = info.plural ? key.split(' || ') : [key];
      const want = placeholders(sources[sources.length - 1]);
      for (const form of forms) {
        if (typeof form !== 'string' || !form.trim()) { problems.push(`empty: ${JSON.stringify(key)}`); continue; }
        // A language may drop the count from one plural form ("one key"), never invent a slot.
        const got = placeholders(form);
        const extra = got.filter((p) => !want.includes(p));
        const lost = info.plural ? [] : want.filter((p) => !got.includes(p));
        if (extra.length || lost.length) problems.push(`placeholders differ: ${JSON.stringify(key)} -> ${JSON.stringify(form)}`);
        if (/^\s/.test(sources[0]) !== /^\s/.test(form) || /\s$/.test(sources[0]) !== /\s$/.test(form)) {
          problems.push(`leading/trailing space differs: ${JSON.stringify(key)}`);
        }
        if (form.includes('\n') !== key.includes('\n')) problems.push(`line breaks differ: ${JSON.stringify(key)}`);
      }
    }
    for (const key of Object.keys(table)) {
      if (!catalog.keys.has(key)) problems.push(`not in the catalog (stale): ${JSON.stringify(key)}`);
    }
    assert.deepEqual(problems.slice(0, 40), [], `${problems.length} problem(s) in ${lang.id}.json`);
  });
}

test('language tables are UTF-8 without a byte-order mark and end with a newline', () => {
  for (const f of fs.readdirSync(LANG_DIR).filter((n) => n.endsWith('.json'))) {
    const buf = fs.readFileSync(path.join(LANG_DIR, f));
    assert.notEqual(buf[0], 0xEF, `${f} starts with a byte-order mark`);
    assert.equal(buf[buf.length - 1], 0x0A, `${f} does not end with a newline`);
    assert.ok(!buf.includes(0x0D), `${f} has CRLF line endings`);
  }
  assert.ok(fs.existsSync(path.join(REPO, 'Docs', 'panorama', 'localization.md')));
});
