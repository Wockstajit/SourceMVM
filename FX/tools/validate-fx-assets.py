"""Prepare and validate the runtime-selected FX resource closure."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
from collections import deque
from pathlib import Path


ARRAY_RE = re.compile(
    r"const\s+VariantRule\s+kVariant(?P<category>\w+)\[\]\s*=\s*\{(?P<body>.*?)\n\};",
    re.DOTALL,
)
# Every FXRULE* form shares the first three args (match, pack, name -> the Insurgency
# target). FXRULE_L / FXRULE_MODERN_L add a Less system name; FXRULE_MODERN* add a
# modern-relative target last. FXRULE_MODERN_ONLY takes no Insurgency target at all.
RULE_RE = re.compile(
    r'FXRULE(?:_L|_MODERN_L|_MODERN)?\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"')
IMPACT_HYBRID_RE = re.compile(
    r'FXRULE_IMPACT\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\)')
# The optional 4th arg of FXRULE_L / FXRULE_MODERN_L: Insurgency's own reduced system.
LESS_RULE_RE = re.compile(
    r'FXRULE(?:_L|_MODERN_L)\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"[^"]+"\s*,\s*"([^"]+)"')
MODERN_RULE_RE = re.compile(
    r'FXRULE_MODERN\(\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"([^"]+)"\s*\)')
MODERN_L_RULE_RE = re.compile(
    r'FXRULE_MODERN_L\(\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*"([^"]+)"\s*\)')
MODERN_ONLY_RULE_RE = re.compile(
    r'FXRULE_MODERN_ONLY\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*\)')
# The per-weapon-class tracer fallback table: names systems no FXRULE row mentions.
TRACER_FALLBACK_RE = re.compile(
    r'\{\s*"[a-z]+"\s*,\s*"(weapon_tracers[a-z0-9_]*)"\s*,\s*"(mw2019_tracer[a-z_]*)"\s*\}')
# Raw table entries (non-macro) that name a modern target directly.
MODERN_PATH_RE = re.compile(r'"(particles/filmmaker/modern/[^"]+\.vpcf)"')
MONEY_RE = re.compile(r'kMoneyBurst\s*=\s*"([^"]+\.vpcf)"')
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
MODERN_MUZZLE_DIR = "particles/filmmaker/modern/arc9_fas_muzzleflashes"
COMPILED_EXTENSIONS = {".vpcf", ".vmat", ".vtex", ".vmdl", ".vsnap"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--particle-fx-cpp", type=Path, required=True)
    parser.add_argument("--content-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--file-list", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--validate-compiled", action="store_true")
    parser.add_argument("--require-modern", action="store_true",
                        help="also validate the MW2019 modern-pack targets (pass when the "
                             "GMod sources were available to the converter)")
    parser.add_argument("--emit-closure", type=Path, default=None,
                        help="write the full keep-set of content-relative source files the "
                             "runtime needs (particle closure + referenced vmat/vtex/vmdl, "
                             "expanded through model/material resource refs). The converter "
                             "uses this pre-compile to prune everything else, so only ~15%% "
                             "of the mod's assets get compiled/shipped.")
    return parser.parse_args()


def runtime_targets(cpp_path: Path, require_modern: bool) -> list[str]:
    # The ParticleFx subsystem is split across focused translation units (2026-07-04):
    # the FXRULE variant tables live in ParticleFxRules.cpp, and kMoneyBurst in
    # ParticleFxInternal.h. The converter still
    # passes ParticleFx.cpp; scan every sibling ParticleFx* source so the closure
    # covers the whole subsystem no matter which file a table lives in.
    siblings = sorted(cpp_path.parent.glob("ParticleFx*.cpp")) + sorted(
        cpp_path.parent.glob("ParticleFx*.h")
    )
    files = siblings if siblings else [cpp_path]
    text = "\n".join(f.read_text(encoding="utf-8") for f in files)
    targets: set[str] = set()
    # Use the generator's naming and surface mapping so pruning retains the actual
    # runtime wrappers and their wall-only smoke children, not just donor roots.
    layer_spec = importlib.util.spec_from_file_location(
        "impact_layers", Path(__file__).with_name("layer-vanilla-impacts.py")
    )
    layer = importlib.util.module_from_spec(layer_spec)
    layer_spec.loader.exec_module(layer)
    if "const VariantRule kVariantImpacts[] = {" in text:
        for vanilla, ins_name, pov_name in layer.impact_rows_text(text):
            surface = layer.surface_of(vanilla)
            for target in layer.candidate_targets(ins_name, pov_name):
                # The unsuffixed Insurgency candidate is defensive, never selected
                # by Impact On; its wrapper does not need to be shipped.
                if target == f"{layer.INS_DIR}/{ins_name}.vpcf":
                    continue
                targets.add(layer.layered_name(target, surface))
    for table in ARRAY_RE.finditer(text):
        body = table.group("body")
        # There is ONE Insurgency tree since the 2026-09-01 rebake; On and Less are two
        # different SYSTEM names inside it, not two converted variant folders.
        for pack, name in RULE_RE.findall(body):
            target = f"particles/filmmaker/insurgency/{pack}/{name}.vpcf"
            targets.add(target)
            if table.group("category") == "Impacts":
                targets.add(target[:-5] + "_less.vpcf")
                targets.add(target[:-5] + "_nosmoketrails.vpcf")
                targets.add(target[:-5] + "_less_nosmoketrails.vpcf")
        if table.group("category") == "Impacts":
            for insurgency_name, povarehok_name in IMPACT_HYBRID_RE.findall(body):
                insurgency = (
                    "particles/filmmaker/insurgency/impact_fx_ins/"
                    f"{insurgency_name}_less.vpcf"
                )
                targets.add(insurgency)
                targets.add(insurgency[:-5] + "_nosmoketrails.vpcf")
                # The Povarehok hybrid never gets a _nosmoketrails variant: the hook applies
                # that closure to Insurgency targets only (ParticleFxHook.cpp), because it
                # removed Povarehok's intended puff.
                targets.add(
                    "particles/filmmaker/povarehok/less/impacts/impact_fx/"
                    f"{povarehok_name}.vpcf"
                )
    # The tracer fallback classifies untabled CS2 tracer names by weapon-class substring,
    # so its targets are reachable at runtime without appearing in any FXRULE row.
    for on_name, _modern_name in TRACER_FALLBACK_RE.findall(text):
        targets.add(f"particles/filmmaker/insurgency/weapon_fx_tracers/{on_name}.vpcf")
    if require_modern:
        for rel in MODERN_RULE_RE.findall(text):
            targets.add(f"particles/filmmaker/modern/{rel}.vpcf")
        for rel in MODERN_L_RULE_RE.findall(text):
            targets.add(f"particles/filmmaker/modern/{rel}.vpcf")
        for rel in MODERN_ONLY_RULE_RE.findall(text):
            targets.add(f"particles/filmmaker/modern/{rel}.vpcf")
        for _on_name, modern_name in TRACER_FALLBACK_RE.findall(text):
            targets.add(f"particles/filmmaker/modern/mw2019_tracer/{modern_name}.vpcf")
        for path in MODERN_PATH_RE.findall(text):
            targets.add(path)
    # Authored native CS2 lights are also shipped in the FX mount.
    for path in re.findall(r'k(?:MollyLight|MollyArea|FlashLight)\s*=\s*"([^"\n]+\.vpcf)"', text):
        targets.add(path)
    money = MONEY_RE.search(text)
    if money:
        targets.add(money.group(1))
    if not targets:
        raise RuntimeError(f"No FXRULE targets found in {cpp_path}")
    return sorted(targets)


def source_path(root: Path, resource: str) -> Path:
    return root.joinpath(*resource.replace("\\", "/").split("/"))


def compiled_path(root: Path, resource: str) -> Path:
    return source_path(root, resource + "_c")


def inspect_closure(content_root: Path, targets: list[str]) -> tuple[set[str], set[str], list[str], list[str]]:
    particle_closure: set[str] = set()
    references: set[str] = set()
    missing_root_targets: list[str] = []
    missing_child_references: list[str] = []
    pending = deque((target, True) for target in targets)

    while pending:
        resource, is_root = pending.popleft()
        if resource in particle_closure:
            continue
        particle_closure.add(resource)
        path = source_path(content_root, resource)
        if not path.is_file():
            if is_root:
                missing_root_targets.append(resource)
            else:
                missing_child_references.append(resource)
            continue
        for reference in RESOURCE_RE.findall(path.read_text(encoding="utf-8")):
            normalized = reference.replace("\\", "/")
            references.add(normalized)
            if normalized.startswith("particles/filmmaker/") and normalized.endswith(".vpcf"):
                pending.append((normalized, False))
    return particle_closure, references, missing_root_targets, missing_child_references


def main() -> int:
    args = parse_args()
    content_root = args.content_root.resolve()
    game_root = args.game_root.resolve()
    targets = runtime_targets(args.particle_fx_cpp.resolve(), args.require_modern)
    closure, references, missing_root_targets, missing_child_references = inspect_closure(content_root, targets)

    local_resources: set[str] = set()
    external_resources: set[str] = set()
    legacy_models: list[str] = []
    for resource in closure | references:
        path = source_path(content_root, resource)
        if path.is_file():
            local_resources.add(resource)
            if path.suffix.lower() == ".vmdl":
                first_line = path.open("r", encoding="utf-8").readline()
                if "format:source1imported" in first_line:
                    legacy_models.append(resource)
        elif Path(resource).suffix.lower() in COMPILED_EXTENSIONS:
            external_resources.add(resource)
    external_models = sorted(
        resource for resource in external_resources
        if Path(resource).suffix.lower() == ".vmdl"
        and not compiled_path(game_root, resource).is_file()
    )

    if args.emit_closure is not None:
        # Expand the keep-set through refs the vpcf walk does not chase: converted
        # models (ModelDoc kv3) and materials carry their own resource:"..." refs
        # (a model's materials, a material's textures). Without this, pruning
        # would delete e.g. the gib models' vmats and they'd compile textureless.
        keep = set(local_resources)
        pending = deque(keep)
        while pending:
            resource = pending.popleft()
            path = source_path(content_root, resource)
            if not path.is_file() or path.suffix.lower() not in {".vmdl", ".vmat", ".vpcf"}:
                continue
            for reference in RESOURCE_RE.findall(
                    path.read_text(encoding="utf-8", errors="replace")):
                normalized = reference.replace("\\", "/")
                if normalized in keep:
                    continue
                if source_path(content_root, normalized).is_file():
                    keep.add(normalized)
                    pending.append(normalized)
        args.emit_closure.parent.mkdir(parents=True, exist_ok=True)
        args.emit_closure.write_text("\n".join(sorted(keep)) + "\n", encoding="utf-8")
        print(f"Closure keep-set: {len(keep)} content files -> {args.emit_closure}")

    missing_compiled: list[str] = []
    if args.validate_compiled:
        for resource in sorted(local_resources):
            if Path(resource).suffix.lower() not in COMPILED_EXTENSIONS:
                continue
            if not compiled_path(game_root, resource).is_file():
                missing_compiled.append(resource)

    args.file_list.parent.mkdir(parents=True, exist_ok=True)
    args.file_list.write_text(
        "\n".join(str(source_path(content_root, target)) for target in targets) + "\n",
        encoding="utf-8",
    )

    report = {
        "targets": targets,
        "targetCount": len(targets),
        "particleClosureCount": len(closure),
        "localResourceCount": len(local_resources),
        "externalResources": sorted(external_resources),
        "externalModels": external_models,
        "missingRootTargets": sorted(missing_root_targets),
        "missingChildReferences": sorted(missing_child_references),
        "legacyModels": sorted(legacy_models),
        "missingCompiled": missing_compiled,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(
        f"FX closure: {len(targets)} roots, {len(closure)} particles, "
        f"{len(local_resources)} local resources, {len(external_resources)} external references."
    )
    if missing_root_targets:
        print("Missing runtime targets:")
        for resource in sorted(missing_root_targets):
            print(f"  {resource}")
    if missing_child_references:
        print("Missing child particle references inside converted mod assets:")
        for resource in sorted(missing_child_references):
            print(f"  {resource}")
    if legacy_models:
        print("Legacy VMDL wrappers rejected by current CS2:")
        for resource in sorted(legacy_models):
            print(f"  {resource}")
    if external_models:
        print("External model resources required by the converted mod closure:")
        for resource in external_models:
            print(f"  {resource}")
    if missing_compiled:
        print("Local resources missing compiled output:")
        for resource in missing_compiled:
            print(f"  {resource}")
    return 1 if missing_root_targets or legacy_models or external_models or missing_compiled else 0


if __name__ == "__main__":
    raise SystemExit(main())
