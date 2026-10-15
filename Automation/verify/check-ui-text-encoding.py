#!/usr/bin/env python3
"""Catch mojibake in the Panorama UI text -- in the sources AND in the built DLL.

WHY THIS EXISTS
    2026-09-02, reported from a screenshot: the Camera Editor showed `âš™` where a gear should be,
    `âœ•` for the close button, `â€”` for an em dash. The SOURCES were clean UTF-8 -- the bad bytes
    were in the shipped DLL, baked into a STALE OBJECT FILE from an earlier state of the headers.
    An incremental build never recompiled that translation unit, so the game kept rendering UI text
    that no longer existed anywhere in the repo. Reading the source could never have found it.

    That is the whole point of checking the binary: source-only review is blind to it.

WHAT MOJIBAKE LOOKS LIKE HERE
    Panorama decodes the script bytes as UTF-8. A glyph gets double-encoded when its UTF-8 bytes are
    first read as cp1252 (one char per byte) and then re-encoded to UTF-8 -- so `⚙` (E2 9A 99)
    becomes `âš™` (C3 A2 C5 A1 E2 84 A2). This script computes that exact transform for every
    non-ASCII character the UI actually uses and looks for the result.

USAGE
    python Automation/verify/check-ui-text-encoding.py              # sources + newest built DLL
    python Automation/verify/check-ui-text-encoding.py --dll <path> # a specific binary
    python Automation/verify/check-ui-text-encoding.py --sources-only

Exit code 0 = clean, 1 = mojibake found (or the requested DLL was missing).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PANORAMA = REPO / "AfxHookSource2" / "Filmmaker" / "Panorama"
DLL_CANDIDATES = [
    REPO / "build" / "x64-source2-release" / "AfxHookSource2" / "Release" / "AfxHookSource2.dll",
    REPO / "build" / "x64-release" / "AfxHookSource2" / "Release" / "AfxHookSource2.dll",
    REPO / "build" / "source2-release" / "bin" / "x64" / "AfxHookSource2.dll",
]


def double_encoded(ch: str) -> bytes | None:
    """The bytes `ch` turns into when its UTF-8 form is misread as cp1252 and re-encoded."""
    try:
        return ch.encode("utf-8").decode("cp1252").encode("utf-8")
    except (UnicodeDecodeError, UnicodeEncodeError):
        return None  # not every byte is mappable in cp1252; those glyphs cannot mojibake this way


def ui_characters() -> dict[str, list[str]]:
    """Every non-ASCII character used by the Panorama scripts, and which files use it."""
    found: dict[str, list[str]] = {}
    for path in sorted(PANORAMA.glob("*.h")) + sorted(PANORAMA.glob("*.inc")):
        text = path.read_text(encoding="utf-8")
        for ch in text:
            if ord(ch) > 127:
                found.setdefault(ch, [])
                if path.name not in found[ch]:
                    found[ch].append(path.name)
    return found


def check_sources() -> list[str]:
    """A source file that already CONTAINS mojibake (someone saved a mis-decoded buffer)."""
    problems = []
    # Â Ã â are the lead-ins of a double-encoded sequence; a following non-ASCII char confirms it.
    leads = "ÂÃâ"
    for path in sorted(PANORAMA.glob("*.h")) + sorted(PANORAMA.glob("*.inc")):
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.split("\n"), 1):
            for index, ch in enumerate(line[:-1]):
                if ch in leads and ord(line[index + 1]) > 127:
                    problems.append(f"{path.name}:{number}: {line.strip()[:100]}")
                    break
    return problems


def check_binary(dll: Path, characters: dict[str, list[str]]) -> list[str]:
    """The real catch: correct sources, wrong bytes in the shipped binary."""
    blob = dll.read_bytes()
    problems = []
    for ch, files in sorted(characters.items()):
        pattern = double_encoded(ch)
        if not pattern:
            continue
        count = blob.count(pattern)
        if count:
            where = blob.find(pattern)
            context = blob[max(0, where - 60):where].decode("utf-8", "replace")[-50:]
            problems.append(
                f"U+{ord(ch):04X} appears double-encoded {count}x "
                f"(used by {', '.join(files[:3])}) near: ...{context}"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--sources-only", action="store_true")
    args = parser.parse_args()

    characters = ui_characters()
    print(f"Panorama UI uses {len(characters)} distinct non-ASCII characters.")

    problems = check_sources()
    if problems:
        print("\nMOJIBAKE IN SOURCE:")
        for line in problems:
            print("  " + line)

    if not args.sources_only:
        dll = args.dll
        if dll is None:
            built = [p for p in DLL_CANDIDATES if p.exists()]
            if not built:
                print("\nNo built AfxHookSource2.dll found; skipping the binary check.")
                print("Build first -- the binary check is the one that catches a stale object file.")
                return 1 if problems else 0
            dll = max(built, key=lambda p: p.stat().st_mtime)
        if not dll.exists():
            print(f"\n{dll} does not exist.")
            return 1
        print(f"Checking binary: {dll}")
        binary_problems = check_binary(dll, characters)
        if binary_problems:
            print("\nMOJIBAKE IN THE BUILT DLL (sources may be clean -- suspect a stale .obj):")
            for line in binary_problems:
                print("  " + line)
            print("\nFix: force the Panorama translation units to recompile, e.g.")
            print("  (Get-ChildItem AfxHookSource2\\Filmmaker\\Panorama\\*.h) | "
                  "ForEach-Object { $_.LastWriteTime = Get-Date }")
            print("  cmake --build --preset x64-source2-release --target AfxHookSource2")
        problems += binary_problems

    if problems:
        print(f"\nFAIL: {len(problems)} problem(s).")
        return 1
    print("OK: no double-encoded UI text in the sources or the binary.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
