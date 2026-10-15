#!/usr/bin/env python3
"""Generate the cam-editor cosmetics catalog from the live CS2 game files.

Reads scripts/items/items_game.txt + resource/csgo_english.txt straight out of the
CS2 pak01_dir.vpk and emits:

  * AfxHookSource2/Filmmaker/Data/cosmetics.json          (full catalog, for reference/tooling)
  * AfxHookSource2/Filmmaker/Panorama/CameraEditorCosmeticsCatalog.inc
        (the COSMETICS = {...} JS object the cam-editor modal #includes)

Every released weapon|skin combination is enumerated from the loot lists (the same source
CS2 itself uses), so the catalog contains *every* skin in the game -- not a hand-picked stub.
Display names come from each paint kit's `description_tag` (the real finish name, e.g.
"Redline", "Hyper Beast"), never the internal dev name (e.g. "cu_ak47_cobra").

Requires the `vdf` package (pip install vdf). items_game.txt cannot be parsed reliably by a
hand-rolled KeyValues reader -- duplicate keys + scale silently drop ~80% of the paint kits.
"""
import sys
import argparse
import json
import re
import struct
from pathlib import Path

import vdf

# Where CS2 actually is on this machine; see Automation/lib/steam_paths.py.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from steam_paths import cs2_install_dirs  # noqa: E402


# defname -> (display name, item definition index, loadout slot)
WEAPONS = [
    ("weapon_deagle", "Desert Eagle", 1, "secondary"),
    ("weapon_elite", "Dual Berettas", 2, "secondary"),
    ("weapon_fiveseven", "Five-SeveN", 3, "secondary"),
    ("weapon_glock", "Glock-18", 4, "secondary"),
    ("weapon_ak47", "AK-47", 7, "primary"),
    ("weapon_aug", "AUG", 8, "primary"),
    ("weapon_awp", "AWP", 9, "primary"),
    ("weapon_famas", "FAMAS", 10, "primary"),
    ("weapon_g3sg1", "G3SG1", 11, "primary"),
    ("weapon_galilar", "Galil AR", 13, "primary"),
    ("weapon_m249", "M249", 14, "primary"),
    ("weapon_m4a1", "M4A4", 16, "primary"),
    ("weapon_mac10", "MAC-10", 17, "primary"),
    ("weapon_p90", "P90", 19, "primary"),
    ("weapon_mp5sd", "MP5-SD", 23, "primary"),
    ("weapon_ump45", "UMP-45", 24, "primary"),
    ("weapon_xm1014", "XM1014", 25, "primary"),
    ("weapon_bizon", "PP-Bizon", 26, "primary"),
    ("weapon_mag7", "MAG-7", 27, "primary"),
    ("weapon_negev", "Negev", 28, "primary"),
    ("weapon_sawedoff", "Sawed-Off", 29, "primary"),
    ("weapon_tec9", "Tec-9", 30, "secondary"),
    ("weapon_hkp2000", "P2000", 32, "secondary"),
    ("weapon_mp7", "MP7", 33, "primary"),
    ("weapon_mp9", "MP9", 34, "primary"),
    ("weapon_nova", "Nova", 35, "primary"),
    ("weapon_p250", "P250", 36, "secondary"),
    ("weapon_scar20", "SCAR-20", 38, "primary"),
    ("weapon_sg556", "SG 553", 39, "primary"),
    ("weapon_ssg08", "SSG 08", 40, "primary"),
    ("weapon_m4a1_silencer", "M4A1-S", 60, "primary"),
    ("weapon_usp_silencer", "USP-S", 61, "secondary"),
    ("weapon_cz75a", "CZ75-Auto", 63, "secondary"),
    ("weapon_revolver", "R8 Revolver", 64, "secondary"),
]

