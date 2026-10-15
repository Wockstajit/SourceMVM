#!/usr/bin/env python3
"""Expand local #includes and extract adjacent R"TAG(... )TAG" C++ raw strings."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"\s*$', re.MULTILINE)
RAW_START_RE = re.compile(r'R"([A-Za-z0-9_]*)\(')


def extract(path: Path, stack: tuple[Path, ...] = ()) -> str:
    path = path.resolve()
    if path in stack:
        chain = " -> ".join(str(item) for item in (*stack, path))
        raise ValueError(f"recursive include: {chain}")

    source = path.read_text(encoding="utf-8")
    output: list[str] = []
    cursor = 0
    active_tag: str | None = None

    while cursor < len(source):
        if active_tag is None:
            include = INCLUDE_RE.match(source, cursor)
            if include:
                output.append(extract(path.parent / include.group(1), (*stack, path)))
                cursor = include.end()
                if cursor < len(source) and source[cursor] == "\n":
                    cursor += 1
                continue

            start = RAW_START_RE.search(source, cursor)
            next_include = None
            for match in INCLUDE_RE.finditer(source, cursor):
                next_include = match
                break
            if next_include and (start is None or next_include.start() < start.start()):
                output.append(extract(path.parent / next_include.group(1), (*stack, path)))
                cursor = next_include.end()
                if cursor < len(source) and source[cursor] == "\n":
                    cursor += 1
                continue
            if start is None:
                break
            active_tag = start.group(1)
            cursor = start.end()
            continue

        terminator = f"){active_tag}\""
        end = source.find(terminator, cursor)
        if end < 0:
            raise ValueError(f"unterminated raw string R\"{active_tag}( in {path}")
        output.append(source[cursor:end])
        cursor = end + len(terminator)
        active_tag = None

    return "".join(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="C++ header containing adjacent raw strings")
    parser.add_argument("output", type=Path, help="expanded JavaScript output")
    args = parser.parse_args()

    rendered = extract(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"wrote {args.output} ({len(rendered.encode('utf-8'))} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
