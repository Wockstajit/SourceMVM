"""Generate cinematic firing profiles from VRF-decompiled installed CS2 assets.

Inputs: scripts/weapons.vdata_c decompiled as KV3 text and scripts/items/items_game.txt.
This deliberately reads weapon data rather than estimating rates from weapon categories.
"""
import argparse
import hashlib
import re
from pathlib import Path


def generate(weapons, items):
    ids = {}
    # Restrict to the items section; paint kits and other tables also have numeric keys.
    section = re.search(r'^\t"items"\s*\{(.*?)^\t\}', items, re.M | re.S)
    if not section:
        raise ValueError('items section missing')
    for match in re.finditer(r'^\t\t"(\d+)"\s*\{(.*?)^\t\t\}', section[1], re.M | re.S):
        name = re.search(r'"name"\s+"(weapon_[^"]+)"', match[2])
        if name:
            ids[name[1]] = int(match[1])
    rows = []
    for match in re.finditer(r'^\t(weapon_\w+)\s*=\s*\{(.*?)^\t\}', weapons, re.M | re.S):
        name, body = match[1], match[2]
        if name not in ids:
            continue
        kind = re.search(r'm_WeaponType\s*=\s*"WEAPONTYPE_(\w+)"', body)
        if not kind or kind[1] not in {'PISTOL', 'SUBMACHINEGUN', 'RIFLE', 'SHOTGUN', 'SNIPER_RIFLE', 'MACHINEGUN'}:
            continue
        cycle = re.search(r'm_flCycleTime\s*=\s*(?:\[\s*)?([0-9.]+)', body)
        if not cycle or not 0.01 <= float(cycle[1]) <= 10:
            raise ValueError('missing/invalid cycle: ' + name)
        rows.append((ids[name], name, cycle[1], kind[1] == 'SNIPER_RIFLE'))
    if len(rows) < 30:
        raise ValueError(f'Incomplete firearm data: {len(rows)} entries')
    result = '#pragma once\n#include <cstring>\n\nnamespace Filmmaker {\n'
    result += '// Generated from installed CS2 weapon data. Regenerate after weapon balance updates.\n'
    result += '// weapons.vdata SHA256: ' + hashlib.sha256(weapons.encode()).hexdigest() + '\n'
    result += 'struct RagdollWeaponFireProfile { unsigned item; const char* name; double cycle; bool singleShot; };\n'
    result += 'inline const RagdollWeaponFireProfile* RagdollWeaponProfile(unsigned item, const char* name) {\n'
    result += '\tstatic constexpr RagdollWeaponFireProfile profiles[] = {\n'
    for item, name, cycle, single in sorted(rows):
        result += f'\t\t{{{item},"{name}",{cycle},{str(single).lower()}}},\n'
    result += '\t};\n\tfor(const auto& p:profiles) if(item && p.item==item) return &p;\n'
    result += '\tfor(const auto& p:profiles) if(name && !std::strcmp(name,p.name)) return &p;\n'
    result += '\treturn nullptr;\n}\n}\n'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('weapons', type=Path)
    parser.add_argument('items', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(generate(args.weapons.read_text(encoding='utf-8'), args.items.read_text(encoding='utf-8')), encoding='utf-8')