# defname -> (display name, def index, paint-name token used to find that knife's own finishes)
KNIVES = [
    ("weapon_bayonet", "Bayonet", 500, "bayonet"),
    ("weapon_knife_css", "Classic Knife", 503, "_knifecss"),
    ("weapon_knife_flip", "Flip Knife", 505, "flip"),
    ("weapon_knife_gut", "Gut Knife", 506, "gut"),
    ("weapon_knife_karambit", "Karambit", 507, "karam"),
    ("weapon_knife_m9_bayonet", "M9 Bayonet", 508, "m9_bay"),
    ("weapon_knife_tactical", "Huntsman Knife", 509, "huntsman"),
    ("weapon_knife_falchion", "Falchion Knife", 512, "falchion"),
    ("weapon_knife_survival_bowie", "Bowie Knife", 514, "bowie"),
    ("weapon_knife_butterfly", "Butterfly Knife", 515, "butterfly"),
    ("weapon_knife_push", "Shadow Daggers", 516, "push"),
    ("weapon_knife_cord", "Paracord Knife", 517, "cord"),
    ("weapon_knife_canis", "Survival Knife", 518, "canis"),
    ("weapon_knife_ursus", "Ursus Knife", 519, "ursus"),
    ("weapon_knife_gypsy_jackknife", "Navaja Knife", 520, "gypsy"),
    ("weapon_knife_outdoor", "Nomad Knife", 521, "outdoor"),
    ("weapon_knife_stiletto", "Stiletto Knife", 522, "stiletto"),
    ("weapon_knife_widowmaker", "Talon Knife", 523, "widow"),
    ("weapon_knife_skeleton", "Skeleton Knife", 525, "skeleton"),
    ("weapon_knife_kukri", "Kukri Knife", 526, "kukri"),
]

# Universal knife finishes (one canonical paint kit each, applied to every knife). The internal
# names are stable; IDs/display names/rarity/wear are resolved from the live data at generation.
SHARED_KNIFE_FINISH_NAMES = [
    "aa_fade",                 # Fade
    "hy_webs",                 # Crimson Web
    "aq_oiled",                # Case Hardened
    "aq_blued",                # Blue Steel
    "aq_forced",               # Stained
    "am_zebra",                # Slaughter
    "hy_forest_boreal",        # Boreal Forest
    "hy_ddpat",                # Forest DDPAT
    "sp_mesh_tan",             # Safari Mesh
    "sp_dapple",               # Scorched
    "sp_tape_urban",           # Urban Masked
    "so_night",                # Night
    "aq_damascus",             # Damascus Steel
    "an_tiger_orange",         # Tiger Tooth
    "so_purple",               # Ultraviolet
    "aq_steel",                # Rust Coat
    "am_marble_fade",          # Marble Fade
    "am_doppler_phase1",       # Doppler (Phase 1)
    "am_doppler_phase2",       # Doppler (Phase 2)
    "am_doppler_phase3",       # Doppler (Phase 3)
    "am_doppler_phase4",       # Doppler (Phase 4)
    "am_ruby_marbleized",      # Doppler (Ruby)
    "am_sapphire_marbleized",  # Doppler (Sapphire)
    "am_blackpearl_marbleized",# Doppler (Black Pearl)
    "am_gamma_doppler_phase1", # Gamma Doppler (Phase 1)
    "am_gamma_doppler_phase2", # Gamma Doppler (Phase 2)
    "am_gamma_doppler_phase3", # Gamma Doppler (Phase 3)
    "am_gamma_doppler_phase4", # Gamma Doppler (Phase 4)
    "am_emerald_marbleized",   # Gamma Doppler (Emerald)
]

# def index -> (display name, legacy paint-name prefix [no longer used for pairing]). Finish<->glove
# pairing is now icon-driven (see the gloves section); this list only fixes which gloves appear, their
# display names, and their order. 5028/5029 are the default team gloves and carry no finishes.
GLOVES = [
    (5027, "Bloodhound Gloves", "bloodhound"),
    (5030, "Sport Gloves", "sporty"),
    (5031, "Driver Gloves", "slick"),
    (5032, "Hand Wraps", "handwrap"),
    (5033, "Moto Gloves", "motorcycle"),
    (5034, "Specialist Gloves", "specialist"),
    (5035, "Hydra Gloves", "hydra"),
    (4725, "Broken Fang Gloves", "operation10"),
]

# rarity-tag substrings (paint_kits_rarity) -> numeric tier matching csgostyles.css color-rarity-*.
# Substring match in order, so longer/more specific tags must come first. Matched against the tag
# VALUE ("rarity_ancient"), never the kit name -- `cu_ssg08_immortal` is tagged `rare` and must stay
# Mil-Spec.
#
# Tier 7 is CONTRABAND (gold). Verified against the live items_game: the only kit above `ancient` is
# `cu_m4a1_howling -> immortal`, i.e. CS2 spells Contraband as `immortal` in paint_kits_rarity and
# never emits the literal `contraband` tag. Both map to 7 so the M4A4 | Howl stays gold either way.
# Do NOT "fix" immortal down to 6: that repaints the Howl Covert red.
RARITY_MAP = [
    ("contraband", 7), ("immortal", 7), ("ancient", 6), ("legendary", 5),
    ("mythical", 4), ("rare", 3), ("uncommon", 2), ("common", 1), ("default", 0),
]

