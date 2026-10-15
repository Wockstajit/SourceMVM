#!/usr/bin/env python3
"""VRF-assisted CS2 cosmetics research/extraction tool (OFFLINE helper, not a runtime dep).

Maps where every cosmetic data point lives inside the CS2 game files and writes a research
report + a sample catalog + a found-paths list + a missing/uncertain list under
  Automation/output/vrf_cosmetics_research/   (git-ignored).

This deliberately does NOT make ValveResourceFormat (VRF) a dependency of the filmmaker tool:
  * At runtime the tool consumes the generated catalog (CameraEditorCosmeticsCatalog.inc) and
    references CS2's own inventory icons by s2r:// resource path -- nothing is extracted.
  * VRF's Source2Viewer-CLI.exe is used here only as an offline helper to LIST the VPK and (if
    you ever need PNGs outside the game) to decompile .vtex_c -> .png. The PNG command is
    DOCUMENTED below and in the report; this script does not mass-extract.

Data is parsed straight from pak01_dir.vpk using the in-repo VPK reader from
generate_cosmetics_catalog.py (so this script needs no VRF assemblies, only the `vdf` pip pkg).

Usage:
  python Automation/tools/vrf_cosmetics_research.py
  python Automation/tools/vrf_cosmetics_research.py --cs2-root "D:\\...\\Counter-Strike Global Offensive"
"""
import sys
import os
import argparse
import importlib.util
import json
import re
from datetime import datetime, timezone
from pathlib import Path

# Where CS2 actually is on this machine. See Automation/lib/steam_paths.py -- these defaults
# used to be a hand-written list of drive letters headed by the author's own Steam library.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_game_dirs, cs2_install_dirs  # noqa: E402

HERE = Path(__file__).resolve().parent

# Reuse the existing generator's VPK reader + vdf + Catalog (DRY; no second KeyValues parser).
_spec = importlib.util.spec_from_file_location("gencat", HERE / "generate_cosmetics_catalog.py")
gencat = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gencat)
vdf = gencat.vdf
Vpk = gencat.Vpk

_INSTALLS = cs2_install_dirs()
DEFAULT_CS2 = str(_INSTALLS[0]) if _INSTALLS else ""
# Source2Viewer-CLI (ValveResourceFormat). Not a build dependency -- only the asset
# conversion/research tools need it. Resolved as a checkout sitting NEXT TO this repository,
# which is the documented layout; set SOURCEMVM_VRF_CLI to point elsewhere.
_VRF_SIBLING = (
    Path(__file__).resolve().parents[2].parent
    / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe"
)
DEFAULT_VRF = os.environ.get("SOURCEMVM_VRF_CLI") or str(_VRF_SIBLING)
DEFAULT_OUT = HERE.parent / "output" / "vrf_cosmetics_research"

IMG = "panorama/images/"        # all econ icons live under this in the VPK
DG = IMG + "econ/default_generated/"   # pre-generated weapon/knife/glove finish icons


def strip_c(key):
    """VPK keys are compiled (`*.vmdl_c`); the in-game/resource path drops the trailing `_c`."""
    return key[:-2] if key.endswith("_c") else key


def samples(keys, *needles, suffix="", n=6):
    out = []
    for k in keys:
        if suffix and not k.endswith(suffix):
            continue
        if all(nd in k for nd in needles):
            out.append(k)
            if len(out) >= n:
                break
    return out


def first(keys, *needles, suffix=""):
    s = samples(keys, *needles, suffix=suffix, n=1)
    return s[0] if s else None


