"""Export labeled smoke sprite candidates for picking vistasmoke replacements.

Writes Automation/output/smoke-picker-preview/ with PNG sheets + sample frames
per candidate, plus INDEX.txt.

Usage (from repo root):
    python Automation/tools/export-smoke-picker-preview.py
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUTPUT = REPO / "Automation/output/smoke-picker-preview"

_sibling = Path(__file__).resolve().parent / "export-usp-fx-textures.py"
_spec = importlib.util.spec_from_file_location("export_usp_fx_textures", _sibling)
ex = importlib.util.module_from_spec(_spec)
assert _spec and _spec.loader
_spec.loader.exec_module(ex)

CANDIDATES: list[dict[str, str]] = [
    {
        "id": "01_vistasmokev1",
        "label": "vistasmokev1 — CURRENT (molotov only after fix)",
        "vtex": "materials/particle/vistasmokev1/vistasmokev1.vtex",
        "used_today": "Muzzle wisps, shell smoke, HE explosions (being removed)",
        "pick_for": "Do not pick — molotov reference only",
    },
    {
        "id": "02_vistasmokev1_emods",
        "label": "vistasmokev1_emods — emods variant",
        "vtex": "materials/particle/vistasmokev1/vistasmokev1_emods.vtex",
        "used_today": "Shell-eject smoke, molotov ground child",
        "pick_for": "Do not pick — molotov reference only",
    },
    {
        "id": "03_insandstorm_thinsmoke_05",
        "label": "insandstorm_t_thinsmoke_05_bc — AC thin smoke",
        "vtex": "materials/particle/ac/insandstorm_t_thinsmoke_05_bc.vtex",
        "used_today": "Wall impact smoke (impact_concrete_child_smoke, copyka228)",
        "pick_for": "Muzzle wisps / shell / HE / impacts",
    },
    {
        "id": "04_insandstorm_thinsmoke_01",
        "label": "insandstorm_t_thinsmoke_01_bc — AC thin smoke alt",
        "vtex": "materials/particle/ac/insandstorm_t_thinsmoke_01_bc.vtex",
        "used_today": "Other AC impact/explosion systems in mod",
        "pick_for": "Alternative thin smoke",
    },
    {
        "id": "05_insandstorm_thinsmoke_06",
        "label": "insandstorm_t_thinsmoke_06_bc — AC thin smoke alt",
        "vtex": "materials/particle/ac/insandstorm_t_thinsmoke_06_bc.vtex",
        "used_today": "AC mod smoke library",
        "pick_for": "Alternative thin smoke",
    },
    {
        "id": "06_insandstorm_thinsmoke_07",
        "label": "insandstorm_t_thinsmoke_07_bc — AC thin smoke alt",
        "vtex": "materials/particle/ac/insandstorm_t_thinsmoke_07_bc.vtex",
        "used_today": "AC mod smoke library",
        "pick_for": "Alternative thin smoke",
    },
    {
        "id": "07_smoke1",
        "label": "smoke1 — pack neutral smoke",
        "vtex": "materials/particle/smoke1/smoke1.vtex",
        "used_today": "weapon_muzzle_flash_smoke_small3 (AWP, para, hunting rifle)",
        "pick_for": "Muzzle wisps (vanilla-style)",
    },
    {
        "id": "08_sq_fulldustfront1_2",
        "label": "sq_fulldustfront1_2 — AC dust flipbook",
        "vtex": "materials/particle/ac/sq_fulldustfront1_2.vtex",
        "used_today": "ac_muzzle_pistol_smoke_b (USP main smoke stack)",
        "pick_for": "Muzzle / shell puff",
    },
    {
        "id": "09_wd_gfx_steam0264",
        "label": "wd_gfx_steam0264_d_high — AC steam flipbook",
        "vtex": "materials/particle/ac/wd_gfx_steam0264_d_high.vtex",
        "used_today": "ac_muzzle_pistol_smoke_c; warm RGB baked in",
        "pick_for": "Heavier muzzle smoke (watch for warm tint)",
    },
    {
        "id": "10_beam_smoke_01",
        "label": "beam_smoke_01 — rope/smear smoke",
        "vtex": "materials/particle/beam_smoke_01.vtex",
        "used_today": "weapon_muzzle_smoke_long sustained spray",
        "pick_for": "Lingering plumes / sustained fire",
    },
    {
        "id": "11_fas_smoke_trail",
        "label": "fas_smoke_trail — gray trail wisps",
        "vtex": "materials/effects/fas_smoke_trail.vtex",
        "used_today": "Tracer trails, ac_muzzle_smg_trail",
        "pick_for": "Light trails / wisps",
    },
    {
        "id": "12_fas_smoke_beam",
        "label": "fas_smoke_beam — beam smoke streak",
        "vtex": "materials/effects/fas_smoke_beam.vtex",
        "used_today": "Shotgun alt barrel smoke trail",
        "pick_for": "Sustained / beam-style smoke",
    },
    {
        "id": "13_smokesprites0463",
        "label": "smokesprites0463 — MW2019 smoke sprite",
        "vtex": "materials/particle/smokesprites0463.vtex",
        "used_today": "Modern pack smoke library",
        "pick_for": "Modern-style replacement",
    },
    {
        "id": "14_anim_vapor",
        "label": "anim_vapor — MW2019 vapor sheet",
        "vtex": "materials/particle/anim_vapor.vtex",
        "used_today": "Modern pack vapor effects",
        "pick_for": "Soft vapor / light smoke",
    },
    {
        "id": "15_ins_smoketrail",
        "label": "ins_smoketrail — MW2019 smoke trail",
        "vtex": "materials/particle/ins_smoketrail.vtex",
        "used_today": "Modern pack trail smoke",
        "pick_for": "Trail / light smoke",
    },
    {
        "id": "16_trails_smoke",
        "label": "trails/smoke — generic trail smoke",
        "vtex": "materials/trails/smoke.vtex",
        "used_today": "GMod/MW2019 trail library",
        "pick_for": "Trail wisps",
    },
    {
        "id": "17_fas_dust_a",
        "label": "fas_dust_a — MW2019 dust puff",
        "vtex": "materials/effects/fas_dust_a.vtex",
        "used_today": "Modern dust/impact library",
        "pick_for": "Dust puff / light debris smoke",
    },
    {
        "id": "18_fas_heavy_smoke_spike",
        "label": "fas_heavy_smoke_spike — MW2019 heavy smoke",
        "vtex": "materials/effects/fas_heavy_smoke_spike.vtex",
        "used_today": "Modern heavy smoke library",
        "pick_for": "HE explosion / thick plume",
    },
]


def main() -> int:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    manifest = {"outputDir": str(OUTPUT.relative_to(REPO)), "candidates": []}
    lines = [
        "Smoke picker preview — open *_sheet.png for full flipbook, *_frame_00.png for one frame",
        "",
    ]

    missing = 0
    for entry in CANDIDATES:
        cid = entry["id"]
        vtex = entry["vtex"]
        result = ex.export_texture(vtex, OUTPUT)

        renamed: list[str] = []
        for png_rel in result.get("pngs", []):
            src = REPO / png_rel
            if not src.is_file():
                continue
            if ".vtex" in src.name:
                suffix = src.name.split(".vtex", 1)[1]
            else:
                suffix = "_" + src.name
            dest = OUTPUT / f"{cid}{suffix}"
            if dest.resolve() != src.resolve():
                if dest.exists():
                    dest.unlink()
                src.replace(dest)
            renamed.append(str(dest.relative_to(REPO)))

        row = {**entry, "vtf": result.get("vtf"), "pngs": renamed, "error": result.get("error")}
        manifest["candidates"].append(row)

        sheet = next(
            (p for p in renamed if "_sheet" in p or (p.endswith(".png") and "_frame_" not in p)),
            None,
        )
        if result.get("error"):
            missing += 1
            lines.append(f"[MISSING] {cid}: {entry['label']}")
            lines.append(f"  vtex: {vtex}")
            lines.append(f"  error: {result['error']}")
        else:
            lines.append(f"{cid}: {entry['label']}")
            lines.append(f"  sheet: {sheet}")
            lines.append(f"  used today: {entry['used_today']}")
            lines.append(f"  good for: {entry['pick_for']}")
        lines.append("")

    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    (OUTPUT / "INDEX.txt").write_text("\n".join(lines), encoding="utf-8")

    ok = sum(1 for c in manifest["candidates"] if c.get("pngs"))
    print(f"Exported {ok}/{len(CANDIDATES)} candidates to {OUTPUT.relative_to(REPO)}")
    if missing:
        print(f"Missing: {missing} — see INDEX.txt", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
