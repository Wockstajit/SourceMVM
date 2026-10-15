# CS2 weapon skins: legacy vs CS2 index

Every released weapon-skin combination in CS2, sorted by weapon and classified as Legacy or CS2.

Files:
- [`cs2_weapon_skins_legacy_index.csv`](cs2_weapon_skins_legacy_index.csv): flat table, one row per weapon×skin.
- [`cs2_weapon_skins_legacy_index.json`](cs2_weapon_skins_legacy_index.json): the same data grouped under each weapon, with a `_meta` summary.

## What Legacy and CS2 mean here

The classification comes from the game itself. In `items_game.txt`, each paint kit may carry an optional `use_legacy_model "1"` flag:

- Legacy: the paint kit still renders on the old CS:GO weapon model. Valve did not remaster it for CS2's new model. (`use_legacy_model = 1`)
- CS2: the paint kit uses the modern CS2 weapon model. It was either made for CS2 or is a CS:GO skin remastered onto the new model. (no `use_legacy_model`)

The cosmetics renderer in this project reads the same flag to pick the mesh group (`SetMeshGroupMask`). See the `weapon-legacy-vs-cs2-mesh` notes. A skin marked "Legacy" here must render on the legacy mesh, and a "CS2" skin on the modern mesh.

> Note: a classification can look surprising. For example, AWP Dragon Lore is Legacy because Valve kept it on the old model. The label follows how the skin renders in-game, which is what matters for filmmaking.

## Columns (CSV)

| column | meaning |
|---|---|
| `weapon` | display name, e.g. `AK-47` |
| `weapon_class` | engine class, e.g. `weapon_ak47` (`weapon_m4a1` = M4A4, `weapon_m4a1_silencer` = M4A1-S) |
| `skin_name` | display name, e.g. `Asiimov` |
| `paint_kit` | internal paint-kit name, e.g. `cu_ak47_asiimov` |
| `paint_kit_id` | numeric paint-kit id used by the econ system |
| `classification` | `Legacy` or `CS2` |

## Scope

Covers all paintable weapons (35 weapon classes incl. Zeus x27). Knives, gloves, agents, stickers, and other cosmetics are out of scope. StatTrak and Souvenir are item qualities, not different skins, so each paint+weapon pair appears once.

## Regenerating

Counts (as of generation): 1404 skins: 521 CS2, 883 Legacy, across 35 weapons.

The data comes straight from the installed game files. Rebuild it in two steps:

```bash
# 1. Extract the two source files from the CS2 VPK with Source2Viewer-CLI:
Source2Viewer-CLI.exe -i ".../csgo/pak01_dir.vpk" -f "scripts/items/items_game.txt" -o <tmp>
Source2Viewer-CLI.exe -i ".../csgo/pak01_dir.vpk" -f "resource/csgo_english.txt"   -o <tmp>

# 2. Rebuild the index:
python Automation/tools/build_skins_legacy_index.py \
    <tmp>/scripts/items/items_game.txt \
    <tmp>/resource/csgo_english.txt \
    docs
```

Builder script: [`Automation/tools/build_skins_legacy_index.py`](../../Automation/tools/build_skins_legacy_index.py).
Re-run it after a CS2 update to pick up newly added skins.
