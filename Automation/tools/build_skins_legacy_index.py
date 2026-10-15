#!/usr/bin/env python3
"""Build a CS2 weapon-skin index classified as Legacy vs CS2.

Data source: the game's own scripts/items/items_game.txt + resource/csgo_english.txt
(extract these from pak01_dir.vpk with Source2Viewer-CLI before running).

Classification: a paint kit carries "use_legacy_model" "1" when it still renders on
the old CS:GO weapon model (i.e. it was NOT remastered for CS2). Those skins are
"Legacy". Everything else uses the modern CS2 model and is "CS2".

Weapon x paintkit combos are taken from the [paintkit]weapon tokens in the loot
lists (these are the actually-released skins). Output is sorted by weapon, then
skin name, as both CSV and JSON.

Usage:
  python build_skins_legacy_index.py <items_game.txt> <csgo_english.txt> <out_dir>
"""
import sys
import re
import json
import csv
from pathlib import Path


def load_localization(path):
    """csgo_english.txt 'Tokens' block -> {key.lower(): value}."""
    text = Path(path).read_text(encoding="utf-8-sig", errors="replace")
    out = {}
    # Lines look like:  "Key"   "Value"
    for m in re.finditer(r'^\s*"([^"]+)"\s+"((?:[^"\\]|\\.)*)"\s*$', text, re.MULTILINE):
        out[m.group(1).lower()] = m.group(2)
    return out


def load_paint_kits(items_text):
    """Return {paintkit_name: {'id', 'legacy', 'tag'}} from the paint_kits blocks."""
    kits = {}
    # Find every "paint_kits" { ... } region, then each numeric-id sub-block.
    for pkm in re.finditer(r'"paint_kits"\s*\{', items_text):
        start = pkm.end()
        depth = 1
        i = start
        n = len(items_text)
        while i < n and depth > 0:
            c = items_text[i]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            i += 1
        block = items_text[start:i - 1]
        # Each entry:  "123" { ... }
        for em in re.finditer(r'"(\d+)"\s*\{', block):
            es = em.end()
            d = 1
            j = es
            while j < len(block) and d > 0:
                ch = block[j]
                if ch == '{':
                    d += 1
                elif ch == '}':
                    d -= 1
                j += 1
            entry = block[es:j - 1]
            name = re.search(r'"name"\s+"([^"]+)"', entry)
            if not name:
                continue
            name = name.group(1)
            legacy = bool(re.search(r'"use_legacy_model"\s+"1"', entry))
            tag = re.search(r'"description_tag"\s+"([^"]+)"', entry)
            tag = tag.group(1) if tag else None
            # Keep first definition; later dupes (across paint_kits blocks) match.
            if name not in kits:
                kits[name] = {"id": em.group(1), "legacy": legacy, "tag": tag}
    return kits


def load_weapon_names(items_text, loc):
    """Map weapon class (weapon_ak47) -> display name (AK-47)."""
    names = {}
    for m in re.finditer(r'"item_class"\s+"(weapon_[a-z0-9_]+)"', items_text):
        cls = m.group(1)
        # item_name is typically the next non-empty line.
        tail = items_text[m.end():m.end() + 300]
        nm = re.search(r'"item_name"\s+"#([^"]+)"', tail)
        if nm:
            disp = loc.get(nm.group(1).lower())
            if disp and cls not in names:
                names[cls] = disp
    return names


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    items_path, loc_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    items_text = Path(items_path).read_text(encoding="utf-8-sig", errors="replace")
    loc = load_localization(loc_path)
    kits = load_paint_kits(items_text)
    weapon_names = load_weapon_names(items_text, loc)

    # Unique [paintkit]weapon combos = released skins.
    combos = set(re.findall(r'\[([a-z0-9_]+)\]weapon_([a-z0-9_]+)', items_text))

    # A few weapons whose display name isn't reachable via item_class scanning.
    weapon_name_fallback = {
        "weapon_usp_silencer": "USP-S",
        "weapon_m4a1_silencer": "M4A1-S",
        "weapon_cz75a": "CZ75-Auto",
        "weapon_mp5sd": "MP5-SD",
        "weapon_revolver": "R8 Revolver",
    }
    for k, v in weapon_name_fallback.items():
        weapon_names.setdefault(k, v)

    rows = []
    missing_kit = set()
    for paintkit, weap_short in sorted(combos):
        weapon_class = "weapon_" + weap_short
        kit = kits.get(paintkit)
        if kit is None:
            missing_kit.add(paintkit)
            continue
        if paintkit == "default" or paintkit == "workshop_default":
            continue
        # Skin display name from the paint kit's tag (#PaintKit_xxx_Tag -> "Asiimov").
        skin_name = None
        if kit["tag"]:
            skin_name = loc.get(kit["tag"].lstrip("#").lower())
        if not skin_name:
            skin_name = loc.get(("PaintKit_" + paintkit + "_Tag").lower())
        if not skin_name:
            skin_name = paintkit  # fall back to internal name
        weapon_disp = weapon_names.get(weapon_class, weapon_class)
        rows.append({
            "weapon": weapon_disp,
            "weapon_class": weapon_class,
            "skin_name": skin_name,
            "paint_kit": paintkit,
            "paint_kit_id": kit["id"],
            "classification": "Legacy" if kit["legacy"] else "CS2",
        })

    rows.sort(key=lambda r: (r["weapon"].lower(), r["skin_name"].lower()))

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    json_path = out / "cs2_weapon_skins_legacy_index.json"
    csv_path = out / "cs2_weapon_skins_legacy_index.csv"

    # Grouped-by-weapon JSON.
    grouped = {}
    for r in rows:
        grouped.setdefault(r["weapon"], []).append({
            "skin_name": r["skin_name"],
            "paint_kit": r["paint_kit"],
            "paint_kit_id": r["paint_kit_id"],
            "classification": r["classification"],
        })
    cs2 = sum(1 for r in rows if r["classification"] == "CS2")
    legacy = sum(1 for r in rows if r["classification"] == "Legacy")
    payload = {
        "_meta": {
            "source": "CS2 items_game.txt + csgo_english.txt",
            "classification_rule": "Legacy = paint kit has use_legacy_model 1 (old CS:GO model, not remastered for CS2); CS2 = modern CS2 model.",
            "total_skins": len(rows),
            "cs2_count": cs2,
            "legacy_count": legacy,
            "weapons": len(grouped),
        },
        "weapons": grouped,
    }
    json_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")

    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["weapon", "weapon_class", "skin_name",
                                          "paint_kit", "paint_kit_id", "classification"])
        w.writeheader()
        w.writerows(rows)

    print(f"skins: {len(rows)}  CS2: {cs2}  Legacy: {legacy}  weapons: {len(grouped)}")
    if missing_kit:
        print(f"(paintkits with token but no kit def, skipped: {len(missing_kit)})")
    print(f"wrote {json_path}")
    print(f"wrote {csv_path}")


if __name__ == "__main__":
    main()
