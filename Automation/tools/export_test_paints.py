#!/usr/bin/env python3
"""Export per-weapon legacy/modern test paints for verify script (one call)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from weapon_skin_database import get_database  # noqa: E402


def main() -> int:
    defs = [int(x) for x in sys.argv[1:] if x.isdigit()]
    db = get_database()
    out = {}
    for d in defs:
        p = db.pick_test_paints(d)
        if p:
            out[str(d)] = p
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
