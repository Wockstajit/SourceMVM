#!/usr/bin/env python3
"""Extract and retarget Half-Life: Alyx Combine humanoid animations for CS2 SOURCE:MVM.

Reads compiled Half-Life: Alyx enemy models via Source2Viewer-CLI (ValveResourceFormat) to GLB,
converts glTF Y-up meters into CS2 Z-up inches, and computes rest-pose-invariant world-space
rotation deltas + pelvis translation deltas for 51 core humanoid + finger bones shared 1-to-1
between Half-Life: Alyx enemies and CS2 player agents.

Models extracted (all share 100% identical 51-bone humanoid/finger hierarchy):
  - combine_grunt         (curated library + rifle combat/signals/reactions)
  - combine_soldier_heavy (true 0.0-deg-seam 8-way walk/run, walk stops, shotgun reload, shield)
  - combine_suppressor    (suppressor windup/fire/reload, 8-way sprint, injured limps)
  - metrocop_choreo       (8-way crouch-walk, 8-way walk/run, pistol/SMG/lowcover/baton/fire)

Outputs (all local / git-ignored under FX/actor-animations/ and FX/sources/alyx-deaths/):
  1. FX/actor-animations/actor_animations.mvmanim  (MVMACT02 binary pack for Bot Performance)
  2. FX/sources/alyx-deaths/alyx_deaths.json       (verified full-body HL:A death clips for
                                                    FX/tools/extract-death-anims.py)

Usage:
  python FX/tools/extract-alyx-anims.py [--hla-root "F:\\Half-Life - Alyx"] [--vrf-cli PATH]
"""

import argparse
import itertools
import json
import math
import os
import pickle
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VRF = os.environ.get("SOURCEMVM_VRF_CLI") or str(
    ROOT.parent / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe"
)
DEFAULT_HLA = os.environ.get("SOURCEMVM_HLA_ROOT") or r"F:\Half-Life - Alyx"
DEFAULT_OUT = ROOT / "FX" / "actor-animations" / "actor_animations.mvmanim"
DEFAULT_DEATHS_OUT = ROOT / "FX" / "sources" / "alyx-deaths" / "alyx_deaths.json"

# 51 core humanoid + finger bones shared between HL:A enemies and CS2 player agents.
# Tuple: (CS2 bone name, HL:A bone name)
CORE_BONES = [
    ("pelvis", "pelvis"),
    ("spine_0", "spine_0"),
    ("spine_1", "spine_1"),
    ("spine_2", "spine_2"),
    ("spine_3", "spine_3"),
    ("neck_0", "neck_0"),
    ("head_0", "head"),
    ("clavicle_L", "clavicle_L"),
    ("arm_upper_L", "arm_upper_L"),
    ("arm_lower_L", "arm_lower_L"),
    ("hand_L", "hand_L"),
    ("clavicle_R", "clavicle_R"),
    ("arm_upper_R", "arm_upper_R"),
    ("arm_lower_R", "arm_lower_R"),
    ("hand_R", "hand_R"),
    ("leg_upper_L", "leg_upper_L"),
    ("leg_lower_L", "leg_lower_L"),
    ("ankle_L", "ankle_L"),
    ("leg_upper_R", "leg_upper_R"),
    ("leg_lower_R", "leg_lower_R"),
    ("ankle_R", "ankle_R"),
    # 30 finger bones (15 left, 15 right) — shared 1-to-1 between HL:A enemies and CS2 agents
    ("finger_thumb_0_L", "finger_thumb_0_L"), ("finger_thumb_1_L", "finger_thumb_1_L"), ("finger_thumb_2_L", "finger_thumb_2_L"),
    ("finger_index_0_L", "finger_index_0_L"), ("finger_index_1_L", "finger_index_1_L"), ("finger_index_2_L", "finger_index_2_L"),
    ("finger_middle_0_L", "finger_middle_0_L"), ("finger_middle_1_L", "finger_middle_1_L"), ("finger_middle_2_L", "finger_middle_2_L"),
    ("finger_ring_0_L", "finger_ring_0_L"), ("finger_ring_1_L", "finger_ring_1_L"), ("finger_ring_2_L", "finger_ring_2_L"),
    ("finger_pinky_0_L", "finger_pinky_0_L"), ("finger_pinky_1_L", "finger_pinky_1_L"), ("finger_pinky_2_L", "finger_pinky_2_L"),
    ("finger_thumb_0_R", "finger_thumb_0_R"), ("finger_thumb_1_R", "finger_thumb_1_R"), ("finger_thumb_2_R", "finger_thumb_2_R"),
    ("finger_index_0_R", "finger_index_0_R"), ("finger_index_1_R", "finger_index_1_R"), ("finger_index_2_R", "finger_index_2_R"),
    ("finger_middle_0_R", "finger_middle_0_R"), ("finger_middle_1_R", "finger_middle_1_R"), ("finger_middle_2_R", "finger_middle_2_R"),
    ("finger_ring_0_R", "finger_ring_0_R"), ("finger_ring_1_R", "finger_ring_1_R"), ("finger_ring_2_R", "finger_ring_2_R"),
    ("finger_pinky_0_R", "finger_pinky_0_R"), ("finger_pinky_1_R", "finger_pinky_1_R"), ("finger_pinky_2_R", "finger_pinky_2_R"),
]

# Core bone that a limb bone points at (for direction retargeting). -1 = leaf / not corrected.
CORE_CHILD = [-1, -1, -1, -1, -1, -1, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1, 19, 20, -1] + [-1] * 30
CORE_L_HIP, CORE_R_HIP, CORE_ANKLES = 15, 18, (17, 20)

# Weapon the actor is posed for: 0 none/knife, 1 two-handed rifle, 2 pistol, 3 grenade.
WEAPON_NONE, WEAPON_RIFLE, WEAPON_PISTOL, WEAPON_GRENADE = 0, 1, 2, 3
WEAPON_OVERRIDE = {
    "hla_pound_door": WEAPON_NONE,
    "hla_pound_door_loop": WEAPON_NONE,
    "hla_poke_ground": WEAPON_NONE,
    "hla_push_button": WEAPON_NONE,
    "hla_carry_front_loop": WEAPON_NONE,
    "hla_carry_back_loop": WEAPON_NONE,
    "hla_grenade_throw": WEAPON_GRENADE,
    "hla_grenade_place": WEAPON_GRENADE,
}

# Categories matching ActorAnimationsMath.h
CAT_LOCOMOTION = 0
CAT_CROUCH = 1
CAT_WEAPON = 2
CAT_GESTURE = 3
CAT_IDLE = 4
CAT_REACTION = 5
CAT_CINEMATIC = 6

HLA_ENEMY_MODELS = [
    ("grunt", "models/characters/combine_grunt/combine_grunt.vmdl_c"),
    ("heavy", "models/characters/combine_soldier_heavy/combine_soldier_heavy.vmdl_c"),
    ("suppressor", "models/characters/combine_suppressor/combine_suppressor.vmdl_c"),
    ("metrocop", "models/characters/metrocop/metrocop_choreo.vmdl_c"),
]

# 8-way CS2 local movement octants: +X = fwd (0 deg), +Y = left (+90 deg), -Y = right (-90 deg), -X = back (+-180 deg)
OCTANT_TARGETS = [
    ("fwd", 0.0),
    ("fwd_l", 45.0),
    ("strafe_l", 90.0),
    ("back_l", 135.0),
    ("back", 180.0),
    ("back_r", -135.0),
    ("strafe_r", -90.0),
    ("fwd_r", -45.0),
]

OCTANT_LABELS = {
    "fwd": "Forward",
    "fwd_r": "Forward-Right",
    "strafe_r": "Strafe Right",
    "back_r": "Back-Right",
    "back": "Backward",
    "back_l": "Back-Left",
    "strafe_l": "Strafe Left",
    "fwd_l": "Forward-Left",
}

COMPASS_SUFFIXES = ["n", "ne", "e", "se", "s", "sw", "w", "nw"]

# True seamless native 8-way directional locomotion sets replacing old chopped slices.
# Tuple: (id_prefix, disp_prefix, model_key, src_prefix, cat, uniform_root_speed, weapon_class)
DIRECTIONAL_8WAY_SETS = [
    ("hla_walk", "Walk", "heavy", "com_sol_hev_sg_walk_loop_", CAT_LOCOMOTION, 66.0, WEAPON_RIFLE),
    ("hla_run", "Run", "metrocop", "run_", CAT_LOCOMOTION, 155.0, WEAPON_RIFLE),
    ("hla_heavy_run", "Heavy Shotgun Run", "heavy", "com_sol_hev_run_sg_combat_cycle_01_", CAT_LOCOMOTION, 96.0, WEAPON_RIFLE),
    ("hla_crouch_walk", "Crouch Walk", "metrocop", "crouch_walk_", CAT_CROUCH, 80.0, WEAPON_RIFLE),
    ("hla_sup_sprint", "Suppressor Sprint", "suppressor", "sprint_alt_suppressor_", CAT_LOCOMOTION, 125.0, WEAPON_RIFLE),
    ("hla_mc_walk", "Metrocop Walk", "metrocop", "walk_", CAT_LOCOMOTION, 52.0, WEAPON_RIFLE),
    ("hla_mc_run", "Metrocop Run", "metrocop", "run_", CAT_LOCOMOTION, 155.0, WEAPON_RIFLE),
]

# 4-way walk stop deceleration clips mapped by initial velocity vector
WALK_STOP_SPECS = [
    ("n", "s", "e", "w"),  # candidate suffixes from com_sol_hev_sg_walk_stop_*
]