# Item classes whose ITEM rarity is fixed regardless of the paint kit's own rarity. A paint kit like
# "Case Hardened" is Mil-Spec on a rifle, but ★ Karambit | Case Hardened is Covert -- the knife/glove
# item rarity wins. Reading rec["rarity"] for these produced a knife list smeared across every tier.
FIXED_ITEM_RARITY = {"knife": 6, "gloves": 6}

# phase/gem suffix for the marbleized (Doppler) finishes so they read unambiguously.
PHASE_SUFFIX = [
    ("gamma_doppler_phase1", "Phase 1"), ("gamma_doppler_phase2", "Phase 2"),
    ("gamma_doppler_phase3", "Phase 3"), ("gamma_doppler_phase4", "Phase 4"),
    ("doppler_phase1", "Phase 1"), ("doppler_phase2", "Phase 2"),
    ("doppler_phase3", "Phase 3"), ("doppler_phase4", "Phase 4"),
    ("ruby", "Ruby"), ("sapphire", "Sapphire"), ("blackpearl", "Black Pearl"),
    ("emerald_marbleized", "Emerald"),
]


class Vpk:
    """Minimal read-only VPK v1/v2 directory reader (enough to pull a few text/model entries)."""

    def __init__(self, dir_path: Path):
        self.dir_path = dir_path
        self.prefix = dir_path.name[:-8]
        self.base = dir_path.parent
        self.entries = {}
        self.header_size = 0
        self.tree_size = 0
        self._read_dir()

    def _read_z(self, f):
        out = bytearray()
        while True:
            b = f.read(1)
            if not b:
                raise EOFError("unexpected end of VPK tree")
            if b == b"\0":
                return out.decode("utf-8", "replace")
            out.extend(b)

    def _read_dir(self):
        with self.dir_path.open("rb") as f:
            header = f.read(28)
            sig, version, tree_size = struct.unpack("<III", header[:12])
            if sig != 0x55AA1234:
                raise RuntimeError(f"{self.dir_path} is not a VPK dir file")
            self.header_size = 28 if version == 2 else 12
            self.tree_size = tree_size
            f.seek(self.header_size)
            while True:
                ext = self._read_z(f)
                if not ext:
                    break
                while True:
                    folder = self._read_z(f)
                    if not folder:
                        break
                    while True:
                        name = self._read_z(f)
                        if not name:
                            break
                        raw = f.read(18)
                        crc, preload, archive, offset, length, term = struct.unpack("<IHHIIH", raw)
                        preload_data = f.read(preload) if preload else b""
                        if term != 0xFFFF:
                            raise RuntimeError("bad VPK entry terminator")
                        full = f"{folder}/{name}.{ext}" if folder != " " else f"{name}.{ext}"
                        self.entries[full.lower()] = (archive, offset, length, preload_data)

    def read(self, path):
        key = path.lower().replace("\\", "/")
        archive, offset, length, preload = self.entries[key]
        if archive == 0x7FFF:
            data_path = self.dir_path
            data_offset = self.header_size + self.tree_size + offset
        else:
            data_path = self.base / f"{self.prefix}_{archive:03d}.vpk"
            data_offset = offset
        with data_path.open("rb") as f:
            f.seek(data_offset)
            return preload + f.read(length)


def rarity_value(raw):
    raw = (raw or "").lower()
    for needle, value in RARITY_MAP:
        if needle in raw:
            return value
    return 3


def js_string(value):
    return json.dumps(value, ensure_ascii=True)


def title_from_model(model):
    stem = Path(model).stem
    # Both spellings, matching agent_label's own regex below. Stripping only the long form left
    # the short one in the FAMILY name -- "tm_professional_varf" became
    # "Professional Varf · Variant F", so the ten Professional variants each formed their own
    # one-member group in the agent picker instead of grouping under "Professional".
    stem = re.sub(r"_(?:variant|var)[a-z0-9]*$", "", stem)
    stem = re.sub(r"^(ctm|tm)_", "", stem)
    words = stem.replace("_", " ").split()
    acronyms = {"fbi", "sas", "swat", "gsg9", "idf", "tacp", "seal", "st6", "ksk", "usaf", "gign"}
    out = []
    for w in words:
        out.append(w.upper() if w.lower() in acronyms else w.capitalize())
    return " ".join(out) if out else stem


