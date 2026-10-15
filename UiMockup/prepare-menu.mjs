// Copy maintained native layout/style references into the preview's served directory.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
export function prepareMenu(outDir = path.join(HERE, 'generated')) {
  const ref = path.resolve(HERE, '../reference/cs2 refs');
  const layouts = {}, styles = {};
  for (const name of ['matchtiles/player', 'matchinfo', 'mainmenu_play', 'popups/popup_workshop_mode_select'])
    layouts[name] = fs.readFileSync(path.join(ref, 'layout', name + '.xml'), 'utf8');
  for (const name of ['csgostyles', 'mainmenu_watch', 'matchtile', 'matchinfo_scoreboard', 'matchinfo'])
    styles[name] = fs.readFileSync(path.join(ref, 'styles', name + '.css'), 'utf8');
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, 'native-menu.json'), JSON.stringify({ layouts, styles }));
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) prepareMenu();
