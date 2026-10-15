#!/usr/bin/env python3
"""Convert the Camera Shakify baked motion curves (Lua) into a C++ source file.

Standalone one-shot tool for the Handheld Camera feature. It has nothing to do with the
particle-FX converter (`convert-fx-source1.ps1`) and must never be folded into it --
different input, different output, different feature.

Input   reference/gmod-fx/camera-shakify/lua/autorun/sh_camerashakify_data.lua
Output  AfxHookSource2/Filmmaker/Movie/HandheldCamCurves.cpp

The Lua is a flat `data = {...}` array per preset, 6 floats per frame in the order
(rightOffset, upOffset, fwdOffset, pitch, yaw, roll). That layout is kept verbatim so the
C++ sampler is a plain indexed lookup, exactly like the Lua one.

The motion data originates from the Blender addon `EatTheFuture/camera_shakify` by
Nathan Vegdahl and Ian Hubert, released CC0. That attribution is emitted into the generated
file header -- do not strip it.

Usage (from the repo root):
    python FX/tools/convert-shakify-curves.py
    python FX/tools/convert-shakify-curves.py --check   # verify the committed file is current
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT = (
    REPO_ROOT / "reference" / "gmod-fx" / "camera-shakify" / "lua" / "autorun" / "sh_camerashakify_data.lua"
)
DEFAULT_OUTPUT = REPO_ROOT / "AfxHookSource2" / "Filmmaker" / "Movie" / "HandheldCamCurves.cpp"

# One preset block: CameraShakify.Shakes["KEY"] = { name = "...", fps = N, frames = N, data = {...} }
PRESET_RE = re.compile(
    r'CameraShakify\.Shakes\["(?P<key>[A-Z0-9_]+)"\]\s*=\s*\{'
    r'\s*name\s*=\s*"(?P<name>[^"]*)"\s*,'
    r'\s*fps\s*=\s*(?P<fps>[0-9.]+)\s*,'
    r'\s*frames\s*=\s*(?P<frames>[0-9]+)\s*,'
    r'\s*data\s*=\s*\{(?P<data>[^}]*)\}',
    re.S,
)
ORDER_RE = re.compile(r'CameraShakify\.ShakeOrder\s*=\s*\{(?P<body>[^}]*)\}', re.S)

VALUES_PER_FRAME = 6
VALUES_PER_LINE = 12


class ConversionError(RuntimeError):
    pass


def fmt(value: float) -> str:
    """Compact C++ float literal.

    The source data has 5 decimals; keep it and drop trailing zeros. A decimal point is always
    retained -- `0f` and `24f` are not valid float literals, only `0.0f` / `24.0f` are.
    """
    text = f"{value:.5f}"
    if "." in text:
        text = text.rstrip("0")
        if text.endswith("."):
            text += "0"
    if text == "-0.0":  # the Lua data contains literal `-0` entries
        text = "0.0"
    return text + "f"


def parse(lua_text: str) -> list[dict]:
    presets: dict[str, dict] = {}
    for match in PRESET_RE.finditer(lua_text):
        key = match.group("key")
        frames = int(match.group("frames"))
        values = [float(token) for token in match.group("data").split(",") if token.strip()]

        expected = frames * VALUES_PER_FRAME
        if len(values) != expected:
            raise ConversionError(
                f"{key}: frames={frames} implies {expected} values, found {len(values)}"
            )

        presets[key] = {
            "key": key,
            "name": match.group("name"),
            "fps": float(match.group("fps")),
            "frames": frames,
            "values": values,
        }

    if not presets:
        raise ConversionError("no presets matched -- did the Lua format change?")

    order_match = ORDER_RE.search(lua_text)
    if not order_match:
        raise ConversionError("CameraShakify.ShakeOrder not found")
    order = [token.strip().strip('"') for token in order_match.group("body").split(",") if token.strip()]

    missing = [key for key in order if key not in presets]
    if missing:
        raise ConversionError(f"ShakeOrder names presets that were not parsed: {missing}")
    extra = [key for key in presets if key not in order]
    if extra:
        raise ConversionError(f"parsed presets missing from ShakeOrder: {extra}")

    return [presets[key] for key in order]


def emit(presets: list[dict], source_name: str) -> str:
    total_frames = sum(preset["frames"] for preset in presets)
    total_values = total_frames * VALUES_PER_FRAME

    out: list[str] = []
    w = out.append

    w("// GENERATED FILE -- DO NOT EDIT BY HAND.")
    w("//")
    w("// Regenerate with:  python FX/tools/convert-shakify-curves.py")
    w(f"// Source:           {source_name}")
    w("//")
    w("// Baked handheld-camera motion curves for HandheldCam.h (the \"curve\" layer).")
    w("//")
    w("// The motion data is from the Blender addon `camera_shakify` by Nathan Vegdahl and")
    w("// Ian Hubert (github.com/EatTheFuture/camera_shakify), released under CC0. It reached us")
    w("// via the Garry's Mod \"Camera Shakify\" addon (workshop 3760884357), which flattened it")
    w("// into the per-frame array layout kept here. Keep this attribution.")
    w("//")
    w("// Layout: 6 floats per frame, in order")
    w("//   [0] right offset  [1] up offset  [2] forward offset   (world units, camera basis)")
    w("//   [3] pitch         [4] yaw        [5] roll             (degrees)")
    w(f"// {len(presets)} presets, {total_frames} frames, {total_values} floats total.")
    w("")
    w('#include "HandheldCam.h"')
    w("")
    w("namespace Filmmaker {")
    w("namespace {")
    w("")

    for preset in presets:
        values = preset["values"]
        w(f"// {preset['name']} -- {preset['frames']} frames @ {preset['fps']:g} fps")
        w(f"const float kCurve_{preset['key']}[] = {{")
        for start in range(0, len(values), VALUES_PER_LINE):
            chunk = values[start : start + VALUES_PER_LINE]
            w("\t" + ", ".join(fmt(value) for value in chunk) + ",")
        w("};")
        w("")

    w("} // namespace")
    w("")
    w("// Presentation order matches the source addon's ShakeOrder, so the UI dropdown and the")
    w("// console `handheld preset` argument list read the same as the original.")
    w("const HandheldCurve g_handheldCurves[] = {")
    for preset in presets:
        w(
            f'\t{{ "{preset["key"]}", "{preset["name"]}", '
            f'{preset["fps"]:.1f}f, {preset["frames"]}, kCurve_{preset["key"]} }},'
        )
    w("};")
    w("")
    w(f"const int g_handheldCurveCount = {len(presets)};")
    w("")
    w("} // namespace Filmmaker")
    w("")

    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail if the output file is not up to date")
    args = parser.parse_args(argv)

    if not args.input.is_file():
        print(f"error: input not found: {args.input}", file=sys.stderr)
        print("       extract the Camera Shakify addon first (see reference/gmod-fx/README.md)", file=sys.stderr)
        return 2

    try:
        presets = parse(args.input.read_text(encoding="utf-8", errors="replace"))
    except ConversionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    try:
        source_name = args.input.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        source_name = args.input.name
    text = emit(presets, source_name)

    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.is_file() else None
        if current != text:
            print(f"error: {args.output} is out of date -- rerun without --check", file=sys.stderr)
            return 1
        print(f"ok: {args.output} is up to date")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")

    total_frames = sum(preset["frames"] for preset in presets)
    print(f"wrote {args.output}")
    print(f"  {len(presets)} presets, {total_frames} frames, {total_frames * VALUES_PER_FRAME} floats")
    for preset in presets:
        print(f"    {preset['key']:<20} {preset['name']:<22} {preset['frames']:>4} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
