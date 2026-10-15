"""Tests for Automation/tools/deadlock-hero-bones.py against the real extracted skeleton data.

  py -3 Automation/tests/deadlock-hero-bones-tests.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
	"deadlock_hero_bones", ROOT / "Automation" / "tools" / "deadlock-hero-bones.py")
hero_bones = importlib.util.module_from_spec(spec)
sys.modules["deadlock_hero_bones"] = hero_bones
spec.loader.exec_module(hero_bones)


class HeroBonesTests(unittest.TestCase):
	@classmethod
	def setUpClass(cls):
		cls.data = hero_bones.load_data()

	def test_data_file_actually_has_heroes(self):
		self.assertGreaterEqual(len(self.data["heroes"]), 30,
			"the extracted data file looks too small -- did the extractor run against the real VPK?")

	def test_every_hero_resolves_every_mount(self):
		# The one thing this whole module exists to guarantee: an attach camera must never be told
		# "no bone for that mount" for a hero who is actually in the game.
		misses = []
		for hero, entry in self.data["heroes"].items():
			bones = set(entry["bones"])
			for mount in hero_bones.MOUNTS:
				bone = hero_bones.resolve_mount(bones, mount)
				if bone is None:
					misses.append((hero, mount))
		if misses:
			print(f"\n{len(misses)} hero/mount combination(s) with no resolved bone:")
			for hero, mount in misses:
				print(f"  {hero}: {mount}")
		self.assertEqual(misses, [], "every hero must resolve every mount (see printed list above)")

	def test_resolved_bone_is_actually_on_that_hero(self):
		# resolve_mount must never hand back a name the hero's own model does not have -- that would
		# be a worse bug than "no bone found", because the caller would trust it and read garbage.
		for hero, entry in self.data["heroes"].items():
			bones = set(entry["bones"])
			for mount in hero_bones.MOUNTS:
				bone = hero_bones.resolve_mount(bones, mount)
				if bone is not None:
					self.assertIn(bone, bones, f"{hero}/{mount} resolved to '{bone}', not in its own bone list")

	def test_chain_order_is_respected_not_just_membership(self):
		# A hero with BOTH the first and second choice present must get the first choice, not
		# whichever the set iteration happens to produce.
		bones = {"weapon_hand_R", "weaponHand_R", "weapon_bone_R", "hand_R"}
		self.assertEqual(hero_bones.resolve_mount(bones, "hand_r"), "weapon_hand_R")
		bones.discard("weapon_hand_R")
		self.assertEqual(hero_bones.resolve_mount(bones, "hand_r"), "weaponHand_R")
		bones.discard("weaponHand_R")
		self.assertEqual(hero_bones.resolve_mount(bones, "hand_r"), "weapon_bone_R")
		bones.discard("weapon_bone_R")
		self.assertEqual(hero_bones.resolve_mount(bones, "hand_r"), "hand_R")

	def test_universal_mounts_never_need_a_fallback(self):
		# head/neck/pelvis/feet are single-entry chains because that exact bone is on all 38 heroes
		# (measured) -- a hero missing it would be new, real information worth seeing fail loudly
		# rather than quietly falling back. hand_l/hand_r/chest/muzzle are NOT in this list on
		# purpose: their first choice is a real bone that only SOME heroes have, which is exactly
		# why those mounts have a multi-entry chain at all (see test_chain_order_is_respected).
		for hero, entry in self.data["heroes"].items():
			bones = set(entry["bones"])
			for mount in ("head", "neck", "pelvis", "foot_l", "foot_r"):
				chain = hero_bones.MOUNTS[mount]
				self.assertIn(chain[0], bones,
					f"{hero} is missing '{chain[0]}' for '{mount}' -- previously measured as universal")

	def test_unknown_mount_name_is_reported_not_silently_none(self):
		self.assertIsNone(hero_bones.resolve_mount({"head", "hand_R"}, "not_a_real_mount"))

	def test_unknown_hero_resolves_to_none_not_a_crash(self):
		self.assertIsNone(hero_bones.resolve_for_hero(self.data, "not_a_real_hero", "head"))

	def test_cli_single_lookup(self):
		any_hero = next(iter(self.data["heroes"]))
		import io
		from contextlib import redirect_stdout
		buf = io.StringIO()
		with redirect_stdout(buf):
			rc = hero_bones.main([any_hero, "head"])
		self.assertEqual(rc, 0)
		self.assertEqual(buf.getvalue().strip(), "head")


if __name__ == "__main__":
	sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
