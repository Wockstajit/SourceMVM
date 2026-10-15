"""Resolves a mount name ('head', 'chest', 'hand_r', ...) to the real bone Deadlock's own hero
model actually has, for the P5 attach camera.

Backed by Automation/data/deadlock-hero-bones.json, extracted from the live game's own hero models
via Source2Viewer-CLI (see Automation/tools/deadlock-hero-bones-extract.py to regenerate it after a
Deadlock update). NOT a per-hero guess: every fallback below is ordered by how often that exact bone
name actually appears across the 38 heroes in that data (see
Automation/tests/deadlock-hero-bones-tests.py, which asserts every hero resolves every mount against
this same file).

  py -3 Automation/tools/deadlock-hero-bones.py <hero> <mount>   # one lookup, for the console/CLI
  py -3 Automation/tools/deadlock-hero-bones.py --summary        # coverage table

FINDINGS THAT SHAPED THIS TABLE (measured, not assumed -- the plan's original guessed chain
("weapon_hand_R -> hand_R" etc.) turned out half wrong once checked against real data):
  - head, neck_0, spine_0, pelvis, hand_L, hand_R, ankle_L, ankle_R: on ALL 38 heroes. Every chain
    below ends on one of these, so every mount resolves for every hero -- there is no per-hero
    exception list.
  - spine_2 / spine_1: on 37 of 38 (one hero has only spine_0), hence the 3-deep chest fallback.
  - The "off-hand attachment" bone is NOT one consistent name: "weapon_hand_R" (17 heroes),
    "weaponHand_R" (18 heroes, camelCase -- a different naming pass on those models), and
    "weapon_bone_R" (5 heroes) are three real, mutually exclusive spellings for the same idea. All
    three are tried before falling back to the universal "hand_R".
  - The muzzle point is the least consistent of all: "muzzle_fx" (33 heroes), "muzzle_flash" (11),
    or plain "muzzle" (2) cover all but 5 heroes, which instead expose "weaponTip"/"weaponEnd" (the
    end of their weapon's own bone chain) -- itself unavailable on Astro, which falls further to
    weapon_bone_R and then hand_R like the mounts above.
"""
from __future__ import annotations

import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DATA_PATH = REPO / "Automation" / "data" / "deadlock-hero-bones.json"

# Ordered candidate bone names per mount. The FIRST name present on a given hero's own bone list
# wins; the last entry in every chain is a bone confirmed present on all 38 heroes, so a lookup
# never falls through with no answer.
MOUNTS: dict[str, list[str]] = {
	"head": ["head"],
	"neck": ["neck_0"],
	"chest": ["spine_2", "spine_1", "spine_0"],
	"pelvis": ["pelvis"],
	"hand_l": ["weapon_hand_L", "weaponHand_L", "hand_L"],
	"hand_r": ["weapon_hand_R", "weaponHand_R", "weapon_bone_R", "hand_R"],
	"muzzle": ["muzzle_fx", "muzzle_flash", "muzzle", "weaponTip", "weaponEnd",
	           "weapon_hand_R", "weaponHand_R", "weapon_bone_R", "hand_R"],
	"foot_l": ["ankle_L"],
	"foot_r": ["ankle_R"],
}


def load_data(path: pathlib.Path = DATA_PATH) -> dict:
	with open(path, encoding="utf-8") as f:
		return json.load(f)


def resolve_mount(hero_bones: set[str] | list[str], mount: str) -> str | None:
	"""The real bone name for `mount` on a hero whose own bone set is `hero_bones`, or None if the
	mount name itself is unknown (a typo, not a missing bone -- a missing bone still resolves via
	the chain's universal fallback, see the module docstring)."""
	chain = MOUNTS.get(mount)
	if chain is None:
		return None
	bones = hero_bones if isinstance(hero_bones, set) else set(hero_bones)
	for candidate in chain:
		if candidate in bones:
			return candidate
	return None


def resolve_for_hero(data: dict, hero: str, mount: str) -> str | None:
	entry = data["heroes"].get(hero)
	if entry is None:
		return None
	return resolve_mount(entry["bones"], mount)


def _print_summary(data: dict) -> None:
	heroes = data["heroes"]
	print(f"{len(heroes)} heroes, {len(MOUNTS)} mounts")
	for mount in MOUNTS:
		misses = [h for h in heroes if resolve_for_hero(data, h, mount) is None]
		picked = {}
		for h in heroes:
			b = resolve_for_hero(data, h, mount)
			picked[b] = picked.get(b, 0) + 1
		print(f"  {mount:8s} misses={len(misses)}  " +
		      ", ".join(f"{b}={n}" for b, n in sorted(picked.items(), key=lambda kv: -kv[1])))


def main(argv: list[str]) -> int:
	if argv and argv[0] == "--summary":
		_print_summary(load_data())
		return 0
	if len(argv) != 2:
		print(__doc__.strip().splitlines()[0], file=sys.stderr)
		print("usage: deadlock-hero-bones.py <hero> <mount>  |  --summary", file=sys.stderr)
		return 2
	hero, mount = argv
	data = load_data()
	if mount not in MOUNTS:
		print(f"unknown mount '{mount}'; known: {', '.join(MOUNTS)}", file=sys.stderr)
		return 2
	bone = resolve_for_hero(data, hero, mount)
	if bone is None:
		print(f"no hero '{hero}' in {DATA_PATH.name}" if hero not in data["heroes"]
		      else f"'{hero}' has none of the bones {mount} tries (unexpected -- report this)",
		      file=sys.stderr)
		return 1
	print(bone)
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