def agent_label(model):
    """Distinct per-variant agent label so the Agent dropdown never shows duplicate names.
    'ctm_fbi_varianta' -> 'FBI · Variant A'; 'tm_pro_var2' -> 'Pro · Variant 2'; base -> no suffix."""
    stem = Path(model).stem
    suffix = ""
    m = re.search(r"_(?:variant|var)([a-z0-9]+)$", stem)
    if m:
        token = m.group(1)
        suffix = " · Variant " + (token.upper() if token.isalpha() else token)
    return title_from_model(model) + suffix


class Catalog:
    def __init__(self, ig, loc):
        self.ig = ig
        self.loc = loc
        self.paint = {}      # id -> {name, tag, rarity, wmin, wmax}
        self.name2id = {}    # internal name -> id
        self._load_paint_kits()
        self.weapon_combos = self._load_weapon_combos()
        self.combo_rarity = self._load_combo_rarity()

    def _load_combo_rarity(self):
        """`[paintkit]weapon_defname` -> rarity tier, read from the rarity-suffixed client loot lists.

        THIS is the rarity CS2 actually displays for a weapon finish, and it is NOT
        `paint_kits_rarity`. Verified against the live data: `cu_usp_kill_confirmed` and
        `cu_ak47_asiimov` are both tagged `legendary` (Classified) in paint_kits_rarity, but sit in
        `*_ancient` loot lists -- and in game USP-S | Kill Confirmed and AK-47 | Asiimov are Covert.
        Trusting paint_kits_rarity collapsed the top tiers: whole weapons ended up with no Covert
        finish at all, and the unknown-kit fallback (Mil-Spec) became the largest bucket.

        Items that are in no rarity-suffixed loot list (the Howl, souvenir-only oddities) are absent
        here and fall back to the paint kit tag, which is what still makes the Howl Contraband.
        """
        suffixes = ("common", "uncommon", "rare", "mythical", "legendary",
                    "ancient", "immortal", "contraband")
        out = {}
        for list_name, members in self.ig.get("client_loot_lists", {}).items():
            low = str(list_name).lower()
            tier = None
            for suf in suffixes:
                if low.endswith("_" + suf):
                    tier = rarity_value(suf)
                    break
            if tier is None or not isinstance(members, dict):
                continue
            for entry in members:
                # Highest tier wins: a skin can appear in several collections, and a souvenir/
                # StatTrak variant list must never demote the base finish.
                if entry not in out or tier > out[entry]:
                    out[entry] = tier
        return out

    def _load_paint_kits(self):
        rar = self.ig.get("paint_kits_rarity", {})
        for key, data in self.ig.get("paint_kits", {}).items():
            if not str(key).isdigit() or not isinstance(data, dict):
                continue
            internal = data.get("name", "")
            if not internal or internal == "default":
                continue
            if "devtexture" in internal:  # leftover dev/test placeholder ("... | dev_texture")
                continue
            pid = int(key)
            tag = self.loc(data.get("description_tag", "")).strip()
            if not tag or tag.startswith("#"):
                tag = internal.replace("_", " ").title()
            rec = {
                "name": internal,
                "tag": tag,
                "rarity": rarity_value(rar.get(internal, "")),
                "wmin": float(data.get("wear_remap_min", 0.0)),
                "wmax": float(data.get("wear_remap_max", 1.0)),
            }
            self.paint[pid] = rec
            self.name2id[internal] = pid

    def _load_weapon_combos(self):
        """weapon classname -> set(paint internal name) from every loot list ([paint]weapon)."""
        pat = re.compile(r"^\[([^\]]+)\](.+)$")
        combos = {}

        def walk(node):
            try:
                items = node.items()
            except AttributeError:
                return
            for k, v in items:
                m = pat.match(k)
                if m and m.group(2).startswith("weapon_"):
                    combos.setdefault(m.group(2), set()).add(m.group(1))
                if hasattr(v, "items"):
                    walk(v)

        walk(self.ig.get("client_loot_lists", {}))
        walk(self.ig.get("revolving_loot_lists", {}))
        return combos

    def finish_name(self, pid):
        rec = self.paint.get(pid)
        if not rec:
            return None
        tag = rec["tag"]
        for needle, suffix in PHASE_SUFFIX:
            if needle in rec["name"]:
                return f"{tag} ({suffix})"
        return tag