# Preserved existing non-directional curated combine_grunt animations: (vmdl_anim_name, clean_id, display_name, category, loopable)
CURATED_ACTOR_CLIPS = [
    # Locomotion
    ("motion_match_walk_01", "hla_walk_01", "Tactical Walk 1", CAT_LOCOMOTION, True),
    ("motion_match_walk_02", "hla_walk_02", "Tactical Walk 2", CAT_LOCOMOTION, True),
    ("motion_match_walk_03", "hla_walk_03", "Tactical Walk 3", CAT_LOCOMOTION, True),
    ("motion_match_walk_04", "hla_walk_04", "Tactical Walk 4", CAT_LOCOMOTION, True),
    ("motion_match_walk_05", "hla_walk_05", "Tactical Walk 5", CAT_LOCOMOTION, True),
    ("run_down_axis_to_strafe_e", "hla_run_strafe_e", "Run Strafe Right", CAT_LOCOMOTION, False),
    ("run_down_axis_to_strafe_w", "hla_run_strafe_w", "Run Strafe Left", CAT_LOCOMOTION, False),
    ("run_large_turn_e", "hla_run_turn_e", "Run Turn Right", CAT_LOCOMOTION, False),
    ("run_large_turn_w", "hla_run_turn_w", "Run Turn Left", CAT_LOCOMOTION, False),
    ("combinesoldier_infestation_run_around_corner_01", "hla_run_corner_01", "Run Around Corner 1", CAT_LOCOMOTION, False),
    ("combinesoldier_infestation_run_around_corner_02", "hla_run_corner_02", "Run Around Corner 2", CAT_LOCOMOTION, False),
    ("combinesoldier_infestation_idle_pacing", "hla_patrol_pacing", "Patrol Pacing", CAT_LOCOMOTION, True),
    # Crouch
    ("crouch_idle_rifle_all", "hla_crouch_idle", "Crouch Idle (Rifle)", CAT_CROUCH, True),
    ("crouch_pose_new", "hla_crouch_pose", "Crouch Ready Pose", CAT_CROUCH, True),
    ("duck_all", "hla_duck", "Duck Down", CAT_CROUCH, False),
    ("duck_all_fast", "hla_duck_fast", "Duck Down Fast", CAT_CROUCH, False),
    ("supressor_roll_from_cover_crouch_cw", "hla_crouch_cover_roll", "Roll From Cover", CAT_CROUCH, False),
    # Aim & Weapon
    ("com_grunt_reload_standing_all", "hla_reload_grunt", "Reload Standing (Grunt)", CAT_WEAPON, False),
    ("reload_standing_all", "hla_reload_standing", "Reload Standing", CAT_WEAPON, False),
    ("reload_standing_all_02", "hla_reload_standing_02", "Reload Standing Alt", CAT_WEAPON, False),
    ("reload_from_idle_all", "hla_reload_from_idle", "Reload From Idle", CAT_WEAPON, False),
    ("com_grunt_grenade_throw_all", "hla_grenade_throw", "Throw Grenade", CAT_WEAPON, False),
    ("grenade_place_fast_all", "hla_grenade_place", "Place Grenade Fast", CAT_WEAPON, False),
    ("melee_gunhit_all", "hla_melee_bash", "Rifle Melee Bash", CAT_WEAPON, False),
    ("shoot_01", "hla_shoot_burst", "Shoot Burst", CAT_WEAPON, False),
    ("shoot_01_relax", "hla_shoot_relax", "Shoot & Relax", CAT_WEAPON, False),
    ("shoot_05_relax_loop", "hla_shoot_loop", "Sustained Fire Loop", CAT_WEAPON, True),
    ("shoot_posechange", "hla_shoot_posechange", "Aim Stance Shift", CAT_WEAPON, False),
    # Gestures & Signals
    ("signal_advance_all", "hla_signal_advance", "Signal: Advance", CAT_GESTURE, False),
    ("signal_forward_all", "hla_signal_forward", "Signal: Move Forward", CAT_GESTURE, False),
    ("signal_forward_look_around", "hla_signal_forward_look", "Signal: Forward & Scan", CAT_GESTURE, False),
    ("signal_group_all", "hla_signal_group", "Signal: Regroup", CAT_GESTURE, False),
    ("signal_halt_all", "hla_signal_halt", "Signal: Halt", CAT_GESTURE, False),
    ("signal_takecover_all", "hla_signal_takecover", "Signal: Take Cover", CAT_GESTURE, False),
    ("signal_holler_01", "hla_signal_holler_01", "Shout / Callout 1", CAT_GESTURE, False),
    ("signal_holler_02", "hla_signal_holler_02", "Shout / Callout 2", CAT_GESTURE, False),
    ("captain_signal_forward_check_left", "hla_signal_check_left", "Signal & Check Left", CAT_GESTURE, False),
    ("combinesoldier_custom_entrance_engage_signal_l", "hla_signal_engage", "Signal: Engage Target", CAT_GESTURE, False),
    ("combinesoldier_infestation_pound_on_door", "hla_pound_door", "Pound On Door", CAT_GESTURE, False),
    ("combinesoldier_infestation_pound_on_door_loop", "hla_pound_door_loop", "Pound On Door (Loop)", CAT_GESTURE, True),
    ("combinesoldier_infestation_idle_poke_ground_01", "hla_poke_ground", "Inspect / Poke Ground", CAT_GESTURE, False),
    # Idle
    ("combatidle", "hla_combat_idle", "Combat Idle", CAT_IDLE, True),
    ("idle_rifle_lowered", "hla_idle_lowered", "Idle (Rifle Lowered)", CAT_IDLE, True),
    ("idle_rifle_up", "hla_idle_rifle_up", "Idle (Rifle Raised)", CAT_IDLE, True),
    ("combat_to_idle", "hla_combat_to_idle", "Combat To Relaxed", CAT_IDLE, False),
    ("idle_to_combat", "hla_idle_to_combat", "Relaxed To Combat", CAT_IDLE, False),
    ("combatidle_to_firing", "hla_idle_to_firing", "Ready To Aim", CAT_IDLE, False),
    ("firing_to_combatidle", "hla_firing_to_idle", "Aim To Ready", CAT_IDLE, False),
    ("combine_grunt_questioning_citizens_xen_idle", "hla_guard_idle", "Guard / Questioning Idle", CAT_IDLE, True),
    # Reactions
    ("new_flinch_01", "hla_flinch_01", "Hit Flinch 1", CAT_REACTION, False),
    ("new_flinch_02", "hla_flinch_02", "Hit Flinch 2", CAT_REACTION, False),
    ("new_flinch_03", "hla_flinch_03", "Hit Flinch 3", CAT_REACTION, False),
    ("new_flinch_04", "hla_flinch_04", "Hit Flinch 4", CAT_REACTION, False),
    ("new_flinch_05", "hla_flinch_05", "Hit Flinch 5", CAT_REACTION, False),
    ("explosion_wince_a_n", "hla_explosion_wince_n", "Explosion Wince (Front)", CAT_REACTION, False),
    ("explosion_wince_a_s", "hla_explosion_wince_s", "Explosion Wince (Back)", CAT_REACTION, False),
    ("explosion_wince_a_e", "hla_explosion_wince_e", "Explosion Wince (Right)", CAT_REACTION, False),
    ("explosion_wince_a_w", "hla_explosion_wince_w", "Explosion Wince (Left)", CAT_REACTION, False),
    ("grenade_lands_wince_n", "hla_grenade_wince_n", "Grenade Wince (Front)", CAT_REACTION, False),
    ("grenade_panic_chest", "hla_grenade_panic", "Grenade Panic", CAT_REACTION, False),
    ("gas_can_panic_grunt", "hla_tank_panic", "Suit Breach Panic", CAT_REACTION, False),
    # Cinematic
    ("combinesoldier_custom_entrance_rubble_slide", "hla_rubble_slide", "Rubble Slide Entrance", CAT_CINEMATIC, False),
    ("entrance_push_gate_button", "hla_push_button", "Press Wall Button", CAT_CINEMATIC, False),
    ("jump_down_rifle", "hla_jump_down", "Jump Down (Rifle)", CAT_CINEMATIC, False),
    ("barrel_carrier_front_loop", "hla_carry_front_loop", "Carry Heavy Object (Front)", CAT_CINEMATIC, True),
    ("barrel_carrier_back_loop", "hla_carry_back_loop", "Carry Heavy Object (Back)", CAT_CINEMATIC, True),
    ("first_encounter_trainyard_entrance", "hla_trainyard_entrance", "Tactical Breach Entrance", CAT_CINEMATIC, False),
    ("soldier_entrance_01", "hla_soldier_entrance", "Doorway Entry", CAT_CINEMATIC, False),
    ("last_prevault_combat_01", "hla_prevault_01", "Vault Stance Entry 1", CAT_CINEMATIC, False),
    ("last_prevault_combat_02", "hla_prevault_02", "Vault Stance Entry 2", CAT_CINEMATIC, False),
]

DEATH_CANDIDATES = [
    "death_rear",
    "death_shoulder_a",
    "explosion_death_n",
    "new_death_01",
    "new_death_02",
    "new_death_05",
    "new_death_06",
    "new_death_07",
    "new_death_09",
    "new_death_10",
    "new_death_11",
    "new_death_12",
    "new_death_13",
    "new_death_14",
    "new_death_16",
    "new_death_17",
    "new_death_18",
    "new_death_20",
    "new_death_21",
    "new_death_22",
    "new_death_24",
]


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qnorm(q):
    n = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
    if n < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return (q[0] / n, q[1] / n, q[2] / n, q[3] / n)


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (
        vx + w * tx + y * tz - z * ty,
        vy + w * ty + z * tx - x * tz,
        vz + w * tz + x * ty - y * tx,
    )


