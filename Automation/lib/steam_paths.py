"""Find Counter-Strike 2 on THIS machine, for the Python side of Automation/.

The Python counterpart of Automation/lib/Resolve-Cs2Dir.ps1, and the same reasoning: these tools
used to open with a hand-written list of drive letters headed by the author's own Steam library,
so on any other machine the first entries were dead and only a default-location install worked at
all. Reading Steam's own library list covers every install instead of guessing five.

Resolution order:

    1. $SOURCEMVM_CS2_DIR                        - explicit override, wins outright.
    2. Steam's registered install path, then every library in steamapps/libraryfolders.vdf.
    3. The conventional install locations, as a last resort when Steam is not registered.

Usage, keeping each tool's existing "list of candidates" shape:

    from steam_paths import cs2_game_dirs
    DEFAULT_GAME_DIRS = cs2_game_dirs()      # ...<install>/game paths, best first

Windows-only, like the rest of this repository. Returns [] rather than raising when nothing is
found, so callers keep their own "pass --game" error message.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

__all__ = ["steam_libraries", "cs2_install_dirs", "cs2_game_dirs"]

_APP_RELPATH = Path("steamapps") / "common" / "Counter-Strike Global Offensive"
_EXE_RELPATH = Path("game") / "bin" / "win64" / "cs2.exe"

# Used only when Steam itself cannot be located. Deliberately generic: no drive letter here is
# anyone's in particular.
_CONVENTIONAL_STEAM_DIRS = [
    r"C:\Program Files (x86)\Steam",
    r"C:\Program Files\Steam",
    r"D:\Steam",
    r"D:\SteamLibrary",
]


def _registered_steam_dir() -> str:
    """Steam's install path from the registry, or '' when it is not registered."""
    try:
        import winreg
    except ImportError:  # not Windows
        return ""

    for root, key, value in (
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Valve\Steam", "SteamPath"),
    ):
        try:
            with winreg.OpenKey(root, key) as handle:
                found = winreg.QueryValueEx(handle, value)[0]
                if found:
                    return str(found).replace("/", "\\")
        except OSError:
            continue
    return ""


def steam_libraries() -> list[Path]:
    """Every Steam library folder on this machine, the main install first."""
    roots: list[str] = []
    registered = _registered_steam_dir()
    if registered:
        roots.append(registered)
    roots.extend(d for d in _CONVENTIONAL_STEAM_DIRS if d not in roots)

    libraries: list[Path] = []
    for root in roots:
        path = Path(root)
        if path not in libraries and path.is_dir():
            libraries.append(path)
        # Paths inside a .vdf are C-escaped: a real F:\SteamLibrary is stored "F:\\SteamLibrary".
        vdf = path / "steamapps" / "libraryfolders.vdf"
        try:
            text = vdf.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in re.finditer(r'"path"\s+"([^"]+)"', text, re.IGNORECASE):
            library = Path(match.group(1).replace("\\\\", "\\"))
            if library not in libraries and library.is_dir():
                libraries.append(library)
    return libraries


def cs2_install_dirs() -> list[Path]:
    """CS2 install roots (the folder holding game/bin/win64/cs2.exe), best first."""
    found: list[Path] = []

    override = os.environ.get("SOURCEMVM_CS2_DIR", "").strip()
    if override:
        candidate = Path(override)
        if (candidate / _EXE_RELPATH).is_file():
            found.append(candidate)

    for library in steam_libraries():
        candidate = library / _APP_RELPATH
        if candidate not in found and (candidate / _EXE_RELPATH).is_file():
            found.append(candidate)
    return found


def cs2_game_dirs() -> list[str]:
    """The `<install>/game` directories most of these tools actually want, best first."""
    return [str(install / "game") for install in cs2_install_dirs()]