def build():
    ap = argparse.ArgumentParser()
    _installs = cs2_install_dirs()
    ap.add_argument("--cs2-root", default=str(_installs[0]) if _installs else "")
    ap.add_argument("--json-out", default=r"AfxHookSource2\Filmmaker\Data\cosmetics.json")
    ap.add_argument("--js-out", default=r"AfxHookSource2\Filmmaker\Panorama\CameraEditorCosmeticsCatalog.inc")
    args = ap.parse_args()

    root = Path(args.cs2_root)
    vpk = Vpk(root / "game" / "csgo" / "pak01_dir.vpk")
    items_text = vpk.read("scripts/items/items_game.txt").decode("utf-8", "replace")
    english_text = vpk.read("resource/csgo_english.txt").decode("utf-8", "replace")

    ig = vdf.loads(items_text, mapper=vdf.VDFDict)["items_game"]
    tokens = vdf.loads(english_text, mapper=vdf.VDFDict)["lang"]["Tokens"]
    token_table = {str(k).lower(): v for k, v in tokens.items()}

    def loc(value):
        if not value:
            return ""
        key = value[1:] if value.startswith("#") else value
        return token_table.get(key.lower(), value)

    cat = Catalog(ig, loc)

    # Real CS2 econ thumbnail (relative to panorama/images/). For a painted skin the game ships a
    # pre-generated icon at econ/default_generated/<econ_name>_<paintkit_internal>_light_png.vtex_c;
    # the unpainted/base item is at econ/weapons/base_weapons/<econ_name>_png.vtex_c. Metadata stores
    # the rel path without prefix/extension; the Panorama UI expands it to
    # s2r://panorama/images/<img>_png.vtex.
    def normalize_img_rel(value):
        if not value:
            return ""
        rel = str(value).strip().replace("\\", "/")
        prefixes = (
            "s2r://panorama/images/",
            "panorama/images/",
            "file://{images}/",
        )
        for prefix in prefixes:
            if rel.startswith(prefix):
                rel = rel[len(prefix):]
                break
        if rel.endswith("_png.vtex_c"):
            rel = rel[:-len("_png.vtex_c")]
        elif rel.endswith("_png.vtex"):
            rel = rel[:-len("_png.vtex")]
        elif rel.lower().endswith(".png"):
            rel = rel[:-4]
        return rel.strip("/")

    def _img_exists(rel):
        rel = normalize_img_rel(rel)
        return bool(rel) and ("panorama/images/" + rel + "_png.vtex_c") in vpk.entries

    def attach_img(meta, rel):
        rel = normalize_img_rel(rel)
        if _img_exists(rel):
            meta["img"] = rel
            return True
        return False

    def gen_skin_img(econ_name, pid):
        if not econ_name:
            return None
        if pid and pid in cat.paint:
            # CS2 ships an icon per wear tier; most items have _light, some only _medium/_heavy.
            for tier in ("light", "medium", "heavy"):
                rel = f"econ/default_generated/{econ_name}_{cat.paint[pid]['name']}_{tier}"
                if _img_exists(rel):
                    return rel
            # No pre-generated icon for this exact combo -> fall back to the base item silhouette so
            # the row still shows the correct weapon/glove instead of a blank box.
        base = f"econ/weapons/base_weapons/{econ_name}"
        return base if _img_exists(base) else None

    # Glove econ item names (for default_generated icon paths), pulled live from items_game.
    glove_econ = {}
    for didx, data in ig.get("items", {}).items():
        if isinstance(data, dict) and data.get("prefab") in ("hands", "hands_paintable") and str(didx).isdigit():
            glove_econ[int(didx)] = data.get("name", "")

    def img_meta(meta, econ_name, pid):
        rel = gen_skin_img(econ_name, pid)
        if rel:
            meta["img"] = normalize_img_rel(rel)
        return meta

    catalog = {"agents": [], "weapons": [], "knives": [], "gloves": []}
    default_ct_agent = {"color": "#cfe3ffff", "team": "CT", "def": 5037, "paint": 0,
                        "model": "agents/models/ctm_sas/ctm_sas.vmdl"}
    attach_img(default_ct_agent, "econ/characters/customplayer_ctm_sas")
    default_t_agent = {"color": "#ffd9b0ff", "team": "T", "def": 5036, "paint": 0,
                       "model": "agents/models/tm_phoenix/tm_phoenix.vmdl"}
    attach_img(default_t_agent, "econ/characters/customplayer_tm_phoenix")
    js_slots = {
        # Team-aware default agents (so a CT player shows "Default CT Agent" with its real portrait,
        # a T player shows "Default T Agent"); the JS team filter hides the other side automatically.
        "agent": [
            ["Default CT Agent", "ct_default", default_ct_agent],
            ["Default T Agent", "t_default", default_t_agent],
        ],
        "primary": [],
        "secondary": [],
        # Team-specific default knives only (no generic "Default knife"): def 42 = CT default,
        # 59 = T default. Both are always listed (knife slot isn't team-filtered), mirroring gloves.
        "knife": [
            ["Default CT knife", "42:0", img_meta({"color": "rarColor(0)", "def": 42, "paint": 0, "team": "CT"}, "weapon_knife", 0)],
            ["Default T knife", "59:0", img_meta({"color": "rarColor(0)", "def": 59, "paint": 0, "team": "T"}, "weapon_knife_t", 0)],
        ],
        # Team-specific default gloves only (no generic "Default gloves"): 5029 = CT, 5028 = T.
        "gloves": [
            ["Default CT Gloves", "5029:0", img_meta({"color": "rarColor(0)", "def": 5029, "paint": 0, "team": "CT"}, glove_econ.get(5029, "ct_gloves"), 0)],
            ["Default T Gloves", "5028:0", img_meta({"color": "rarColor(0)", "def": 5028, "paint": 0, "team": "T"}, glove_econ.get(5028, "t_gloves"), 0)],
        ],
    }

    def skin_rarity(pid, item_class=None, defname=None):
        """Displayed rarity for one finish, best source first:
             1. a fixed item rarity (knives/gloves are always Covert)  -- FIXED_ITEM_RARITY
             2. the rarity-suffixed loot list this exact [kit]weapon combo ships in -- authoritative
             3. the paint kit's own tag -- only reached by items in no case (e.g. the Howl)
        """
        if item_class in FIXED_ITEM_RARITY:
            return FIXED_ITEM_RARITY[item_class]
        if defname:
            combo = cat.combo_rarity.get(f"[{cat.paint[pid]['name']}]{defname}")
            if combo is not None:
                return combo
        return cat.paint[pid]["rarity"]

    def skin_meta(defidx, pid, econ_name=None, item_class=None, defname=None):
        rec = cat.paint[pid]
        meta = {"color": f"rarColor({skin_rarity(pid, item_class, defname)})", "def": defidx, "paint": pid}
        if rec["wmin"] != 0.0:
            meta["wearMin"] = round(rec["wmin"], 4)
        if rec["wmax"] != 1.0:
            meta["wearMax"] = round(rec["wmax"], 4)
        return img_meta(meta, econ_name, pid)

    # ---- Weapons: every released weapon|skin from the loot lists ----
    for defname, wname, defidx, slot in WEAPONS:
        pids = sorted(
            cat.name2id[p] for p in cat.weapon_combos.get(defname, ()) if p in cat.name2id
        )
        # vanilla / stock entry first, then finishes A->Z
        js_slots[slot].append([f"Default {wname} skin", f"{defidx}:0",
                               img_meta({"color": "rarColor(0)", "def": defidx, "paint": 0}, defname, 0)])
        options = []
        cat_skins = []
        for pid in pids:
            finish = cat.finish_name(pid)
            options.append([f"{wname} | {finish}", str(pid), skin_meta(defidx, pid, defname, None, defname)])
            rec = cat.paint[pid]
            cat_skins.append({"name": f"{wname} | {finish}", "paintKit": pid,
                              "rarity": skin_rarity(pid, None, defname),
                              "wearMin": rec["wmin"], "wearMax": rec["wmax"]})
        options.sort(key=lambda o: o[0].lower())
        cat_skins.sort(key=lambda s: s["name"].lower())
        js_slots[slot].extend(options)
        catalog["weapons"].append({"name": wname, "defIndex": defidx, "slot": slot, "skins": cat_skins})

    # ---- Knives: shared universal finishes + each knife's own (token-matched) finishes ----
    shared_ids = [cat.name2id[n] for n in SHARED_KNIFE_FINISH_NAMES if n in cat.name2id]
    for defname, kname, defidx, token in KNIVES:
        pids = list(shared_ids)
        for pid, rec in cat.paint.items():
            if pid >= 10000:
                continue
            if token and token in rec["name"] and pid not in pids:
                pids.append(pid)
        seen_labels = set()
        cat_skins = []
        rows = []
        for pid in pids:
            finish = cat.finish_name(pid)
            label = f"{kname} | {finish}"
            if label in seen_labels:
                continue
            seen_labels.add(label)
            rows.append([label, f"{defidx}:{pid}", skin_meta(defidx, pid, defname, "knife")])
            rec = cat.paint[pid]
            cat_skins.append({"name": label, "paintKit": pid, "rarity": skin_rarity(pid, "knife"),
                              "wearMin": rec["wmin"], "wearMax": rec["wmax"]})
        rows.sort(key=lambda o: o[0].lower())
        cat_skins.sort(key=lambda s: s["name"].lower())
        js_slots["knife"].extend(rows)
        catalog["knives"].append({"name": kname, "defIndex": defidx, "skins": cat_skins})

    # ---- Gloves: enumerate each glove's REAL finishes from the icons the game actually ships ----
    # Grouping by paint-name prefix mis-pairs finishes (CS2's Hydra paints are named
    # `bloodhound_hydra_*`, so a prefix match drags them under Bloodhound and leaves Hydra empty).
    # Instead, pair finish<->glove by the pre-generated icon:
    #   panorama/images/econ/default_generated/<glove_econ>_<paintkit_internal>_<light|medium|heavy>.
    # This guarantees correct pairing AND that every row has a verified thumbnail.
    gen_icon_prefix = "panorama/images/econ/default_generated/"
    gen_icon_keys = [k for k in vpk.entries
                     if k.startswith(gen_icon_prefix) and k.endswith("_png.vtex_c")]
    for defidx, gname, _legacy_prefix in GLOVES:
        econ_name = glove_econ.get(defidx, "")
        rows = []
        cat_skins = []
        seen_pids = set()
        if econ_name:
            base = gen_icon_prefix + econ_name + "_"
            for key in gen_icon_keys:
                if not key.startswith(base):
                    continue
                mid = key[len(base):-len("_png.vtex_c")]
                for suf in ("_light", "_medium", "_heavy"):
                    if mid.endswith(suf):
                        mid = mid[:-len(suf)]
                        break
                pid = cat.name2id.get(mid)
                if pid is None or pid in seen_pids:
                    continue
                seen_pids.add(pid)
                finish = cat.finish_name(pid)
                label = f"{gname} | {finish}"
                rows.append([label, f"{defidx}:{pid}", skin_meta(defidx, pid, econ_name, "gloves")])
                rec = cat.paint[pid]
                cat_skins.append({"name": label, "paintKit": pid, "rarity": skin_rarity(pid, "gloves"),
                                  "wearMin": rec["wmin"], "wearMax": rec["wmax"]})
        rows.sort(key=lambda o: o[0].lower())
        cat_skins.sort(key=lambda s: s["name"].lower())
        js_slots["gloves"].extend(rows)
        catalog["gloves"].append({"name": gname, "defIndex": defidx, "skins": cat_skins})

    # ---- Agents: enumerate the shipped agent models from the VPK and ENRICH each with its real
    # econ item def + inventory image by reverse-looking-up the matching `customplayer_<stem>` item
    # in items_game. The def gives the dropdown a real CS2 thumbnail (via the ItemImage faux-id path
    # in the editor JS) and unlocks the agent cosmetic command; the variant suffix keeps every
    # variant a distinct, non-duplicate option. items_game `model_player` is an unreliable prefab
    # placeholder (often tm_phoenix), so the VPK model path stays the source of truth for the model.
    cp_items = {}  # "customplayer_<stem>" -> {def, image, team}
    for didx, data in ig.get("items", {}).items():
        if not isinstance(data, dict) or data.get("prefab") != "customplayer":
            continue
        nm = data.get("name", "")
        if not nm.startswith("customplayer_"):
            continue
        classes = data.get("used_by_classes") or {}
        team = "CT" if "counter-terrorists" in classes else ("T" if "terrorists" in classes else "")
        cp_items[nm] = {
            "def": int(didx) if str(didx).isdigit() else 0,
            "image": data.get("image_inventory", "") or "",
            "team": team,
        }

    agent_models = []
    for key in sorted(vpk.entries):
        if not key.startswith("agents/models/") or not key.endswith(".vmdl_c"):
            continue
        model = key[:-2]
        if "/ctm_" not in model and "/tm_" not in model:
            continue
        agent_models.append(model)
    agents_with_image = 0
    for model in agent_models:
        stem = Path(model).stem
        team = "CT" if "/ctm_" in model else "T"
        item = cp_items.get("customplayer_" + stem, {})
        agent_def = item.get("def", 0)
        if item.get("team"):
            team = item["team"]
        name = agent_label(model)
        meta = {"color": "rarColor(5)", "team": team, "def": agent_def, "paint": 0, "model": model}
        # Portrait image: EVERY agent variant ships a portrait at
        # panorama/images/econ/characters/customplayer_<stem>_png.vtex_c -- independent of whether the
        # variant has its own econ item def. Load it directly by s2r:// resource path in the JS so
        # the dropdown shows a real thumbnail for ALL agents, not just the def-mapped ones.
        img_rel = "econ/characters/customplayer_" + stem
        if attach_img(meta, img_rel):
            agents_with_image += 1
        elif attach_img(meta, item.get("image")):
            agents_with_image += 1
        js_slots["agent"].append([name, model, meta])
        catalog["agents"].append({"name": name, "team": team, "defIndex": agent_def,
                                  "model": model, "image": meta.get("img", "")})
    # A shipped model Valve never gave a portrait (tm_leet_variantk: model and materials, but no
    # customplayer item and no image) would be a blank tile. Borrow the nearest variant portrait of
    # the SAME family instead, and mark it so the UI can tell it is not the variant's own artwork.
    rows = js_slots["agent"]
    for row, entry in zip(rows[-len(agent_models):], catalog["agents"][-len(agent_models):]):
        meta = row[2]
        if meta.get("img"):
            continue
        family = title_from_model(row[1])
        donors = sorted((r for r in rows[-len(agent_models):]
                         if r[2].get("img") and not r[2].get("imgFallback")
                         and title_from_model(r[1]) == family), key=lambda r: r[1])
        if not donors:
            continue
        donor = min(donors, key=lambda r: abs(len(r[1]) - len(row[1])) * 100
                    + abs(ord(Path(r[1]).stem[-1]) - ord(Path(row[1]).stem[-1])))
        meta["img"] = donor[2]["img"]
        meta["imgFallback"] = True
        entry["image"] = meta["img"]
        print(f"agent portrait fallback: {Path(row[1]).stem} -> {meta['img']}")
    print(f"agents with portrait image: {agents_with_image}/{len(agent_models)}")

    return catalog, js_slots, args


