#!/usr/bin/env node
// Localization catalog tool (Docs/panorama/localization.md).
//
//   node Automation/i18n/catalog.mjs                 summary: catalog size, coverage per language
//   node Automation/i18n/catalog.mjs --json <file>   write the catalog (key, plural, files) as JSON
//   node Automation/i18n/catalog.mjs --missing <id>  print the keys a language table lacks, as JSON
//   node Automation/i18n/catalog.mjs --merge <id> <file>   add/replace entries from a JSON file
//   node Automation/i18n/catalog.mjs --prune         drop entries no script uses any more
//
// Tables are written with keys in catalog order (the order the scripts are scanned), so a diff of
// a table shows only the entries that changed.

import fs from 'node:fs';
import path from 'node:path';
import { LANG_DIR, LANGUAGES, collect, readTable } from './strings.mjs';

const args = process.argv.slice(2);
const { keys, problems } = collect();
const others = LANGUAGES.filter((l) => l.id !== 'english');

function writeTable(lang, table) {
  const ordered = {};
  for (const key of keys.keys()) if (table[key] !== undefined) ordered[key] = table[key];
  for (const key of Object.keys(table)) if (!(key in ordered)) ordered[key] = table[key];
  fs.mkdirSync(LANG_DIR, { recursive: true });
  fs.writeFileSync(path.join(LANG_DIR, lang + '.json'), JSON.stringify(ordered, null, 1) + '\n', 'utf8');
}

for (const p of problems) console.error(`${p.file}:${p.line} ${p.text}`);

if (args[0] === '--json') {
  const out = [...keys].map(([key, info]) => ({ key, plural: info.plural, files: [...info.files] }));
  fs.writeFileSync(args[1], JSON.stringify(out, null, 1) + '\n', 'utf8');
  console.log(`${out.length} keys -> ${args[1]}`);
} else if (args[0] === '--missing') {
  const table = readTable(args[1]) || {};
  const out = [...keys.keys()].filter((key) => table[key] === undefined);
  process.stdout.write(JSON.stringify(out, null, 1) + '\n');
} else if (args[0] === '--merge') {
  const table = readTable(args[1]) || {};
  const incoming = JSON.parse(fs.readFileSync(args[2], 'utf8'));
  let added = 0;
  for (const [key, value] of Object.entries(incoming)) {
    if (!keys.has(key)) { console.error(`skipped, not in the catalog: ${JSON.stringify(key)}`); continue; }
    table[key] = value; added++;
  }
  writeTable(args[1], table);
  console.log(`${args[1]}: merged ${added} entries`);
} else if (args[0] === '--prune') {
  for (const lang of others) {
    const table = readTable(lang.id);
    if (!table) continue;
    const kept = {};
    let dropped = 0;
    for (const [key, value] of Object.entries(table)) { if (keys.has(key)) kept[key] = value; else dropped++; }
    writeTable(lang.id, kept);
    if (dropped) console.log(`${lang.id}: dropped ${dropped}`);
  }
} else {
  console.log(`${keys.size} keys (${[...keys.values()].filter((k) => k.plural).length} plural)`);
  for (const lang of others) {
    const table = readTable(lang.id) || {};
    const have = [...keys.keys()].filter((key) => table[key] !== undefined).length;
    console.log(`${lang.id.padEnd(12)} ${have}/${keys.size}`);
  }
}
process.exitCode = problems.length ? 1 : 0;