def qslerp(a, b, t):
    dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    if dot < 0.0:
        b = (-b[0], -b[1], -b[2], -b[3])
        dot = -dot
    if dot > 0.9995:
        return qnorm((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]),
                      a[2] + t * (b[2] - a[2]), a[3] + t * (b[3] - a[3])))
    th0 = math.acos(max(-1.0, min(1.0, dot)))
    th = th0 * t
    s0 = math.cos(th) - dot * math.sin(th) / math.sin(th0)
    s1 = math.sin(th) / math.sin(th0)
    return qnorm((s0 * a[0] + s1 * b[0], s0 * a[1] + s1 * b[1],
                  s0 * a[2] + s1 * b[2], s0 * a[3] + s1 * b[3]))


def qangle_deg(a, b):
    dot = abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])
    dot = min(1.0, max(-1.0, dot))
    return math.degrees(2.0 * math.acos(dot))


# Pure right-handed rotation from glTF (X=left, Y=up, Z=fwd) to CS2 (X=fwd, Y=left, Z=up)
Q_G2S = (0.5, 0.5, 0.5, 0.5)
Q_S2G = qconj(Q_G2S)
INCHES_PER_METER = 1.0 / 0.0254


def vec_g2s(v):
    return (v[2] * INCHES_PER_METER, v[0] * INCHES_PER_METER, v[1] * INCHES_PER_METER)


def parse_glb(path: Path):
    data = path.read_bytes()
    jlen, _ = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20 : 20 + jlen])
    bin_off = 20 + jlen
    blen, _ = struct.unpack_from("<II", data, bin_off)
    blob = data[bin_off + 8 : bin_off + 8 + blen]

    acc_cache = {}

    def read_acc(idx):
        if idx in acc_cache:
            return acc_cache[idx]
        acc = gltf["accessors"][idx]
        bv = gltf["bufferViews"][acc["bufferView"]]
        off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        ncomp = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[acc["type"]]
        stride = bv.get("byteStride") or (ncomp * 4)
        res = [struct.unpack_from(f"<{ncomp}f", blob, off + i * stride) for i in range(acc["count"])]
        acc_cache[idx] = res
        return res

    return gltf, read_acc


def sample_track(times, vals, t, is_quat):
    if len(times) == 1 or t <= times[0]:
        return vals[0]
    if t >= times[-1]:
        return vals[-1]
    lo, hi = 0, len(times) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    span = times[hi] - times[lo]
    alpha = (t - times[lo]) / span if span > 1e-8 else 0.0
    if is_quat:
        return qslerp(vals[lo], vals[hi], alpha)
    a, b = vals[lo], vals[hi]
    return (a[0] + alpha * (b[0] - a[0]), a[1] + alpha * (b[1] - a[1]), a[2] + alpha * (b[2] - a[2]))