def build():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cs2-root", default=DEFAULT_CS2)
    ap.add_argument("--vrf-cli", default=DEFAULT_VRF)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    root = Path(args.cs2_root)
    vpk_path = root / "game" / "csgo" / "pak01_dir.vpk"
    vpk = Vpk(vpk_path)
    keys = sorted(vpk.entries)

    items_text = vpk.read("scripts/items/items_game.txt").decode("utf-8", "replace")
    english_text = vpk.read("resource/csgo_english.txt").decode("utf-8", "replace")
    ig = vdf.loads(items_text, mapper=vdf.VDFDict)["items_game"]
    tokens = vdf.loads(english_text, mapper=vdf.VDFDict)["lang"]["Tokens"]
    token_table = {str(k).lower(): v for k, v in tokens.items()}

    def loc(value):
        if not value:
            return ""
        k = value[1:] if value.startswith("#") else value
        return token_table.get(k.lower(), value)

    cat = gencat.Catalog(ig, loc)

    # ---- categorize VPK paths ----
    dg_icons = [k for k in keys if k.startswith(DG) and k.endswith("_png.vtex_c")]
    base_weapon_icons = [k for k in keys if k.startswith(IMG + "econ/weapons/base_weapons/") and k.endswith("_png.vtex_c")]
    agent_portraits = [k for k in keys if k.startswith(IMG + "econ/characters/customplayer_") and k.endswith("_png.vtex_c")]
    weapon_models = [k for k in keys if k.startswith("weapons/models/") and k.endswith(".vmdl_c") and "/knife/" not in k]
    knife_models = [k for k in keys if k.startswith("weapons/models/knife/") and k.endswith(".vmdl_c")]
    arm_glove_models = [k for k in keys if k.startswith("agents/models/shared/arms/glove_") and k.endswith(".vmdl_c")]
    agent_models = [k for k in keys if k.startswith("agents/models/") and ("/ctm_" in k or "/tm_" in k) and k.endswith(".vmdl_c")]

    # glove econ names (def -> items_game name) for the "where are glove icons" answer
    glove_econ = {}
    for didx, data in ig.get("items", {}).items():
        if isinstance(data, dict) and data.get("prefab") in ("hands", "hands_paintable") and str(didx).isdigit():
            glove_econ[int(didx)] = data.get("name", "")

    # agents + variants (mirror generator: VPK model is source of truth, econ item enriches)
    cp_items = {}
    for didx, data in ig.get("items", {}).items():
        if not isinstance(data, dict) or data.get("prefab") != "customplayer":
            continue
        nm = data.get("name", "")
        if not nm.startswith("customplayer_"):
            continue
        classes = data.get("used_by_classes") or {}
        team = "CT" if "counter-terrorists" in classes else ("T" if "terrorists" in classes else "")
        cp_items[nm] = {"def": int(didx) if str(didx).isdigit() else 0, "team": team}

    facts = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "cs2_root": str(root),
        "vpk_path": str(vpk_path),
        "vrf_cli": args.vrf_cli,
        "vrf_cli_exists": Path(args.vrf_cli).exists(),
        "total_entries": len(keys),
        "paint_kits": len(cat.paint),
        "dg_icons": len(dg_icons),
        "base_weapon_icons": len(base_weapon_icons),
        "agent_portraits": len(agent_portraits),
        "weapon_models": len(weapon_models),
        "knife_models": len(knife_models),
        "arm_glove_models": len(arm_glove_models),
        "agent_models": len(agent_models),
        "glove_econ": glove_econ,
    }

    # ---------------- found_paths.txt ----------------
    fp = []
    fp.append("# Concrete CS2 cosmetic data paths (inside pak01_dir.vpk unless noted)")
    fp.append(f"# generated {facts['generated']}  |  VPK: {vpk_path}")
    fp.append(f"# total VPK entries: {len(keys)}")
    fp.append("")
    fp.append("## Data files (KeyValues text)")
    fp.append("scripts/items/items_game.txt        # econ defs: paint kits, rarity, wear, weapon/knife/glove/agent items")
    fp.append("resource/csgo_english.txt           # display names (description_tag -> 'Redline', etc.)")
    fp.append("")
    fp.append(f"## Weapon/knife/glove FINISH icons  ({len(dg_icons)} files)  -> {DG}")
    fp += ["  " + strip_c(k) for k in samples(dg_icons, "_ak47_", n=3)]
    fp += ["  " + strip_c(k) for k in samples(dg_icons, "studded_bloodhound_gloves_", n=2)]
    fp += ["  " + strip_c(k) for k in samples(dg_icons, "weapon_knife_karam", n=2)]
    fp.append("")
    fp.append(f"## Base (vanilla) item icons  ({len(base_weapon_icons)} files)  -> {IMG}econ/weapons/base_weapons/")
    fp += ["  " + strip_c(k) for k in samples(base_weapon_icons, "weapon_", n=4)]
    fp.append("")
    fp.append(f"## Agent portraits  ({len(agent_portraits)} files)  -> {IMG}econ/characters/")
    fp += ["  " + strip_c(k) for k in samples(agent_portraits, n=4)]
    fp.append("")
    fp.append(f"## Weapon models  ({len(weapon_models)} files)")
    fp += ["  " + strip_c(k) for k in samples(weapon_models, "ak47", n=2) + samples(weapon_models, "awp", n=1)]
    fp.append(f"## Knife models  ({len(knife_models)} files)  -> weapons/models/knife/")
    fp += ["  " + strip_c(k) for k in samples(knife_models, n=4)]
    fp.append(f"## Glove + viewmodel-arm models  ({len(arm_glove_models)} files)  -> agents/models/shared/arms/")
    fp += ["  " + strip_c(k) for k in samples(arm_glove_models, n=6)]
    fp.append(f"## Agent models + variants  ({len(agent_models)} files)  -> agents/models/(ctm|tm)_*/")
    fp += ["  " + strip_c(k) for k in samples(agent_models, "ctm_fbi", n=4) + samples(agent_models, "tm_phoenix", n=1)]
    found_paths = "\n".join(fp) + "\n"

    # ---------------- cosmetics_catalog_sample.json ----------------
    def dg_rel(econ, pid):
        """Catalog-style image ref (no _png.vtex suffix) for the first wear tier that exists."""
        nm = cat.paint[pid]["name"]
        for tier in ("light", "medium", "heavy"):
            rel = f"econ/default_generated/{econ}_{nm}_{tier}"
            if (IMG + rel + "_png.vtex_c") in vpk.entries:
                return rel
        return ""

    def skin_entry(weapon_econ, pid):
        rec = cat.paint[pid]
        return {
            "name": None, "paintKit": pid, "rarity": rec["rarity"],
            "image": dg_rel(weapon_econ, pid),
            "minWear": round(rec["wmin"], 2), "maxWear": round(rec["wmax"], 2),
        }

    def finishes_from_icons(econ, label_prefix, n=2):
        """Icon-driven sample finishes for a knife/glove econ base (dedup by paint kit)."""
        out, seen = [], set()
        base = DG + econ + "_"
        for k in dg_icons:
            if not k.startswith(base):
                continue
            mid = k[len(base):-len("_png.vtex_c")]
            for suf in ("_light", "_medium", "_heavy"):
                if mid.endswith(suf):
                    mid = mid[:-len(suf)]
                    break
            pid = cat.name2id.get(mid)
            if pid is None or pid in seen:
                continue
            seen.add(pid)
            rec = cat.paint[pid]
            out.append({"name": f"{label_prefix} | {cat.finish_name(pid)}", "paintKit": pid,
                        "rarity": rec["rarity"], "image": dg_rel(econ, pid),
                        "minWear": round(rec["wmin"], 2), "maxWear": round(rec["wmax"], 2)})
            if len(out) >= n:
                break
        return out

    # AK-47 sample (def 7, econ weapon_ak47)
    ak_pids = sorted(cat.name2id[p] for p in cat.weapon_combos.get("weapon_ak47", ()) if p in cat.name2id)[:3]
    ak_skins = []
    for pid in ak_pids:
        e = skin_entry("weapon_ak47", pid)
        e["name"] = f"AK-47 | {cat.finish_name(pid)}"
        ak_skins.append(e)

    # Karambit sample (def 507, econ weapon_knife_karambit) -- icon-driven so images resolve.
    karam_skins = finishes_from_icons("weapon_knife_karambit", "Karambit", n=2)

    # Sport Gloves sample (def 5030, econ sporty_gloves) -- icon-driven.
    sport_econ = glove_econ.get(5030, "sporty_gloves")
    sport_finishes = finishes_from_icons(sport_econ, "Sport Gloves", n=2)

    fbi_variants = [strip_c(k) for k in samples(agent_models, "ctm_fbi", n=5)]
    sample = {
        "_note": ("Representative slice produced by vrf_cosmetics_research.py. The FULL catalog is "
                  "generated by generate_cosmetics_catalog.py -> CameraEditorCosmeticsCatalog.inc + "
                  "Data/cosmetics.json. Image paths are relative to panorama/images/ and resolve in "
                  "Panorama as s2r://panorama/images/<image>_png.vtex."),
        "rarity": {"0": "default/stock", "1": "common", "2": "uncommon", "3": "rare",
                   "4": "mythical", "5": "legendary", "6": "ancient/contraband", "7": "immortal"},
        "agents": [
            {"name": gencat.agent_label(m), "team": "CT",
             "defIndex": cp_items.get("customplayer_" + Path(m).stem, {}).get("def", 0),
             "variantId": Path(m).stem,
             "model": m,
             "image": "econ/characters/customplayer_" + Path(m).stem}
            for m in fbi_variants
        ] + [
            {"name": gencat.agent_label(m), "team": "T",
             "defIndex": cp_items.get("customplayer_" + Path(m).stem, {}).get("def", 0),
             "variantId": Path(m).stem, "model": strip_c(m),
             "image": "econ/characters/customplayer_" + Path(m).stem}
            for m in samples(agent_models, "tm_phoenix", n=1)
        ],
        "weapons": [
            {"name": "AK-47", "defIndex": 7, "slot": "primary",
             "model": first(weapon_models, "ak47", suffix=".vmdl_c") and strip_c(first(weapon_models, "ak47", suffix=".vmdl_c")),
             "baseImage": "econ/weapons/base_weapons/weapon_ak47", "skins": ak_skins},
        ],
        "knives": [
            {"name": "Karambit", "defIndex": 507,
             "model": first(knife_models, "karambit", suffix=".vmdl_c") and strip_c(first(knife_models, "karambit", suffix=".vmdl_c")),
             "skins": karam_skins},
        ],
        "gloves": [
            {"name": "Sport Gloves", "defIndex": 5030, "econName": sport_econ,
             "model": first(arm_glove_models, "glove_sporty", suffix=".vmdl_c") and strip_c(first(arm_glove_models, "glove_sporty", suffix=".vmdl_c")),
             "finishes": sport_finishes},
        ],
    }

    # ---------------- missing_or_uncertain_items.md ----------------
    # glove paints (id>=10000) that have no default_generated icon under any known glove econ base
    glove_bases = [glove_econ.get(d, "") for d in (4725, 5027, 5030, 5031, 5032, 5033, 5034, 5035)]
    iconed_glove_pids = set()
    for k in dg_icons:
        for eb in glove_bases:
            if eb and k.startswith(DG + eb + "_"):
                mid = k[len(DG + eb + "_"):-len("_png.vtex_c")]
                for suf in ("_light", "_medium", "_heavy"):
                    if mid.endswith(suf):
                        mid = mid[:-len(suf)]
                        break
                pid = cat.name2id.get(mid)
                if pid is not None:
                    iconed_glove_pids.add(pid)
    all_glove_pids = {pid for pid, rec in cat.paint.items() if pid >= 10000}
    glove_no_icon = sorted(all_glove_pids - iconed_glove_pids)

    agents_no_def = [strip_c(m) for m in agent_models
                     if cp_items.get("customplayer_" + Path(m).stem, {}).get("def", 0) == 0][:20]

    mu = []
    mu.append("# Missing / uncertain cosmetic items")
    mu.append(f"_generated {facts['generated']}_\n")
    mu.append("## Glove paint kits (id >= 10000) with no pre-generated icon under a known glove base")
    mu.append(f"Count: **{len(glove_no_icon)}** of {len(all_glove_pids)} glove paint kits.")
    mu.append("These are typically community/unreleased or non-wearable paint kits; the icon-driven "
              "glove catalog correctly omits them (no blank rows).")
    if glove_no_icon:
        mu.append("")
        mu.append("| paintKit | internal name |")
        mu.append("|---|---|")
        for pid in glove_no_icon[:40]:
            mu.append(f"| {pid} | `{cat.paint[pid]['name']}` |")
        if len(glove_no_icon) > 40:
            mu.append(f"| … | (+{len(glove_no_icon) - 40} more) |")
    mu.append("")
    mu.append("## Agent models with no econ item def in items_game")
    mu.append(f"Count: **{len(agents_no_def)}** (shown up to 20). These still get a portrait by "
              "s2r:// resource path, but have no econ def to drive an inventory-based command.")
    for m in agents_no_def:
        mu.append(f"- `{m}`")
    mu.append("")
    mu.append("## Notes")
    mu.append("- Default knife base icons (`weapon_knife`, `weapon_knife_t`) and default team-glove "
              "icons (`ct_gloves`, `t_gloves`) are present under econ/weapons/base_weapons/.")
    mu.append("- Knife FINISH icons use the same default_generated convention as weapons, keyed by "
              "each knife's econ name.")
    missing = "\n".join(mu) + "\n"

    # ---------------- cosmetics_research_report.md ----------------
    report = REPORT_TEMPLATE.format(
        gen=facts["generated"], cs2=facts["cs2_root"], vpk=facts["vpk_path"],
        vrf=facts["vrf_cli"], vrf_ok=("found" if facts["vrf_cli_exists"] else "NOT FOUND"),
        total=facts["total_entries"], paints=facts["paint_kits"], dg=facts["dg_icons"],
        basew=facts["base_weapon_icons"], portraits=facts["agent_portraits"],
        wmodels=facts["weapon_models"], kmodels=facts["knife_models"],
        gmodels=facts["arm_glove_models"], amodels=facts["agent_models"],
        glove_econ=json.dumps({str(k): glove_econ[k] for k in sorted(glove_econ)}, indent=2),
    )

    return args, found_paths, sample, missing, report


