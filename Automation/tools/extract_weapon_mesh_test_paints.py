#!/usr/bin/env python3
"""Build Automation/config/weapon_mesh_test_paints.json from items_game + cosmetics.json.

For each weapon def index we care about, picks weapon-valid finishes where:
  - legacyPaint: use_legacy_model=1 (pre-CS2 UV / body_legacy)
  - modernPaint: use_legacy_model=0 (CS2 body_hd)
  - skinTestPaint: vivid modern finish for generic skin-apply checks
  - meshTogglePaint: same as legacyPaint (toggle mesh modern|legacy on ONE paint)

Run: python Automation/tools/extract_weapon_mesh_test_paints.py
"""
import sys
import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path

import vdf

# Where CS2 actually is on this machine; see Automation/lib/steam_paths.py.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_install_dirs  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
COSMETICS_JSON = ROOT / "AfxHookSource2" / "Filmmaker" / "Data" / "cosmetics.json"
OUT = HERE.parent / "config" / "weapon_mesh_test_paints.json"

# def index -> preferred legacy / modern / skin-test display-name substrings (first match wins)
WEAPON_PREFS = {
    7: {
        "legacy": ["Hydroponic", "Redline", "Asiimov"],
        "modern": ["Crane Flight", "Head Shot"],
        "skinTest": ["Redline", "Asiimov"],
    },
    34: {
        "legacy": ["Arctic Tri-Tone", "Hot Rod"],
        "modern": ["Bee-Tron", "Broken Record"],
        "skinTest": ["Hot Rod", "Arctic Tri-Tone"],
    },
    61: {
        "legacy": ["Cyrex", "Printstream", "Kill Confirmed"],
        "modern": ["Jawbreaker", "PC-GRN", "Desert Tactical"],
        "skinTest": ["Jawbreaker", "Neo-Noir", "Kill Confirmed"],
    },
}


def load_gencat():
    spec = importlib.util.spec_from_file_location("gencat", HERE / "generate_cosmetics_catalog.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def legacy_flags_from_items_game(items_text: str) -> dict[int, bool]:
    """Paint kits with use_legacy_model=1 only; absent field means modern (CS2 body_hd)."""
    ig = vdf.loads(items_text, mapper=vdf.VDFDict)["items_game"]
    out = {}
    for key, data in ig.get("paint_kits", {}).items():
        if not str(key).isdigit() or not isinstance(data, dict):
            continue
        raw = data.get("use_legacy_model")
        if raw is not None and str(raw) != "0":
            out[int(key)] = True
    return out


def paint_is_legacy(paint_id: int, legacy_flags: dict[int, bool]) -> bool:
    return legacy_flags.get(int(paint_id), False)


def pick_by_name(skins, legacy_flags, want_legacy, prefs):
    for needle in prefs:
        for s in skins:
            pid = int(s["paintKit"])
            leg = paint_is_legacy(pid, legacy_flags)
            if leg != want_legacy:
                continue
            if needle.lower() in s["name"].lower():
                return s, pid, leg
    for s in skins:
        pid = int(s["paintKit"])
        leg = paint_is_legacy(pid, legacy_flags)
        if leg == want_legacy:
            return s, pid, leg
    return None, None, None


def pick_skin_test(skins, prefs):
    """Vivid finish for skin-apply checks; legacy vs modern does not matter."""
    for needle in prefs:
        for s in skins:
            if needle.lower() in s["name"].lower():
                return s, int(s["paintKit"])
    if skins:
        return skins[0], int(skins[0]["paintKit"])
    return None, None


def main():
    gencat = load_gencat()
    installs = cs2_install_dirs()
    if not installs:
        raise SystemExit("Counter-Strike 2 not found. Set SOURCEMVM_CS2_DIR.")
    cs2_root = installs[0]
    vpk_path = cs2_root / "game" / "csgo" / "pak01_dir.vpk"
    if not vpk_path.exists():
        raise SystemExit(f"CS2 VPK not found: {vpk_path}")
    if not COSMETICS_JSON.exists():
        raise SystemExit(f"Missing {COSMETICS_JSON} — run generate_cosmetics_catalog.py first")

    vpk = gencat.Vpk(vpk_path)
    items_text = vpk.read("scripts/items/items_game.txt").decode("utf-8", "replace")
    legacy_flags = legacy_flags_from_items_game(items_text)
    catalog = json.loads(COSMETICS_JSON.read_text(encoding="utf-8"))

    weapons_out = {}
    for w in catalog.get("weapons", []):
        def_idx = int(w["defIndex"])
        if def_idx not in WEAPON_PREFS:
            continue
        skins = w.get("skins", [])
        prefs = WEAPON_PREFS[def_idx]

        leg_skin, leg_id, _ = pick_by_name(skins, legacy_flags, True, prefs["legacy"])
        mod_skin, mod_id, _ = pick_by_name(skins, legacy_flags, False, prefs["modern"])
        test_skin, test_id = pick_skin_test(skins, prefs["skinTest"])
        if not leg_skin or not mod_skin:
            raise SystemExit(f"Could not resolve legacy+modern for def {def_idx} ({w['name']})")
        if not test_skin:
            test_skin, test_id = mod_skin, mod_id

        weapons_out[str(def_idx)] = {
            "weaponName": w["name"],
            "legacyPaint": {
                "id": leg_id,
                "name": leg_skin["name"],
                "useLegacyModel": True,
            },
            "modernPaint": {
                "id": mod_id,
                "name": mod_skin["name"],
                "useLegacyModel": False,
            },
            "skinTestPaint": {
                "id": test_id,
                "name": test_skin["name"],
            },
            "meshTogglePaint": leg_id,
            "meshToggleNote": (
                "Apply this paint, then toggle mirv_filmmaker cosmetics mesh modern|legacy. "
                "Only body_hd vs body_legacy changes; do not compare two different paints for mesh."
            ),
        }

    doc = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "cosmetics.json skins + items_game paint_kits.use_legacy_model",
        "weapons": weapons_out,
        "notes": {
            "paint38": "Paint kit 38 is Glock-18 | Fade (and shared knife Fade). Never use on USP/AK/MP9.",
            "meshTest": "legacy-vs-modern PAINTS = paint change + mesh. mesh modern|legacy on SAME paint = geometry only.",
        },
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(doc, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote {OUT}")
    for def_idx, w in weapons_out.items():
        print(
            f"  def {def_idx} ({w['weaponName']}): "
            f"legacy={w['legacyPaint']['id']} ({w['legacyPaint']['name']}) "
            f"modern={w['modernPaint']['id']} ({w['modernPaint']['name']})"
        )


if __name__ == "__main__":
    main()