def export_model_glb(vrf_cli: Path, hla_vpk: Path, vmdl_path: str, out_glb: Path):
    cmd = [
        str(vrf_cli),
        "-i", str(hla_vpk),
        "--vpk_filepath", vmdl_path,
        "-o", str(out_glb),
        "-d",
        "--gltf_export_format", "glb",
        "--gltf_export_animations",
        "--gltf_compose_additive",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not out_glb.exists():
        raise RuntimeError(f"Source2Viewer-CLI failed for {vmdl_path} ({proc.returncode}):\n{proc.stdout}\n{proc.stderr}")


def export_cs2_bind_glb(vrf_cli: Path, cs2_vpk: Path, out_glb: Path):
    cmd = [str(vrf_cli), "-i", str(cs2_vpk), "--vpk_filepath", "agents/models/ctm_sas/ctm_sas.vmdl_c",
           "-o", str(out_glb), "-d", "--gltf_export_format", "glb"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not out_glb.exists():
        raise RuntimeError(f"Source2Viewer-CLI failed ({proc.returncode}): {proc.stdout} {proc.stderr}")


def cs2_bind_pose(glb_path: Path):
    """World position (inches) and rotation (CS2 space) of the 51 core bones in the CS2 agent's bind pose."""
    gltf, _ = parse_glb(glb_path)
    nodes = gltf["nodes"]
    parent = [-1] * len(nodes)
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    name_to_idx = {n.get("name", ""): i for i, n in enumerate(nodes)}
    wp = [None] * len(nodes)
    wq = [None] * len(nodes)

    def visit(i):
        if wp[i] is not None:
            return
        t = tuple(nodes[i].get("translation", [0.0, 0.0, 0.0]))
        r = qnorm(tuple(nodes[i].get("rotation", [0.0, 0.0, 0.0, 1.0])))
        p = parent[i]
        if p < 0:
            wp[i], wq[i] = t, r
        else:
            visit(p)
            rp = qrot(wq[p], t)
            wp[i] = (wp[p][0] + rp[0], wp[p][1] + rp[1], wp[p][2] + rp[2])
            wq[i] = qnorm(qmul(wq[p], r))

    out = []
    for cs2_name, _ in CORE_BONES:
        if cs2_name not in name_to_idx:
            raise RuntimeError(f"CS2 agent is missing bone {cs2_name}")
        i = name_to_idx[cs2_name]
        visit(i)
        out.append((vec_g2s(wp[i]), qnorm(qmul(Q_G2S, wq[i]))))
    return out


def qz(angle):
    h = angle * 0.5
    return (0.0, 0.0, math.sin(h), math.cos(h))


SKIP_SUBSTRINGS = (
    "additive", "_add", "add_", "aim_matrix", "aim_9_", "aim_crouch", "aim_duck",
    "aim_lean_poseparam", "poseparam", "nofootrepos", "_zero", "zeroer", "_layer",
    "gest_", "gesture_add", "not_aiming", "_lod", "reference", "refpose", "bind",
    "tpose", "a_pose", "ragdoll", "pre_idle", "preidle", "start_idle", "end_idle",
    "death_poses", "head_poses", "head_roll", "baton_ready", "_finished",
    "aaaaa_test", "_test", "_fake_", "nohead", "face_", "eyes_", "lipsync", "phoneme",
    "notags", "scripted_sequence", "bonemask", "idle_mask_pose", "shield_", "baton_to_shield",
    # Map-specific long NPC sequences & creatures
    "a1_", "a2_", "a3_", "a4_", "a5_", "choreo_", "cine_", "vault_", "carpark_",
    "eli_", "russell_", "jeff_", "gman_", "strider_", "intro_world_", "van_crash_",
    "cit_cop_", "vista_guard_", "broken_wall_", "antlion_", "zombie_", "headcrab_",
    "xen_", "citizen", "alyx_", "vort_", "combine_sniper_", "barnacle_tongue",
    # Death takes (live under Death Animations instead)
    "death_", "new_death_", "explosion_death_", "crushed_barnacle", "panic_death",
    # Redundant motion-matching transition graph snippets
    "motion_match_", "strafe_n_to_", "strafe_e_to_", "strafe_s_to_", "strafe_w_to_",
    "strafe_ne_to_", "strafe_nw_to_", "strafe_se_to_", "strafe_sw_to_",
    "stand_to_run_", "stand_to_strafe_", "firing_walk_motion_match_",
    "run_n_to_", "run_e_to_", "run_s_to_", "run_w_to_",
    "run_ne_to_", "run_nw_to_", "run_se_to_", "run_sw_to_",
    "run_down_axis_", "combat_walk_to_stand_",
)


def is_clean_enemy_anim(name: str, dur: float, keep_exact: set) -> bool:
    if name in keep_exact:
        return True
    low = name.lower()
    if low.startswith("@"):
        return False
    if dur < 0.18 or dur > 22.0:
        return False
    if any(sub in low for sub in SKIP_SUBSTRINGS):
        return False
    # Skip shield walk start/stop variations except if we want shield walk loop / attack / deploy
    if "shield_walk_start_" in low or "shield_walk_stop_" in low:
        return False
    # Skip sg_walk_start_ / sg_walk_stop_ (we explicitly pull 4-way walk_stop)
    if "sg_walk_start_" in low or "sg_walk_stop_" in low:
        return False
    return True


def seam_angle_deg(frames) -> float:
    if len(frames) < 2:
        return 0.0
    r0 = frames[0][1]
    r1 = frames[-1][1]
    # Check main humanoid 21 bones (pelvis..ankles)
    return max(qangle_deg(r0[b], r1[b]) for b in range(21))


def close_loop_seam(frames):
    """Distribute any boundary seam multiplicatively across the whole cycle so angular velocity stays continuous."""
    n = len(frames)
    if n < 4:
        return frames
    r0 = frames[0][1]
    rn = frames[-1][1]
    nb = len(r0)
    q_err = [qnorm(qmul(r0[b], qconj(rn[b]))) for b in range(nb)]
    pz_err = frames[0][0][2] - frames[-1][0][2]
    lift_err = frames[0][3] - frames[-1][3]
    out = []
    for fi in range(n):
        u = float(fi) / float(n - 1)
        p_cur, r_cur, d_cur, l_cur = frames[fi]
        if fi == n - 1:
            out.append(((p_cur[0], p_cur[1], frames[0][0][2]), list(r0), list(frames[0][2]), frames[0][3]))
            continue
        p_new = (p_cur[0], p_cur[1], p_cur[2] + u * pz_err)
        l_new = l_cur + u * lift_err
        r_new = []
        d_new = []
        for b in range(nb):
            dq = qslerp((0.0, 0.0, 0.0, 1.0), q_err[b], u)
            r_new.append(qnorm(qmul(dq, r_cur[b])))
            if b < 21 and (d_cur[b][0] != 0.0 or d_cur[b][1] != 0.0 or d_cur[b][2] != 0.0):
                d_new.append(qrot(dq, d_cur[b]))
            else:
                d_new.append(d_cur[b])
        out.append((p_new, r_new, d_new, l_new))
    return out


def bake_clips(glb_path: Path, anim_filter_fn):
    gltf, read_acc = parse_glb(glb_path)
    nodes = gltf["nodes"]
    parent = [-1] * len(nodes)
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    name_to_idx = {n.get("name", ""): i for i, n in enumerate(nodes)}

    core_idx = []
    for _cs2_name, hla_name in CORE_BONES:
        if hla_name not in name_to_idx:
            raise RuntimeError(f"Missing HL:A bone {hla_name} in {glb_path.name}")
        core_idx.append(name_to_idx[hla_name])

    # Topologically ordered chain of ancestor nodes needed for core_idx forward kinematics
    needed_nodes = set(core_idx)
    for idx in list(needed_nodes):
        p = parent[idx]
        while p >= 0 and p not in needed_nodes:
            needed_nodes.add(p)
            p = parent[p]
    eval_order = sorted(needed_nodes)

    base_t = [tuple(n.get("translation", [0.0, 0.0, 0.0])) for n in nodes]
    base_r = [qnorm(tuple(n.get("rotation", [0.0, 0.0, 0.0, 1.0]))) for n in nodes]

    def eval_needed_world(lt, lr):
        wp = {}
        wq = {}
        for i in eval_order:
            p = parent[i]
            if p < 0 or p not in wp:
                wp[i] = lt[i]
                wq[i] = lr[i]
            else:
                rp = qrot(wq[p], lt[i])
                wp[i] = (wp[p][0] + rp[0], wp[p][1] + rp[1], wp[p][2] + rp[2])
                wq[i] = qnorm(qmul(wq[p], lr[i]))
        return wp, wq

    rest_wp, rest_wq = eval_needed_world(base_t, base_r)
    rest_pelvis_s = vec_g2s(rest_wp[core_idx[0]])
    rest_pos_s = [vec_g2s(rest_wp[i]) for i in core_idx]
    rest_ankle_z = min(rest_pos_s[a][2] for a in CORE_ANKLES)

    def facing_yaw(pos_s):
        lx = pos_s[CORE_L_HIP][0] - pos_s[CORE_R_HIP][0]
        ly = pos_s[CORE_L_HIP][1] - pos_s[CORE_R_HIP][1]
        return math.atan2(ly, lx) - math.pi * 0.5

    rest_yaw = facing_yaw(rest_pos_s)
    rest_q_s = [qnorm(qmul(Q_G2S, rest_wq[i])) for i in core_idx]
    rest_q_s_inv = [qconj(q) for q in rest_q_s]

    baked = {}
    for anim in gltf.get("animations", []):
        name = anim.get("name", "")
        max_t = 0.0
        for ch in anim["channels"]:
            samp = anim["samplers"][ch["sampler"]]
            acc = gltf["accessors"][samp["input"]]
            if "max" in acc and acc["max"]:
                max_t = max(max_t, float(acc["max"][0]))
        if max_t <= 1e-4 or not anim_filter_fn(name, max_t):
            continue

        tracks = {}
        for ch in anim["channels"]:
            ni = ch["target"].get("node")
            path = ch["target"].get("path")
            if ni is None or ni not in needed_nodes or path not in ("translation", "rotation"):
                continue
            samp = anim["samplers"][ch["sampler"]]
            times = [x[0] for x in read_acc(samp["input"])]
            vals = read_acc(samp["output"])
            tracks[(ni, path)] = (times, vals)
        if not tracks:
            continue

        sample_fps = 30.0 if max_t <= 8.0 else 20.0
        frame_count = max(2, int(round(max_t * sample_fps)) + 1)
        frames = []
        pelvis_zs = []
        xy_travel = 0.0
        first_xy = None
        psi0 = None
        prev_psi = 0.0
        max_yaw_limit = math.radians(28.0)

        for fi in range(frame_count):
            t = min(max_t, fi / sample_fps)
            lt = list(base_t)
            lr = list(base_r)
            for (ni, path), (times, vals) in tracks.items():
                if path == "translation":
                    lt[ni] = sample_track(times, vals, t, False)
                else:
                    lr[ni] = qnorm(sample_track(times, vals, t, True))
            wp, wq = eval_needed_world(lt, lr)
            pos_s = [vec_g2s(wp[i]) for i in core_idx]
            psi_raw = facing_yaw(pos_s) - rest_yaw
            if psi0 is None:
                psi0 = math.atan2(math.sin(psi_raw), math.cos(psi_raw))
                prev_psi = psi0
                psi = psi0
            else:
                dpsi = math.atan2(math.sin(psi_raw - prev_psi), math.cos(psi_raw - prev_psi))
                psi = prev_psi + dpsi
                prev_psi = psi
            delta_yaw = psi - psi0
            if abs(delta_yaw) > max_yaw_limit:
                excess = delta_yaw - math.copysign(max_yaw_limit, delta_yaw)
                yaw_fix = qz(-excess)
            else:
                yaw_fix = (0.0, 0.0, 0.0, 1.0)
            p_s = pos_s[0]
            if first_xy is None:
                first_xy = (p_s[0] - rest_pelvis_s[0], p_s[1] - rest_pelvis_s[1])
            dx = (p_s[0] - rest_pelvis_s[0]) - first_xy[0]
            dy = (p_s[1] - rest_pelvis_s[1]) - first_xy[1]
            p_delta = (dx, dy, p_s[2] - rest_pelvis_s[2])
            pelvis_zs.append(p_s[2])
            xy_travel = max(xy_travel, math.hypot(dx, dy))

            r_deltas = []
            for bi, ni in enumerate(core_idx):
                q_s = qnorm(qmul(Q_G2S, wq[ni]))
                qd = qnorm(qmul(yaw_fix, qnorm(qmul(q_s, rest_q_s_inv[bi]))))
                r_deltas.append(qd)
            dirs = []
            for bi in range(len(core_idx)):
                c = CORE_CHILD[bi]
                if c < 0:
                    dirs.append((0.0, 0.0, 0.0))
                    continue
                d = (pos_s[c][0] - pos_s[bi][0], pos_s[c][1] - pos_s[bi][1], pos_s[c][2] - pos_s[bi][2])
                n = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
                d = (d[0] / n, d[1] / n, d[2] / n) if n > 1e-6 else (0.0, 0.0, 0.0)
                dirs.append(qrot(yaw_fix, d))
            lift = min(pos_s[a][2] for a in CORE_ANKLES) - rest_ankle_z
            frames.append((p_delta, r_deltas, dirs, lift))

        pelvis_drop = pelvis_zs[0] - min(pelvis_zs)
        path_len = sum(math.hypot(fb[0][0] - fa[0][0], fb[0][1] - fa[0][1]) for fa, fb in zip(frames, frames[1:]))
        root_speed = path_len / max_t

        # Unwrap quaternion hemisphere signs across frames and gently denoise mocap jitter
        for bi in range(len(core_idx)):
            for fi in range(1, len(frames)):
                pq, cq = frames[fi - 1][1][bi], frames[fi][1][bi]
                if pq[0]*cq[0] + pq[1]*cq[1] + pq[2]*cq[2] + pq[3]*cq[3] < 0.0:
                    frames[fi][1][bi] = (-cq[0], -cq[1], -cq[2], -cq[3])
        if len(frames) >= 5:
            smoothed = [list(f[1]) for f in frames]
            for fi in range(1, len(frames) - 1):
                for bi in range(len(core_idx)):
                    qa, qb, qc = frames[fi - 1][1][bi], frames[fi][1][bi], frames[fi + 1][1][bi]
                    if 0.35 < max(qangle_deg(qa, qb), qangle_deg(qb, qc)) < 10.0:
                        smoothed[fi][bi] = qslerp(qb, qslerp(qa, qc, 0.5), 0.28)
            for fi in range(len(frames)):
                p_d, _r, d_v, lf = frames[fi]
                frames[fi] = (p_d, smoothed[fi], d_v, lf)

        # Measure raw arm/leg deviation & hand span across start/mid/end for Blender composition triage
        def vec_ang(u, v):
            lu = math.sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2])
            lv = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
            if lu < 1e-6 or lv < 1e-6:
                return 0.0
            d = max(-1.0, min(1.0, (u[0]*v[0] + u[1]*v[1] + u[2]*v[2]) / (lu * lv)))
            return math.degrees(math.acos(d))
        dev_arm = max(
            qangle_deg((0.0, 0.0, 0.0, 1.0), frames[fi][1][bi])
            for fi in (0, len(frames) // 2, len(frames) - 1)
            for bi in (8, 9, 12, 13)
        )
        dev_leg = max(
            qangle_deg((0.0, 0.0, 0.0, 1.0), frames[fi][1][bi])
            for fi in (0, len(frames) // 2, len(frames) - 1)
            for bi in (15, 16, 17, 18, 19, 20)
        )
        # Approximate hand span & elbow angle from dirs (CS2 bone lengths ~10.5" upper, ~10" lower)
        el_min = 180.0
        hdist_min = 999.0
        for fi in (0, len(frames) // 2, len(frames) - 1):
            dr = frames[fi][2]
            er = vec_ang(dr[12], dr[13])
            el = vec_ang(dr[8], dr[9])
            el_min = min(el_min, max(er, el))
            r_hand = (dr[12][0]*10.5 + dr[13][0]*10.0, dr[12][1]*10.5 + dr[13][1]*10.0 - 6.5, dr[12][2]*10.5 + dr[13][2]*10.0)
            l_hand = (dr[8][0]*10.5 + dr[9][0]*10.0, dr[8][1]*10.5 + dr[9][1]*10.0 + 6.5, dr[8][2]*10.5 + dr[9][2]*10.0)
            hd = math.sqrt((r_hand[0]-l_hand[0])**2 + (r_hand[1]-l_hand[1])**2 + (r_hand[2]-l_hand[2])**2)
            hdist_min = min(hdist_min, hd)
        is_idle = "idle" in name.lower() and "to_" not in name.lower()
        need_blender = (dev_leg < 8.0) or (dev_arm < 34.0) or (hdist_min > 34.0)
        rejected = (not is_idle) and ((dev_arm < 24.0) or (el_min < 25.0 and hdist_min > 34.0) or (dev_leg < 4.0))

        seam_deg = seam_angle_deg(frames)
        # Skip completely static frozen pose frames unless it is explicitly an idle
        total_rot_motion = sum(
            qangle_deg(frames[0][1][bi], frames[fi][1][bi])
            for fi in range(1, len(frames), max(1, len(frames) // 5))
            for bi in (1, 3, 6, 8, 12, 16, 19)
        )
        baked[name] = {
            "fps": sample_fps,
            "frames": frames,
            "pelvis_drop": pelvis_drop,
            "root_speed": root_speed,
            "xy_travel": xy_travel,
            "duration": max_t,
            "seam_deg": seam_deg,
            "motion": total_rot_motion,
            "dev_arm": dev_arm,
            "dev_leg": dev_leg,
            "hand_dist": hdist_min,
            "need_blender": need_blender,
            "rejected": rejected,
        }
    return baked


def angle_diff_deg(a: float, b: float) -> float:
    d = (a - b + 180.0) % 360.0 - 180.0
    return abs(d)


def assign_8way_by_velocity(baked_model: dict, src_prefix: str):
    """Map the 8 source compass clips (n..nw) to CS2 octants (fwd..fwd_r) by measured displacement."""
    candidates = []
    for csuf in COMPASS_SUFFIXES:
        sname = f"{src_prefix}{csuf}"
        if sname not in baked_model:
            continue
        b = baked_model[sname]
        fr = b["frames"]
        dx = fr[-1][0][0] - fr[0][0][0]
        dy = fr[-1][0][1] - fr[0][0][1]
        if math.hypot(dx, dy) < 1.0:
            # Fallback to peak velocity vector
            best_i = max(range(1, len(fr)), key=lambda i: math.hypot(fr[i][0][0] - fr[0][0][0], fr[i][0][1] - fr[0][0][1]))
            dx = fr[best_i][0][0] - fr[0][0][0]
            dy = fr[best_i][0][1] - fr[0][0][1]
        ang = math.degrees(math.atan2(dy, dx))
        candidates.append((sname, csuf, ang, b))
    if len(candidates) != 8:
        return {}
    # Find minimum-angle-error 1-to-1 bijection (8! = 40,320 permutations, ~8ms)
    best_perm = None
    best_err = 1e9
    for perm in itertools.permutations(range(8)):
        err = sum(angle_diff_deg(candidates[ci][2], OCTANT_TARGETS[oi][1]) for oi, ci in enumerate(perm))
        if err < best_err:
            best_err = err
            best_perm = perm
    out = {}
    for oi, ci in enumerate(best_perm):
        oct_suf, oct_deg = OCTANT_TARGETS[oi]
        sname, _csuf, meas_deg, b = candidates[ci]
        out[oct_suf] = (sname, meas_deg, oct_deg, b)
    return out


def classify_auto_clip(name: str, b: dict):
    low = name.lower()
    # Weapon class
    if any(k in low for k in ("pistol",)):
        weapon = WEAPON_PISTOL
    elif any(k in low for k in ("grenade_throw", "grenade_place")):
        weapon = WEAPON_GRENADE
    elif any(k in low for k in ("baton", "carry_", "pound_door", "poke_ground", "push_button", "cower", "prod_player", "push_player")):
        weapon = WEAPON_NONE
    else:
        weapon = WEAPON_RIFLE

    # Category
    if any(k in low for k in ("crouch", "duck", "lowcover", "roll_from_cover")):
        cat = CAT_CROUCH
    elif any(k in low for k in ("flinch", "wince", "panic", "on_fire", "choked", "shock", "stagger", "stun", "boom_", "hit_player")):
        cat = CAT_REACTION
    elif any(k in low for k in ("shoot", "firing", "fire", "reload", "suppressor_", "melee", "baton_swing", "baton_thrust", "shield_runattack", "shield_deploy", "sidecover", "activate_baton", "draw", "idle_to_firing", "firing_to_combatidle")):
        cat = CAT_WEAPON
    elif any(k in low for k in ("signal", "holler", "point_player", "pound_door", "poke_ground", "push_button", "prod_player", "push_player")):
        cat = CAT_GESTURE
    elif any(k in low for k in ("idle", "crouch_pose", "cower", "plaza_caution")):
        cat = CAT_IDLE
    elif any(k in low for k in ("walk", "run", "sprint", "limp", "strafe", "turn", "patrol", "rappel")):
        cat = CAT_LOCOMOTION
    elif any(k in low for k in ("rubble_slide", "jump_down", "entrance", "dropship", "blockentry", "buttonroutine", "weapon_pickup")):
        cat = CAT_CINEMATIC
    else:
        cat = CAT_WEAPON if "combat" in low else CAT_CINEMATIC

    # Loopability: loops cleanly when seam <= 25 deg AND named as cycle/loop/idle/walk/run/sprint/limp
    # Exclude non-cyclic transitions/reactions/gestures (flinch_run, stand_walk_stand, gestures).
    wants_loop = any(k in low for k in ("loop", "cycle", "idle", "walk", "run", "sprint", "limp", "rappel_loop"))
    if any(k in low for k in ("_to_", "_start", "_stop", "_exit", "entrance", "attack", "reload", "throw", "flinch", "stand_walk_stand", "_gest", "gesture")):
        wants_loop = False
    step_med = float(b.get("step_med", 0.0))
    step_end = float(b.get("step_end", 0.0))
    vel_ok = (step_med <= 1.5) or (0.35 <= (step_end / max(step_med, 1e-4)) <= 2.5)
    loopable = wants_loop and b["seam_deg"] <= 25.0 and vel_ok

    return cat, weapon, loopable


def nice_display_name(raw_name: str, model_key: str) -> str:
    prefix_map = {"grunt": "Grunt", "heavy": "Heavy", "suppressor": "Suppressor", "metrocop": "Metrocop"}
    s = raw_name
    for p in ("com_sol_hev_sg_", "com_sol_hev_", "com_hev_aim_sg_", "com_hev_baton_sg_", "com_hev_", "com_heav_", "com_grunt_", "combinesoldier_custom_entrance_", "combinesoldier_infestation_", "com_cap_signal_fwd_"):
        if s.lower().startswith(p):
            s = s[len(p):]
            break
    s = s.replace("_", " ").strip()
    words = []
    for w in s.split():
        wl = w.lower()
        if wl == "sg":
            words.append("Shotgun")
        elif wl == "smg1":
            words.append("SMG")
        elif wl == "cw":
            words.append("Cover")
        elif wl in ("n", "ne", "e", "se", "s", "sw", "w", "nw"):
            words.append(wl.upper())
        elif wl == "all":
            continue
        else:
            words.append(w.capitalize())
    label = f"{prefix_map.get(model_key, '')}: {' '.join(words)}".strip(": ")
    return label[:63]


def trim_frames(b: dict, max_dur_sec: float):
    fps = b["fps"]
    keep_n = max(2, min(len(b["frames"]), int(round(max_dur_sec * fps)) + 1))
    fr = b["frames"][:keep_n]
    dur = (len(fr) - 1) / fps
    path_len = sum(math.hypot(fb[0][0] - fa[0][0], fb[0][1] - fa[0][1]) for fa, fb in zip(fr, fr[1:]))
    return {
        "fps": fps,
        "frames": fr,
        "pelvis_drop": fr[0][0][2] - min(f[0][2] for f in fr),
        "root_speed": path_len / max(dur, 1e-4),
        "xy_travel": max(math.hypot(f[0][0] - fr[0][0][0], f[0][1] - fr[0][0][1]) for f in fr),
        "duration": dur,
        "seam_deg": seam_angle_deg(fr),
        "motion": b["motion"],
    }


def find_blender(explicit: str) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.exists() else None
    env_b = os.environ.get("SOURCEMVM_BLENDER")
    if env_b and Path(env_b).exists():
        return Path(env_b)
    which = shutil.which("blender")
    if which:
        return Path(which)
    candidates = [
        Path(r"C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe"),
        Path(r"F:\SteamLibrary\steamapps\common\Blender\blender.exe"),
    ]
    bf = Path(r"C:\Program Files\Blender Foundation")
    if bf.exists():
        candidates.extend(sorted(bf.glob("Blender */blender.exe"), reverse=True))
    return next((c for c in candidates if c.exists()), None)


def strip_glb_for_blender(in_glb: Path, out_glb: Path, anim_filter_fn, extra_anim_from_glb=None):
    """Build a minimal armature+animations GLB (no meshes/textures) so Blender loads 100+ clips in <1s."""
    data = in_glb.read_bytes()
    clen = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20 : 20 + clen].decode("utf-8"))
    blen = struct.unpack_from("<I", data, 20 + clen)[0]
    bin_buf = data[28 + clen : 28 + clen + blen]

    base_names = {"combatidle", "com_heav_stand_idle_aiming_01", "idle_rifle_up", "smg1_combat_idle01", "pistol_combat_idle01", "baton_combat_idle01"}
    anims = []
    for a in gltf.get("animations", []):
        nm = a.get("name", "")
        max_t = 0.0
        for ch in a.get("channels", []):
            acc = gltf["accessors"][a["samplers"][ch["sampler"]]["input"]]
            if "max" in acc and acc["max"]:
                max_t = max(max_t, float(acc["max"][0]))
        if nm in base_names or (max_t > 1e-4 and anim_filter_fn(nm, max_t)):
            anims.append(dict(a))

    used_acc = {}
    new_bvs = []
    new_accs = []
    new_bin = bytearray()

    def copy_acc(old_idx):
        if old_idx in used_acc:
            return used_acc[old_idx]
        a = dict(gltf["accessors"][old_idx])
        bv = gltf["bufferViews"][a["bufferView"]]
        off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
        cnt = a["count"]
        stride = bv.get("byteStride", ncomp * 4)
        elem_sz = ncomp * 4
        while len(new_bin) % 4:
            new_bin.append(0)
        bv_off = len(new_bin)
        if stride == elem_sz:
            new_bin.extend(bin_buf[off : off + cnt * elem_sz])
        else:
            for k in range(cnt):
                new_bin.extend(bin_buf[off + k * stride : off + k * stride + elem_sz])
        bv_i = len(new_bvs)
        new_bvs.append({"buffer": 0, "byteOffset": bv_off, "byteLength": len(new_bin) - bv_off})
        a["bufferView"] = bv_i
        a.pop("byteOffset", None)
        idx = len(new_accs)
        new_accs.append(a)
        used_acc[old_idx] = idx
        return idx

    for a in anims:
        a["samplers"] = [dict(s) for s in a.get("samplers", [])]
        for s in a["samplers"]:
            s["input"] = copy_acc(s["input"])
            s["output"] = copy_acc(s["output"])

    # Borrow combatidle from combine_grunt.glb when a sibling GLB lacks a rifle combat idle
    nodes = [dict(n) for n in gltf.get("nodes", [])]
    if extra_anim_from_glb is not None and not any(a.get("name") == "combatidle" for a in anims):
        name_to_idx = {n.get("name", ""): i for i, n in enumerate(nodes)}
        ed = extra_anim_from_glb.read_bytes()
        ecl = struct.unpack_from("<I", ed, 12)[0]
        eg = json.loads(ed[20 : 20 + ecl].decode("utf-8"))
        ebl = struct.unpack_from("<I", ed, 20 + ecl)[0]
        ebuf = ed[28 + ecl : 28 + ecl + ebl]
        enodes = eg.get("nodes", [])
        e_ci = next((a for a in eg.get("animations", []) if a.get("name") == "combatidle"), None)
        if e_ci:
            e_acc_map = {}
            def copy_e_acc(old_idx):
                if old_idx in e_acc_map:
                    return e_acc_map[old_idx]
                a = dict(eg["accessors"][old_idx])
                bv = eg["bufferViews"][a["bufferView"]]
                off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
                ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
                cnt = a["count"]
                stride = bv.get("byteStride", ncomp * 4)
                elem_sz = ncomp * 4
                while len(new_bin) % 4:
                    new_bin.append(0)
                bv_off = len(new_bin)
                if stride == elem_sz:
                    new_bin.extend(ebuf[off : off + cnt * elem_sz])
                else:
                    for k in range(cnt):
                        new_bin.extend(ebuf[off + k * stride : off + k * stride + elem_sz])
                bv_i = len(new_bvs)
                new_bvs.append({"buffer": 0, "byteOffset": bv_off, "byteLength": len(new_bin) - bv_off})
                a["bufferView"] = bv_i
                a.pop("byteOffset", None)
                idx = len(new_accs)
                new_accs.append(a)
                e_acc_map[old_idx] = idx
                return idx
            new_ch = []
            new_smp = []
            for ch in e_ci.get("channels", []):
                eni = ch["target"].get("node")
                if eni is None:
                    continue
                bname = enodes[eni].get("name", "")
                if bname not in name_to_idx:
                    continue
                osmp = dict(e_ci["samplers"][ch["sampler"]])
                osmp["input"] = copy_e_acc(osmp["input"])
                osmp["output"] = copy_e_acc(osmp["output"])
                sidx = len(new_smp)
                new_smp.append(osmp)
                nch = dict(ch)
                nch["sampler"] = sidx
                nch["target"] = dict(ch["target"])
                nch["target"]["node"] = name_to_idx[bname]
                new_ch.append(nch)
            if new_ch:
                anims.append({"name": "combatidle", "channels": new_ch, "samplers": new_smp})

    new_skins = []
    if gltf.get("skins"):
        sk = dict(gltf["skins"][0])
        if "inverseBindMatrices" in sk:
            sk["inverseBindMatrices"] = copy_acc(sk["inverseBindMatrices"])
        new_skins.append(sk)

    def append_acc(raw, count, atype, ct=5126, extra=None):
        while len(new_bin) % 4:
            new_bin.append(0)
        off = len(new_bin)
        new_bin.extend(raw)
        bv_i = len(new_bvs)
        new_bvs.append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
        d = {"bufferView": bv_i, "componentType": ct, "count": count, "type": atype}
        if extra:
            d.update(extra)
        idx = len(new_accs)
        new_accs.append(d)
        return idx

    ap = append_acc(struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0), 3, "VEC3", 5126, {"min": [0, 0, 0], "max": [1, 1, 0]})
    aj = append_acc(struct.pack("<12H", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 3, "VEC4", 5123)
    aw = append_acc(struct.pack("<12f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0), 3, "VEC4", 5126)

    mo = None
    for idx, n in enumerate(nodes):
        if "skin" in n and mo is None:
            mo = idx
            n["mesh"] = 0
            n["skin"] = 0
        else:
            n.pop("mesh", None)
            n.pop("skin", None)
    scenes = gltf.get("scenes", [{"nodes": [0]}])
    if mo is None and new_skins:
        mo = len(nodes)
        nodes.append({"name": "dummy_skin_mesh", "mesh": 0, "skin": 0})
        scenes = list(scenes)
        scenes[0] = dict(scenes[0])
        scenes[0]["nodes"] = list(scenes[0]["nodes"]) + [mo]

    slim = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": scenes,
        "nodes": nodes,
        "skins": new_skins,
        "meshes": [{"primitives": [{"attributes": {"POSITION": ap, "JOINTS_0": aj, "WEIGHTS_0": aw}}]}],
        "animations": anims,
        "accessors": new_accs,
        "bufferViews": new_bvs,
        "buffers": [{"byteLength": len(new_bin)}],
    }
    jb = json.dumps(slim, separators=(",", ":")).encode("utf-8")
    while len(jb) % 4:
        jb += b" "
    while len(new_bin) % 4:
        new_bin.append(0)
    tot = 12 + 8 + len(jb) + 8 + len(new_bin)
    out_glb.write_bytes(
        struct.pack("<III", 0x46546C67, 2, tot)
        + struct.pack("<II", len(jb), 0x4E4F534A)
        + jb
        + struct.pack("<II", len(new_bin), 0x004E4942)
        + bytes(new_bin)
    )
    return len(anims)


def write_cs2_bind_json(bind, out_json: Path):
    joints_src = ROOT / "FX" / "sources" / "brutal-deaths" / "cs2-agent-bind.json"
    joints = []
    if joints_src.exists():
        joints = json.loads(joints_src.read_text(encoding="utf-8")).get("joints", [])
    payload = {
        "bones": [
            {"name": CORE_BONES[i][0], "pos": list(bind[i][0]), "quat": list(bind[i][1])}
            for i in range(len(CORE_BONES))
        ],
        "joints": joints,
    }
    out_json.write_text(json.dumps(payload), encoding="utf-8")


def bake_with_blender(blender_exe: Path, glb_path: Path, anim_filter_fn, td: Path, grunt_glb: Path | None, cs2_bind_json: Path):
    slim_glb = td / f"{glb_path.stem}_slim.glb"
    out_pkl = td / f"{glb_path.stem}_clean.pkl"
    n_kept = strip_glb_for_blender(glb_path, slim_glb, anim_filter_fn, extra_anim_from_glb=grunt_glb)
    clean_script = Path(__file__).resolve().parent / "blender-clean-alyx-anims.py"
    cmd = [
        str(blender_exe), "--factory-startup", "--background",
        "--python", str(clean_script), "--",
        "--glb", str(slim_glb), "--out-pkl", str(out_pkl), "--cs2-bind", str(cs2_bind_json),
    ]
    print(f"  Running Blender on {glb_path.stem} ({n_kept} clips, {slim_glb.stat().st_size / (1024*1024):.1f} MiB)...")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0 or not out_pkl.exists():
        raise RuntimeError(f"Blender cleanup failed for {glb_path.name}:\n{res.stdout[-600:]}\n{res.stderr[-600:]}")
    for line in res.stdout.splitlines():
        if "[blender-clean]" in line:
            print(" ", line)
    return pickle.loads(out_pkl.read_bytes())


def write_mvmanim(out_path: Path, entries, bind):
    """entries: list of (clean_id, display_name, cat, loopable, weapon, override_root_speed_or_None, clip_data_dict)"""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    buf = bytearray()
    buf += b"MVMACT02"
    buf += struct.pack("<III", 2, len(CORE_BONES), len(entries))
    for cs2_name, _ in CORE_BONES:
        raw = cs2_name.encode("ascii")[:23]
        buf += raw + b"\x00" * (24 - len(raw))
    for pos, quat in bind:
        buf += struct.pack("<3f", *pos)
        buf += struct.pack("<4f", *quat)

    for clean_id, display_name, cat, loopable, weapon, ov_speed, b in entries:
        frames = list(b["frames"])
        if loopable and seam_angle_deg(frames) > 0.3:
            frames = close_loop_seam(frames)
        flags = (1 if loopable else 0) | (2 if b["xy_travel"] > 18.0 else 0)
        root_spd = float(ov_speed if ov_speed is not None else b["root_speed"])
        id_raw = clean_id.encode("ascii", "replace")[:63]
        disp_raw = display_name.encode("ascii", "replace")[:63]
        buf += id_raw + b"\x00" * (64 - len(id_raw))
        buf += disp_raw + b"\x00" * (64 - len(disp_raw))
        buf += struct.pack("<BBBB f I f f", cat, flags, weapon, 0, float(b["fps"]), len(frames), float(b["pelvis_drop"]), root_spd)
        for p_delta, r_deltas, dirs, lift in frames:
            buf += struct.pack("<3f", *p_delta)
            buf += struct.pack("<f", lift)
            for q in r_deltas:
                buf += struct.pack("<4f", *q)
            for d in dirs:
                buf += struct.pack("<3f", *d)

    out_path.write_bytes(buf)
    return len(entries), len(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hla-root", default=DEFAULT_HLA)
    ap.add_argument("--vrf-cli", default=DEFAULT_VRF)
    ap.add_argument("--cs2-root", default=os.environ.get("SOURCEMVM_CS2_ROOT") or "",
                    help="CS2 install folder (default: found through Steam)")
    ap.add_argument("--cache-dir", default="",
                    help="Optional folder with pre-exported HL:A model GLBs (avoids re-exporting)")
    ap.add_argument("--blender", default="",
                    help="Path to Blender executable for additive composition & cleanup (auto-detected)")
    ap.add_argument("--no-blender", action="store_true",
                    help="Disable Blender cleanup pass (debug fallback)")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--deaths-out", default=str(DEFAULT_DEATHS_OUT))
    ap.add_argument("--merge-deaths", default=str(ROOT / "FX" / "death-animations" / "ragdoll_animations.mvmanim"),
                    help="Merge verified HL:A deaths (source=3) into existing ragdoll_animations.mvmanim if present")
    args = ap.parse_args()

    vrf_cli = Path(args.vrf_cli)
    hla_vpk = Path(args.hla_root) / "game" / "hlvr" / "pak01_dir.vpk"
    if not hla_vpk.exists():
        raise SystemExit(f"Half-Life: Alyx pak01_dir.vpk not found at {hla_vpk}")

    cs2_root = args.cs2_root
    if not cs2_root:
        sys.path.insert(0, str(ROOT / "Automation" / "lib"))
        import steam_paths
        cs2_root = str(steam_paths.cs2_install_dirs()[0])
    cs2_vpk = Path(cs2_root) / "game" / "csgo" / "pak01_dir.vpk"
    if not cs2_vpk.exists():
        raise SystemExit(f"CS2 pak01_dir.vpk not found at {cs2_vpk}")
    if not vrf_cli.exists():
        raise SystemExit(f"Source2Viewer-CLI not found at {vrf_cli}")

    keep_exact = set([c[0] for c in CURATED_ACTOR_CLIPS] + DEATH_CANDIDATES)
    for _ip, _dp, _mk, sp, _cat, _spd, _wp in DIRECTIONAL_8WAY_SETS:
        for csuf in COMPASS_SUFFIXES:
            keep_exact.add(f"{sp}{csuf}")
    for csuf in COMPASS_SUFFIXES:
        keep_exact.add(f"com_sol_hev_sg_walk_stop_{csuf}")

    baked_by_model = {}
    blender_exe = None if args.no_blender else find_blender(args.blender)
    if blender_exe:
        print(f"Using Blender cleanup pipeline: {blender_exe}")
    elif not args.no_blender:
        print("WARN: Blender executable not found; falling back to raw glTF bake")

    with tempfile.TemporaryDirectory(prefix="hla_anims_") as td:
        cache_dir = Path(args.cache_dir) if args.cache_dir else Path(td)
        cache_dir.mkdir(parents=True, exist_ok=True)
        cs2_glb = Path(td) / "cs2_agent.glb"
        export_cs2_bind_glb(vrf_cli, cs2_vpk, cs2_glb)
        bind = cs2_bind_pose(cs2_glb)
        cs2_bind_json = Path(td) / "cs2_bind.json"
        write_cs2_bind_json(bind, cs2_bind_json)

        from concurrent.futures import ThreadPoolExecutor
        model_glbs = {}
        for mkey, vmdl in HLA_ENEMY_MODELS:
            stem = Path(vmdl).stem
            glb_path = cache_dir / f"{stem}.glb"
            if not glb_path.exists():
                print(f"Exporting {stem} animations via Source2Viewer-CLI...")
                export_model_glb(vrf_cli, hla_vpk, vmdl, glb_path)
            else:
                print(f"Using cached GLB: {glb_path}")
            model_glbs[mkey] = glb_path
        grunt_glb = model_glbs.get("grunt")

        def process_one(mkey, glb_path):
            stem = glb_path.stem
            filter_fn = lambda nm, dur, ke=keep_exact: is_clean_enemy_anim(nm, dur, ke)
            print(f"Baking clean animations from {stem}...")
            mbaked = bake_clips(glb_path, filter_fn)
            if blender_exe and mbaked:
                blender_targets = set(mbaked.keys())
                bl_res = bake_with_blender(
                    blender_exe,
                    glb_path,
                    lambda nm, dur, tg=blender_targets: nm in tg,
                    Path(td),
                    grunt_glb if mkey != "grunt" else None,
                    cs2_bind_json,
                )
                for nm, bv in bl_res.items():
                    if nm in mbaked:
                        mbaked[nm] = bv
            print(f"  baked {len(mbaked)} candidate clips from {stem}")
            return mkey, mbaked

        if blender_exe:
            with ThreadPoolExecutor(max_workers=4) as pool:
                futs = [pool.submit(process_one, mk, model_glbs[mk]) for mk, _ in HLA_ENEMY_MODELS]
                for fut in futs:
                    mk, mb = fut.result()
                    baked_by_model[mk] = mb
        else:
            for mk, _ in HLA_ENEMY_MODELS:
                mk, mb = process_one(mk, model_glbs[mk])
                baked_by_model[mk] = mb

    entries = []
    used_ids = set()
    used_raw_per_model = {k: set() for k, _ in HLA_ENEMY_MODELS}

    def add_entry(clean_id, disp, cat, loopable, weapon, ov_speed, clip_data):
        if clean_id in used_ids:
            return
        used_ids.add(clean_id)
        entries.append((clean_id, disp, cat, loopable, weapon, ov_speed, clip_data))

    # 1. Native 0.0-deg-seam 8-way directional locomotion sets (oriented strictly by CS2 velocity vector)
    print("\n--- 8-Way Seamless Directional Locomotion Verification ---")
    for id_pre, disp_pre, mkey, src_pre, cat, uni_speed, wpn in DIRECTIONAL_8WAY_SETS:
        assigned = assign_8way_by_velocity(baked_by_model[mkey], src_pre)
        for oct_suf, _oct_deg in OCTANT_TARGETS:
            if oct_suf not in assigned:
                continue
            sname, meas_deg, want_deg, b = assigned[oct_suf]
            seam0 = seam_angle_deg(b["frames"])
            cid = f"{id_pre}_{oct_suf}"
            dname = f"{disp_pre} {OCTANT_LABELS[oct_suf]}"
            # Keep actual measured HL:A stride speed in clip.rootSpeed (sync scales smoothly vs uniform family maxspeed)
            add_entry(cid, dname, cat, True, wpn, None, b)
            used_raw_per_model[mkey].add(sname)
            print(f"  {cid:26s} <- {sname:38s} | ang={meas_deg:+6.1f}deg (want {want_deg:+6.1f}) realSpd={b['root_speed']:5.1f} familySpd={uni_speed:5.1f} seam={seam0:4.2f}deg")

    # 2. 4-way walk stop deceleration transitions (com_sol_hev_sg_walk_stop_*)
    heavy = baked_by_model["heavy"]
    stop_candidates = {}
    for csuf in ("n", "s", "e", "w"):
        sname = f"com_sol_hev_sg_walk_stop_{csuf}"
        if sname not in heavy:
            continue
        fr = heavy[sname]["frames"]
        k = min(len(fr) - 1, 10)
        dx = fr[k][0][0] - fr[0][0][0]
        dy = fr[k][0][1] - fr[0][0][1]
        stop_candidates[sname] = (math.degrees(math.atan2(dy, dx)), heavy[sname])
    stop_targets = [("fwd", 0.0, "Walk Stop (Forward)"), ("back", 180.0, "Walk Stop (Backward)"),
                    ("l", 90.0, "Walk Stop (Left)"), ("r", -90.0, "Walk Stop (Right)")]
    avail_stops = list(stop_candidates.keys())
    for ssuf, want_deg, disp in stop_targets:
        if not avail_stops:
            break
        best_s = min(avail_stops, key=lambda s: angle_diff_deg(stop_candidates[s][0], want_deg))
        avail_stops.remove(best_s)
        meas_deg, sb = stop_candidates[best_s]
        trimmed = trim_frames(sb, 0.65)
        add_entry(f"hla_walk_stop_{ssuf}", disp, CAT_LOCOMOTION, False, WEAPON_RIFLE, 0.0, trimmed)
        used_raw_per_model["heavy"].add(best_s)
        print(f"  hla_walk_stop_{ssuf:12s} <- {best_s:38s} | ang={meas_deg:+6.1f}deg (want {want_deg:+6.1f}) dur={trimmed['duration']:.2f}s")

    # 3. Curated non-directional combine_grunt library (preserves every existing ID)
    grunt = baked_by_model["grunt"]
    for vmdl_name, clean_id, display_name, cat, loopable in CURATED_ACTOR_CLIPS:
        if vmdl_name not in grunt:
            continue
        b = grunt[vmdl_name]
        if b.get("rejected"):
            print(f"  SKIP rejected curated clip: {clean_id} ({vmdl_name})")
            continue
        if b.get("composed"):
            print(f"  Blender composed ({b['composed']}): {clean_id} ({vmdl_name}) devArm={b.get('dev_arm',0):.1f} hDist={b.get('hand_dist',0):.1f}in")
        w = WEAPON_OVERRIDE.get(clean_id, WEAPON_RIFLE)
        eff_loop = loopable and (seam_angle_deg(b["frames"]) <= 35.0)
        add_entry(clean_id, display_name, cat, eff_loop, w, None, b)
        used_raw_per_model["grunt"].add(vmdl_name)

    # 4. Auto-extract ALL remaining unique clean enemy animations across all 4 models
    # Map existing grunt raw names so shared aliases on Heavy/Suppressor/Metrocop are not duplicated unless distinct
    already_seen_raw = set(used_raw_per_model["grunt"])
    model_clip_counts = {"directional_and_curated": len(entries)}
    skipped_rejected = 0
    death_raw_set = set(DEATH_CANDIDATES)
    for mkey, _ in HLA_ENEMY_MODELS:
        mbaked = baked_by_model[mkey]
        added_here = 0
        for raw_name in sorted(mbaked.keys()):
            if raw_name in used_raw_per_model[mkey] or raw_name in death_raw_set:
                continue
            rlow = raw_name.lower()
            if "death" in rlow:
                continue
            if mkey in ("heavy", "suppressor") and raw_name in already_seen_raw:
                continue
            b = mbaked[raw_name]
            if b.get("rejected"):
                skipped_rejected += 1
                continue
            cat, weapon, loopable = classify_auto_clip(raw_name, b)
            # Exclude deep collapse/death falls unless idle/crouch/cinematic slide
            if b["pelvis_drop"] >= 16.0 and cat not in (CAT_CROUCH, CAT_CINEMATIC):
                continue
            # Exclude completely frozen non-idle frames
            if cat != CAT_IDLE and b["motion"] < 8.0 and b["xy_travel"] < 2.0:
                continue
            base_slug = re.sub(r"[^a-z0-9_]+", "_", raw_name.lower()).strip("_")
            cid = f"hla_{base_slug}" if mkey == "grunt" else f"hla_{mkey[:3]}_{base_slug}"
            cid = cid[:63]
            if cid in used_ids:
                continue
            if b.get("composed"):
                print(f"  Blender composed ({b['composed']}): {cid} devArm={b.get('dev_arm',0):.1f} hDist={b.get('hand_dist',0):.1f}in")
            disp = nice_display_name(raw_name, mkey)
            add_entry(cid, disp, cat, loopable, weapon, None, b)
            used_raw_per_model[mkey].add(raw_name)
            already_seen_raw.add(raw_name)
            added_here += 1
        model_clip_counts[mkey] = added_here
    print(f"Rejected broken/T-pose clips filtered out: {skipped_rejected}")

    out_path = Path(args.out)
    n_clips, n_bytes = write_mvmanim(out_path, entries, bind)
    print(f"\nPer-source clip summary: {model_clip_counts}")
    print(f"Wrote {n_clips} total HL:A actor clips to {out_path} ({n_bytes / (1024*1024):.2f} MiB)")

    # Preserve HL:A death animation export from combine_grunt
    usable_deaths = []
    for dname in DEATH_CANDIDATES:
        if dname not in grunt:
            continue
        b = grunt[dname]
        if b["pelvis_drop"] >= 12.0:
            usable_deaths.append({
                "name": f"hla_{dname}",
                "source_anim": dname,
                "duration": round(b["duration"], 3),
                "pelvis_drop_in": round(b["pelvis_drop"], 2),
                "frames": len(b["frames"]),
            })
    deaths_out = Path(args.deaths_out)
    deaths_out.parent.mkdir(parents=True, exist_ok=True)
    deaths_out.write_text(json.dumps({"model": "models/characters/combine_grunt/combine_grunt.vmdl_c", "deaths": usable_deaths}, indent=2))
    print(f"Verified {len(usable_deaths)}/{len(DEATH_CANDIDATES)} usable full-body HL:A death animations -> {deaths_out}")

    merge_path = Path(args.merge_deaths)
    if args.merge_deaths and merge_path.exists() and usable_deaths:
        merge_deaths_into_pack(merge_path, usable_deaths, grunt)


# Order of 15 CS2 ragdoll bodies in MVMDANIM v9 (matches DeathAnimationsMath.h)
RAGDOLL_BODY_MAP = [
    ("leg_upper_l", "leg_upper_L"),
    ("leg_lower_l", "leg_lower_L"),
    ("leg_upper_r", "leg_upper_R"),
    ("leg_lower_r", "leg_lower_R"),
    ("head_0", "head_0"),
    ("pelvis", "pelvis"),
    ("spine_2", "spine_2"),
    ("arm_upper_l", "arm_upper_L"),
    ("arm_lower_l", "arm_lower_L"),
    ("arm_upper_r", "arm_upper_R"),
    ("arm_lower_r", "arm_lower_R"),
    ("hand_l", "hand_L"),
    ("hand_r", "hand_R"),
    ("ankle_l", "ankle_L"),
    ("ankle_r", "ankle_R"),
]


def merge_deaths_into_pack(pack_path: Path, usable_deaths, baked):
    import importlib.util
    spec = importlib.util.spec_from_file_location("eda", ROOT / "FX" / "tools" / "extract-death-anims.py")
    eda = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(eda)

    raw = pack_path.read_bytes()
    magic = raw[:8]
    ver, clip_count, body_count, fps, span = struct.unpack_from("<IIIff", raw, 8)
    if magic != b"MVMDANIM" or ver != 9 or body_count != 15:
        print(f"Skipping death merge: unsupported pack header ({magic!r} v{ver} bodies={body_count})")
        return

    entry_size = 32 + 28 + 15 * 12  # 240 bytes per clip table entry
    table_off = 28
    payload_off = table_off + clip_count * entry_size

    # Keep all existing non-Alyx clips (source != 3) so re-running is idempotent
    existing = []
    for i in range(clip_count):
        off = table_off + i * entry_size
        name = raw[off : off + 32].split(b"\0", 1)[0].decode("ascii", "replace")
        src, cat, frames, byte_off, side, stance, weapon = struct.unpack_from("<IIIIIII", raw, off + 32)
        if src == 3 or name.startswith("hla_"):
            continue
        base = [struct.unpack_from("<3f", raw, off + 60 + b * 12) for b in range(15)]
        fbytes = raw[payload_off + byte_off : payload_off + byte_off + frames * 15 * 28]
        existing.append((name, src, cat, frames, side, stance, weapon, base, fbytes))

    # Map CORE_BONES index -> CS2 ragdoll 15-body index
    core_by_cs2 = {cs2.lower(): idx for idx, (cs2, _) in enumerate(CORE_BONES)}
    rag_core_idx = [core_by_cs2[cs2.lower()] for cs2, _ in RAGDOLL_BODY_MAP]

    ref = eda.cs2_reference()
    joints = eda.cs2_joints()
    ref_quats = [eda.matrix_quat(ref[i]["bind"]) for i in range(15)]
    ref_pelvis = (ref[5]["bind"][3], ref[5]["bind"][7], ref[5]["bind"][11])
    ankle_height = min(ref[13]["bind"][11], ref[14]["bind"][11])

    added = 0
    for d in usable_deaths:
        b = baked[d["source_anim"]]
        clip_name = d["name"][:31]
        base = None
        root_offset = None
        frame_bytes = bytearray()
        for p_delta, r_deltas, *_extra in b["frames"]:
            target_rots = [qnorm(qmul(r_deltas[rag_core_idx[i]], ref_quats[i])) for i in range(15)]
            pelvis_pos = (ref_pelvis[0] + p_delta[0], ref_pelvis[1] + p_delta[1], ref_pelvis[2] + p_delta[2])
            target_pos = eda.articulated_positions(pelvis_pos, target_rots, joints)
            if root_offset is None:
                root_offset = (
                    -target_pos[5][0],
                    -target_pos[5][1],
                    ankle_height - min(target_pos[13][2], target_pos[14][2]),
                )
            target_pos = [tuple(p[k] + root_offset[k] for k in range(3)) for p in target_pos]
            if base is None:
                base = target_pos
            for i in range(15):
                dp = (target_pos[i][0] - base[i][0], target_pos[i][1] - base[i][1], target_pos[i][2] - base[i][2])
                frame_bytes += struct.pack("<7f", dp[0], dp[1], dp[2], *target_rots[i])

        low = d["source_anim"].lower()
        cat = 8 if "explosion" in low else (5 if "shoulder" in low else (3 if "rear" in low else 0))
        side = 2 if "rear" in low else 0  # 2 = back, 0 = any
        stance = 0  # standing
        weapon = 0  # universal
        existing.append((clip_name, 3, cat, len(b["frames"]), side, stance, weapon, base, bytes(frame_bytes)))
        added += 1

    table = bytearray()
    payload = bytearray()
    for name, src, cat, frames, side, stance, weapon, base, fbytes in existing:
        table += name.encode("ascii")[:31].ljust(32, b"\0")
        table += struct.pack("<IIIIIII", src, cat, frames, len(payload), side, stance, weapon)
        for pos in base:
            table += struct.pack("<3f", *pos)
        payload += fbytes

    header = b"MVMDANIM" + struct.pack("<IIIff", 9, len(existing), 15, fps, span)
    pack_path.write_bytes(header + table + payload)
    print(f"Merged {added} HL:A death animations into {pack_path} (total {len(existing)} clips)")


if __name__ == "__main__":
    main()