REPORT_TEMPLATE = """# CS2 Cosmetics — VRF research report

_Generated {gen} by `Automation/tools/vrf_cosmetics_research.py`._

- **CS2 root:** `{cs2}`
- **VPK:** `{vpk}` ({total} entries)
- **VRF CLI:** `{vrf}` ({vrf_ok}) — offline helper only, **not** a runtime/build dependency.

## TL;DR
Everything needed for live cosmetic changes is in **`pak01_dir.vpk`**: two KeyValues text files
for the data and the `panorama/images/econ/**` textures for thumbnails. **Panorama can reference
those textures directly via `s2r://`** (proven — the Customize dropdowns already render game icons),
so no PNG extraction is required at runtime. The catalog is built **offline** by
`generate_cosmetics_catalog.py`; the hook consumes the generated `.inc` + `s2r://` refs and uses
native econ panels for the 3D preview.

## Where each thing is defined

| Question | Answer (path inside pak01_dir.vpk) |
|---|---|
| Weapon skins | `scripts/items/items_game.txt` → `paint_kits` + loot lists (`client_loot_lists`/`revolving_loot_lists`, `[paint]weapon_x`). |
| Paint kit IDs | `items_game.txt` → `paint_kits` (numeric key = paint kit id). {paints} paint kits. |
| Rarity values | `items_game.txt` → `paint_kits_rarity` (tag substring → tier 0–7). |
| Item definition indexes | `items_game.txt` → `items` (numeric key = def index; e.g. AK-47 = 7, Karambit = 507). |
| Display names | `resource/csgo_english.txt` (`description_tag` → "Redline", etc.). |
| Weapon thumbnails | `panorama/images/econ/default_generated/<weapon_econ>_<paint>_<light\\|medium\\|heavy>_png.vtex_c` (vanilla: `panorama/images/econ/weapons/base_weapons/<weapon_econ>_png.vtex_c`). {dg} finish icons, {basew} base icons. |
| Knife thumbnails | same `default_generated` convention, keyed by the knife econ name. |
| Glove thumbnails | `panorama/images/econ/default_generated/<glove_econ>_<paint>_<light\\|medium\\|heavy>_png.vtex_c` (glove_econ below). |
| Agent thumbnails | `panorama/images/econ/characters/customplayer_<stem>_png.vtex_c`. {portraits} portraits. |
| Agent variants | model files `agents/models/(ctm\\|tm)_<name>/<name>[_variant{{a..}}].vmdl_c`. {amodels} agent models. |
| Agent model paths | `agents/models/(ctm\\|tm)_<name>/...vmdl`. |
| Glove model paths | `agents/models/shared/arms/glove_<type>/glove_<type>.vmdl`. {gmodels} arm/glove models. |
| Knife model paths | `weapons/models/knife/knife_<type>/weapon_knife_<type>.vmdl`. {kmodels} knife models. |
| Weapon model paths | `weapons/models/<weapon>/...vmdl`. {wmodels} weapon models. |
| Viewmodel arms | `agents/models/shared/arms/...` — the glove models **are** the viewmodel arms (base arms: `glove_fingerless`/`glove_fullfinger`/`glove_hardknuckle`). |

### Glove econ names (def index → items_game `name`, = icon/model base)
```json
{glove_econ}
```

## Can the game's image paths be used directly in Panorama?
**Yes.** A VPK texture `panorama/images/econ/<...>_png.vtex_c` is referenced from Panorama as:
```
s2r://panorama/images/econ/<...>_png.vtex
```
(`s2r` = Source 2 Resource; the engine adds the compiled `_c` suffix and resolves it from the
loaded VPKs). This is exactly what `CameraEditorJs.h` → `panoramaImageSrc()` does, which is why
the Customize dropdown thumbnails render with **no extraction**.

## If you ever need PNGs outside the game (web/docs)
VRF can decompile the `.vtex_c` textures to PNG. **Not needed at runtime**; documented for tooling:
```bat
:: single icon -> PNG
"{vrf}" -i "<icon>.vtex_c" -d -o out.png

:: bulk: every finish icon -> PNG folder
"{vrf}" -i "{vpk}" -f "panorama/images/econ/default_generated" -e vtex_c -d -o econ_png

:: list VPK contents (path/ext filters apply):  -l list, -f path filter, -e ext filter
"{vrf}" -i "{vpk}" -l -f "panorama/images/econ" -e vtex_c
```

## Which files are needed to build a full cosmetics catalog
1. `scripts/items/items_game.txt` — all defs, paint kits, rarity, wear.
2. `resource/csgo_english.txt` — display names.
3. `panorama/images/econ/**` — thumbnails (referenced via `s2r://`, not copied).
4. (3D preview / reference only) the model trees above — paths only; the in-game preview uses
   native econ panels, so no model export is required.

## Runtime vs offline split
- **Generated offline** (before runtime): the cosmetics catalog itself —
  `generate_cosmetics_catalog.py` → `CameraEditorCosmeticsCatalog.inc` + `Data/cosmetics.json`.
  Re-run after a CS2 update.
- **Loaded at runtime** by the hook: the generated catalog (compiled into the DLL via the `.inc`),
  CS2's inventory icons by `s2r://` path, and native Panorama econ panels for the 3D preview.
  No VRF, no extracted assets, no extra files shipped.

See `found_paths.txt` for concrete sample paths and `missing_or_uncertain_items.md` for gaps.
"""


def main():
    args, found_paths, sample, missing, report = build()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "found_paths.txt").write_text(found_paths, encoding="utf-8")
    (out / "cosmetics_catalog_sample.json").write_text(
        json.dumps(sample, ensure_ascii=False, indent=2), encoding="utf-8")
    (out / "missing_or_uncertain_items.md").write_text(missing, encoding="utf-8")
    (out / "cosmetics_research_report.md").write_text(report, encoding="utf-8")
    print(f"wrote research deliverable to {out}")
    for f in ("cosmetics_research_report.md", "cosmetics_catalog_sample.json",
              "found_paths.txt", "missing_or_uncertain_items.md"):
        print("  -", (out / f))


if __name__ == "__main__":
    main()
