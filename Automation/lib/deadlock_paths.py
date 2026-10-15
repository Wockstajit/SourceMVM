"""Find Deadlock on THIS machine, for the Python side of Automation/.

The Deadlock counterpart of Automation/lib/steam_paths.py, kept as its own module so the CS2
resolver has no Deadlock branch in it and vice versa. Steam library enumeration itself is
game-neutral, so that one function is imported rather than copied.

Resolution order:

    1. $SOURCEMVM_DEADLOCK_DIR                   - explicit override, wins outright.
    2. Every Steam library, via steamapps/libraryfolders.vdf.

Usage:

    from deadlock_paths import deadlock_game_dirs
    DEFAULT_GAME_DIRS = deadlock_game_dirs()     # ...<install>/game paths, best first

Windows-only, like the rest of this repository. Returns [] rather than raising when nothing is
found, so callers keep their own "pass --game" error message.

Deadlock is Steam appid 1422450, its mod directory is `citadel` (not `csgo`), and its client
module lives at game/citadel/bin/win64/client.dll rather than beside the engine modules.
"""

from __future__ import annotations

import os
from pathlib import Path

from steam_paths import steam_libraries

__all__ = [
    "APP_ID",
    "MOD_DIR",
    "deadlock_install_dirs",
    "deadlock_game_dirs",
    "deadlock_exe",
]

APP_ID = 1422450
MOD_DIR = "citadel"

_APP_RELPATH = Path("steamapps") / "common" / "Deadlock"
_EXE_RELPATH = Path("game") / "bin" / "win64" / "deadlock.exe"


def deadlock_install_dirs() -> list[Path]:
    """Deadlock install roots (the folder holding game/bin/win64/deadlock.exe), best first."""
    found: list[Path] = []

    override = os.environ.get("SOURCEMVM_DEADLOCK_DIR", "").strip()
    if override:
        candidate = Path(override)
        if (candidate / _EXE_RELPATH).is_file():
            found.append(candidate)

    for library in steam_libraries():
        candidate = library / _APP_RELPATH
        if candidate not in found and (candidate / _EXE_RELPATH).is_file():
            found.append(candidate)
    return found


def deadlock_game_dirs() -> list[str]:
    """The `<install>/game` directories most of these tools actually want, best first."""
    return [str(install / "game") for install in deadlock_install_dirs()]


def deadlock_exe() -> str:
    """Full path to deadlock.exe, or '' when Deadlock is not installed."""
    for install in deadlock_install_dirs():
        return str(install / _EXE_RELPATH)
    return ""