def emit_inc(js_slots):
    def emit_meta(meta):
        parts = []
        for k, v in meta.items():
            if k == "color" and isinstance(v, str) and v.startswith("rarColor("):
                parts.append(json.dumps(k) + ":" + v)
            else:
                parts.append(json.dumps(k) + ":" + json.dumps(v, ensure_ascii=True))
        return "{" + ",".join(parts) + "}"

    def option_line(o, trailing):
        comma = "," if trailing else ""
        return "      [" + js_string(o[0]) + "," + js_string(o[1]) + "," + emit_meta(o[2]) + "]" + comma + "\n"

    chunks = []

    def add_chunk(text):
        if text:
            chunks.append('R"EDJS(\n' + text + ')EDJS"')

    add_chunk("    // Generated by Automation/tools/generate_cosmetics_catalog.py from CS2 pak01_dir.vpk.\n"
              "    // Contains every released weapon/knife/glove finish; do not hand-edit.\n    COSMETICS = {\n")
    slot_order = ["agent", "primary", "secondary", "knife", "gloves"]
    for slot_index, slot in enumerate(slot_order):
        add_chunk(f"      {slot}: [\n")
        opts = js_slots[slot]
        buf = ""
        for i, opt in enumerate(opts):
            line = option_line(opt, trailing=(i + 1 < len(opts)))
            if len(buf) + len(line) > 12000:  # keep each literal under MSVC's 16 KB C2026 cap
                add_chunk(buf)
                buf = ""
            buf += line
        add_chunk(buf)
        add_chunk("      ]" + ("," if slot_index + 1 < len(slot_order) else "") + "\n")
    add_chunk("    };\n")
    return "\n".join(chunks) + "\n"


def main():
    catalog, js_slots, args = build()
    json_out = Path(args.json_out)
    js_out = Path(args.js_out)
    json_out.parent.mkdir(parents=True, exist_ok=True)
    js_out.parent.mkdir(parents=True, exist_ok=True)
    json_out.write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding="utf-8")
    js_out.write_text(emit_inc(js_slots), encoding="utf-8")

    weapon_skins = sum(len(w["skins"]) for w in catalog["weapons"])
    knife_skins = sum(len(k["skins"]) for k in catalog["knives"])
    glove_skins = sum(len(g["skins"]) for g in catalog["gloves"])
    print(f"generated {json_out} and {js_out}")
    print(f"weapons={len(catalog['weapons'])} weapon_skins={weapon_skins} "
          f"knives={len(catalog['knives'])} knife_skins={knife_skins} "
          f"gloves={len(catalog['gloves'])} glove_skins={glove_skins} agents={len(catalog['agents'])}")


if __name__ == "__main__":
    main()
