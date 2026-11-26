#!/usr/bin/env python3
"""Clean, retarget (calibrated offsets + CS2 knee hinge fit), fix cyclic loops, and compose HL:A clips in headless Blender.

Called by FX/tools/extract-alyx-anims.py with a stripped skeleton+animations GLB
(referencing a minimal 1-triangle skinned mesh so Blender builds a true Armature
with PoseBones in ~0.3s per model) and a CS2 bind/joint JSON file.
"""

import argparse
import json
import math
import pickle
import sys
from pathlib import Path

import bpy
import mathutils


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

CORE_PARENT = [
    -1, 0, 1, 2, 3, 4, 5,
    4, 7, 8, 9,
    4, 11, 12, 13,
    0, 15, 16,
    0, 18, 19,
] + [-1] * 30
CORE_CHILD = [-1, -1, -1, -1, -1, -1, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1, 19, 20, -1] + [-1] * 30
CORE_L_HIP, CORE_R_HIP, CORE_ANKLES = 15, 18, (17, 20)
CORE_UPPER_MASK = [False] + [True] * 14 + [False] * 6 + [True] * 30

# Blender glTF armature: -Y is forward, +X is left, +Z is up. Rotate +90 deg about +Z -> CS2 (+X fwd, +Y left, +Z up).
M_B2S = mathutils.Matrix.Rotation(math.pi * 0.5, 4, "Z")
IN_PER_M = 39.37007874015748


def q_to_xyzw(q: mathutils.Quaternion):
    return (float(q.x), float(q.y), float(q.z), float(q.w))


def xyzw_to_q(t) -> mathutils.Quaternion:
    return mathutils.Quaternion((t[3], t[0], t[1], t[2]))


def qangle_deg(a_xyzw, b_xyzw) -> float:
    dot = abs(sum(a_xyzw[i] * b_xyzw[i] for i in range(4)))
    dot = min(1.0, max(-1.0, dot))
    return 2.0 * math.degrees(math.acos(dot))


def qdist_deg(a: mathutils.Quaternion, b: mathutils.Quaternion) -> float:
    d = abs(a.normalized().dot(b.normalized()))
    return 2.0 * math.degrees(math.acos(min(1.0, max(0.0, d))))


def vec_angle_deg(u, v) -> float:
    lu = u.length
    lv = v.length
    if lu < 1e-6 or lv < 1e-6:
        return 0.0
    d = max(-1.0, min(1.0, u.dot(v) / (lu * lv)))
    return math.degrees(math.acos(d))


def signed_angle_about(a: mathutils.Vector, b: mathutils.Vector, axis: mathutils.Vector) -> float:
    ax = axis.normalized()
    ap = a - ax * a.dot(ax)
    bp = b - ax * b.dot(ax)
    return math.atan2(ap.cross(bp).dot(ax), ap.dot(bp))


def swing_between_vecs(u: mathutils.Vector, v: mathutils.Vector) -> mathutils.Quaternion:
    if u.length < 1e-6 or v.length < 1e-6:
        return mathutils.Quaternion((1.0, 0.0, 0.0, 0.0))
    return u.normalized().rotation_difference(v.normalized())


def clear_pose(arm):
    if arm.animation_data:
        arm.animation_data.action = None
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
        pb.location = (0.0, 0.0, 0.0)
        pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)


def assign_action(arm, act):
    clear_pose(arm)
    if not arm.animation_data:
        arm.animation_data_create()
    arm.animation_data.action = act
    if hasattr(act, "slots") and act.slots:
        arm.animation_data.action_slot = act.slots[0]


def frame_set_frac(scene, f_float: float):
    fi = int(math.floor(f_float))
    sub = float(f_float - fi)
    scene.frame_set(fi, subframe=sub)
    bpy.context.view_layer.update()


def cs2_world_pose(arm, pbone):
    m = M_B2S @ arm.matrix_world @ pbone.matrix
    pos = m.to_translation() * IN_PER_M
    q = m.to_quaternion()
    return pos, q


def load_cs2_calibration(cs2_bind_json: str):
    data = json.loads(Path(cs2_bind_json).read_text(encoding="utf-8"))
    cs2_p = [mathutils.Vector(x["pos"]) for x in data["bones"]]
    cs2_q = [xyzw_to_q(x["quat"]).normalized() for x in data["bones"]]
    # Precompute CS2 knee hinge calibration from death-anim reference joints
    knee_joints = {}
    if "joints" in data:
        jmap = {j["child"]: j for j in data["joints"]}
        # In 15-body order: 0=leg_upper_l, 1=leg_lower_l, 2=leg_upper_r, 3=leg_lower_r, 13=ankle_l, 14=ankle_r
        # Map to CORE indices: 15, 16, 18, 19, 17, 20
        for body_c, core_p, core_c, core_a in ((1, 15, 16, 17), (3, 18, 19, 20)):
            if body_c not in jmap:
                continue
            jn = jmap[body_c]
            f1 = xyzw_to_q(jn["frame1"][4:8]).normalized()
            f2 = xyzw_to_q(jn["frame2"][4:8]).normalized()
            bend_ref = (cs2_p[core_c] - cs2_p[core_p]).cross(cs2_p[core_a] - cs2_p[core_c])
            hinge_ref = (cs2_q[core_p] @ f1) @ mathutils.Vector((0.0, 0.0, 1.0))
            sign = 1.0 if hinge_ref.dot(bend_ref) >= 0.0 else -1.0
            child_off = cs2_q[core_c].inverted() @ (cs2_p[core_a] - cs2_p[core_c])
            knee_joints[core_c] = (core_p, f1, f2, sign, child_off)
    return cs2_p, cs2_q, knee_joints


def fit_knee_hinges_world(target_q, pos_s, knee_joints):
    """Align CS2 thigh roll & knee hinge to HL:A hip/knee/ankle bend plane (ported from extract-death-anims.py)."""
    for core_c, (core_p, f1, f2, sign, child_off) in knee_joints.items():
        core_a = core_c + 1
        thigh = pos_s[core_c] - pos_s[core_p]
        shin = pos_s[core_a] - pos_s[core_c]
        if thigh.length < 1e-4 or shin.length < 1e-4:
            continue
        tu = thigh.normalized()
        su = shin.normalized()
        bend_deg = math.degrees(math.acos(max(-1.0, min(1.0, tu.dot(su)))))
        weight = min(1.0, max(0.0, (bend_deg - 3.0) / 9.0))
        if weight > 0.0:
            hinge = ((target_q[core_p] @ f1) @ mathutils.Vector((0.0, 0.0, 1.0))) * sign
            normal = thigh.cross(shin)
            if normal.length > 1e-5:
                if hinge.dot(normal) < 0.0:
                    normal = -normal
                roll = signed_angle_about(hinge, normal, thigh) * weight
                target_q[core_p] = (mathutils.Quaternion(tu, roll) @ target_q[core_p]).normalized()
        axis = (target_q[core_p] @ f1) @ mathutils.Vector((0.0, 0.0, 1.0))
        straight = (target_q[core_p] @ f1 @ f2.inverted()).normalized()
        angle = signed_angle_about(straight @ child_off, shin, axis)
        target_q[core_c] = (mathutils.Quaternion(axis.normalized(), angle) @ straight).normalized()


def is_candidate_loop(aname: str) -> bool:
    low = aname.lower()
    wants = any(k in low for k in ("loop", "cycle", "idle", "walk", "run", "sprint", "limp", "rappel_loop"))
    if not wants:
        return False
    bad = ("_to_", "_start", "_stop", "_exit", "entrance", "attack", "reload", "throw", "flinch", "stand_walk_stand", "_gest", "gesture")
    if any(b in low for b in bad):
        return False
    if low == "combatidle":
        return True
    return True


def is_directional_or_angled(aname: str) -> bool:
    low = aname.lower()
    if any(k in low for k in ("walk_loop_", "run_", "sprint_", "crouch_walk_", "walk_stop_", "cover", "turn", "corner", "strafe", "wince_a_", "wince_c_", "wince_")):
        return True
    if any(low.endswith(suf) for suf in ("_n", "_ne", "_e", "_se", "_s", "_sw", "_w", "_nw")):
        return True
    return False


def is_stationary_clip_name(aname: str) -> bool:
    low = aname.lower()
    return not any(k in low for k in ("walk", "run", "sprint", "limp", "strafe", "turn", "corner", "slide", "jump", "entrance", "rappel", "roll"))


def close_loop_cyclic(frames):
    """Distribute boundary error multiplicatively over ALL frames so angular velocity stays continuous."""
    n = len(frames)
    if n < 4:
        return frames
    r0 = frames[0][1]
    rn = frames[-1][1]
    nb = len(r0)
    # Compute multiplicative world error q_err = r0 * inv(rn) per bone
    q_err = []
    for bi in range(nb):
        q0 = xyzw_to_q(r0[bi])
        q1 = xyzw_to_q(rn[bi])
        if q0.dot(q1) < 0.0:
            q1.negate()
        q_err.append((q0 @ q1.inverted()).normalized())
    pz_err = frames[0][0][2] - frames[-1][0][2]
    lift_err = frames[0][3] - frames[-1][3]
    id_q = mathutils.Quaternion((1.0, 0.0, 0.0, 0.0))
    out = []
    for fi in range(n):
        u = float(fi) / float(n - 1)
        p_cur, r_cur, d_cur, l_cur = frames[fi]
        if fi == n - 1:
            p_new = (p_cur[0], p_cur[1], frames[0][0][2])
            r_new = list(frames[0][1])
            d_new = list(frames[0][2])
            l_new = frames[0][3]
        else:
            p_new = (p_cur[0], p_cur[1], p_cur[2] + u * pz_err)
            l_new = l_cur + u * lift_err
            r_new = []
            d_new = []
            for bi in range(nb):
                dq = id_q.slerp(q_err[bi], u)
                qi = (dq @ xyzw_to_q(r_cur[bi])).normalized()
                r_new.append(q_to_xyzw(qi))
                if bi < 21 and (d_cur[bi][0] != 0.0 or d_cur[bi][1] != 0.0 or d_cur[bi][2] != 0.0):
                    dv = dq @ mathutils.Vector(d_cur[bi])
                    d_new.append((float(dv.x), float(dv.y), float(dv.z)))
                else:
                    d_new.append(d_cur[bi])
        out.append((p_new, r_new, d_new, l_new))
    return out


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    ap = argparse.ArgumentParser()
    ap.add_argument("--glb", required=True)
    ap.add_argument("--out-pkl", required=True)
    ap.add_argument("--cs2-bind", required=True)
    ap.add_argument("--base", default="combatidle")
    ap.add_argument("--fallbacks", default="com_heav_stand_idle_aiming_01,smg1_combat_idle01,pistol_combat_idle01,idle_rifle_up")
    ap.add_argument("--fps", type=float, default=30.0)
    return ap.parse_args(argv)


def main():
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # Set 30fps BEFORE glTF import so keyframe timings and root speeds stay at exact 1.0x HL:A rate.
    bpy.context.scene.render.fps = 30
    bpy.ops.import_scene.gltf(filepath=args.glb)

    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if not arms:
        raise RuntimeError(f"No armature found in {args.glb}")
    arm = arms[0]
    scene = bpy.context.scene

    core_pbones = []
    for _cs2_name, hla_name in CORE_BONES:
        if hla_name not in arm.pose.bones:
            raise RuntimeError(f"Missing pose bone {hla_name} in {args.glb}")
        core_pbones.append(arm.pose.bones[hla_name])
    core_names = [pb.name for pb in core_pbones]
    upper_bone_names = {core_names[i] for i, u in enumerate(CORE_UPPER_MASK) if u}
    for pb in arm.pose.bones:
        nm = pb.name.lower()
        if any(p in nm for p in ("spine", "neck", "head", "clavicle", "arm_", "hand_", "finger_", "scap", "pect", "weapon_hand")):
            upper_bone_names.add(pb.name)

    cs2_p, cs2_q, knee_joints = load_cs2_calibration(args.cs2_bind)
    cs2_q_inv = [q.inverted() for q in cs2_q]
    # Precompute CS2 bind local bone translation offsets for ankle pinning check
    cs2_local_p = []
    for bi in range(21):
        par = CORE_PARENT[bi]
        if par >= 0:
            cs2_local_p.append(cs2_q_inv[par] @ (cs2_p[bi] - cs2_p[par]))
        else:
            cs2_local_p.append(mathutils.Vector((0.0, 0.0, 0.0)))

    # Rest world poses in CS2 coordinates
    clear_pose(arm)
    scene.frame_set(0)
    bpy.context.view_layer.update()
    rest_pos = []
    rest_q = []
    for pb in core_pbones:
        p, q = cs2_world_pose(arm, pb)
        rest_pos.append(p)
        rest_q.append(q.normalized())
    rest_pelvis = rest_pos[0]
    rest_ankle_z = min(rest_pos[a].z for a in CORE_ANKLES)
    pelvis_scale = float(cs2_p[0].z / max(rest_pelvis.z, 1e-3))

    # Calibrated rotation offsets (Death Animations retargeting: aligns hip yaw & removes rest-pose limb swing)
    donor_hips = rest_pos[CORE_L_HIP] - rest_pos[CORE_R_HIP]
    cs2_hips = cs2_p[CORE_L_HIP] - cs2_p[CORE_R_HIP]
    calib_yaw = math.atan2(cs2_hips.y, cs2_hips.x) - math.atan2(donor_hips.y, donor_hips.x)
    undo_calib_yaw = mathutils.Quaternion((0.0, 0.0, 1.0), -calib_yaw)
    rest_yaw = math.atan2(donor_hips.y, donor_hips.x) - math.pi * 0.5

    rot_offsets = []
    for bi in range(len(core_pbones)):
        calibrated = (undo_calib_yaw @ cs2_q[bi]).normalized()
        c = CORE_CHILD[bi]
        if c >= 0:
            cs2_local = cs2_q_inv[bi] @ (cs2_p[c] - cs2_p[bi])
            donor_dir = rest_pos[c] - rest_pos[bi]
            swing = swing_between_vecs(calibrated @ cs2_local, donor_dir)
            calibrated = (swing @ calibrated).normalized()
        rot_offsets.append((rest_q[bi].inverted() @ calibrated).normalized())

    actions = {a.name: a for a in bpy.data.actions}

    base_candidates = [args.base] + [x.strip() for x in args.fallbacks.split(",") if x.strip()]
    base_poses = {}
    for bname in base_candidates:
        if bname in actions:
            assign_action(arm, actions[bname])
            frame_set_frac(scene, 0.0)
            snap = {}
            for pb in arm.pose.bones:
                q = pb.rotation_quaternion.copy()
                if q.w < 0.0:
                    q.negate()
                snap[pb.name] = (q, pb.location.copy(), pb.scale.copy())
            base_poses[bname] = snap
    default_base_name = next((b for b in base_candidates if b in base_poses), None)

    def pick_base_for(anim_name: str):
        low = anim_name.lower()
        if "pistol" in low and "pistol_combat_idle01" in base_poses:
            return base_poses["pistol_combat_idle01"]
        if "smg1" in low and "smg1_combat_idle01" in base_poses:
            return base_poses["smg1_combat_idle01"]
        return base_poses.get(default_base_name) if default_base_name else None

    def measure_pose_metrics():
        pos_q = [cs2_world_pose(arm, pb) for pb in core_pbones]
        dev_arm = max(
            qdist_deg(pos_q[i][1], rest_q[i])
            for i in (8, 9, 12, 13)
        )
        dev_leg = max(
            qdist_deg(pos_q[i][1], rest_q[i])
            for i in (15, 16, 17, 18, 19, 20)
        )
        elR = vec_angle_deg(pos_q[13][0] - pos_q[12][0], pos_q[14][0] - pos_q[13][0])
        elL = vec_angle_deg(pos_q[9][0] - pos_q[8][0], pos_q[10][0] - pos_q[9][0])
        hdist = (pos_q[14][0] - pos_q[10][0]).length
        return dev_arm, dev_leg, elR, elL, hdist

    out = {}
    max_yaw_limit = math.radians(28.0)

    for aname, act in actions.items():
        r0, r1 = act.frame_range
        f_span = float(r1 - r0)
        fps_src = float(scene.render.fps)
        dur = max(0.0, f_span / fps_src)
        if dur <= 1e-4:
            continue

        assign_action(arm, act)
        raw_arm_max, raw_leg_max, raw_hdist_min = 0.0, 0.0, 999.0
        for frac in (0.0, 0.25, 0.5, 0.75, 1.0):
            frame_set_frac(scene, r0 + frac * f_span)
            da, dl, _er, _el, hd = measure_pose_metrics()
            raw_arm_max = max(raw_arm_max, da)
            raw_leg_max = max(raw_leg_max, dl)
            raw_hdist_min = min(raw_hdist_min, hd)

        base_snap = pick_base_for(aname)
        compose_mode = ""
        is_explicit_idle = "idle" in aname.lower() and "to_" not in aname.lower()
        if base_snap is not None and not is_explicit_idle:
            if raw_leg_max < 6.0 and (raw_arm_max < 42.0 or raw_hdist_min > 34.0):
                compose_mode = "full"
            elif raw_leg_max >= 6.0 and raw_arm_max < 42.0 and raw_hdist_min > 34.0:
                compose_mode = "upper"

        # Uniform spacing across [0, dur] so dt between consecutive frames is identical and seam is exact
        target_fps = args.fps if dur <= 8.0 else 20.0
        frame_count = max(2, int(round(dur * target_fps)) + 1)
        sample_fps = float(frame_count - 1) / dur

        raw_local_frames = None
        if compose_mode:
            raw_local_frames = []
            for fi in range(frame_count):
                frac = float(fi) / float(frame_count - 1)
                frame_set_frac(scene, r0 + frac * f_span)
                snap = {}
                for pb in arm.pose.bones:
                    aq = pb.rotation_quaternion.copy()
                    if aq.w < 0.0:
                        aq.negate()
                    snap[pb.name] = (aq, pb.location.copy(), pb.scale.copy())
                raw_local_frames.append(snap)
            arm.animation_data.action = None

        frames = []
        pelvis_zs = []
        xy_travel = 0.0
        first_xy = None
        psi0 = None
        prev_psi = 0.0
        norm_yaw_q = mathutils.Quaternion((1.0, 0.0, 0.0, 0.0))
        do_norm_psi0 = not is_directional_or_angled(aname)
        post_arm_max, post_leg_max, post_hdist_min = 0.0, 0.0, 999.0
        post_el_min = 180.0
        max_back_knee = 0.0
        max_hip_twist = 0.0

        for fi in range(frame_count):
            if compose_mode:
                snap = raw_local_frames[fi]
                for pb in arm.pose.bones:
                    aq, al, asc = snap[pb.name]
                    bq, bl, _bsc = base_snap[pb.name]
                    if compose_mode == "full" or pb.name in upper_bone_names:
                        pb.rotation_quaternion = (bq @ aq).normalized()
                        pb.location = bl + al
                    else:
                        pb.rotation_quaternion = aq
                        pb.location = al
                    pb.scale = asc
                bpy.context.view_layer.update()
            else:
                frac = float(fi) / float(frame_count - 1)
                frame_set_frac(scene, r0 + frac * f_span)

            pos_s = []
            wq_s = []
            for pb in core_pbones:
                p, q = cs2_world_pose(arm, pb)
                pos_s.append(p)
                wq_s.append(q)

            if fi in (0, frame_count // 2, frame_count - 1):
                da, dl, er, el, hd = measure_pose_metrics()
                post_arm_max = max(post_arm_max, da)
                post_leg_max = max(post_leg_max, dl)
                post_hdist_min = min(post_hdist_min, hd)
                post_el_min = min(post_el_min, max(er, el))

            lx = pos_s[CORE_L_HIP].x - pos_s[CORE_R_HIP].x
            ly = pos_s[CORE_L_HIP].y - pos_s[CORE_R_HIP].y
            psi_raw = math.atan2(ly, lx) - math.pi * 0.5 - rest_yaw
            if psi0 is None:
                psi0 = math.atan2(math.sin(psi_raw), math.cos(psi_raw))
                prev_psi = psi0
                psi = psi0
                if do_norm_psi0 and abs(psi0) > math.radians(38.0):
                    norm_yaw_q = mathutils.Quaternion((0.0, 0.0, 1.0), -psi0)
            else:
                dpsi = math.atan2(math.sin(psi_raw - prev_psi), math.cos(psi_raw - prev_psi))
                psi = prev_psi + dpsi
                prev_psi = psi

            delta_yaw = psi - psi0
            if abs(delta_yaw) > max_yaw_limit:
                excess = delta_yaw - math.copysign(max_yaw_limit, delta_yaw)
                yaw_fix = (norm_yaw_q @ mathutils.Quaternion((0.0, 0.0, 1.0), -excess)).normalized()
            else:
                yaw_fix = norm_yaw_q

            p0 = pos_s[0]
            raw_dx = float((p0.x - rest_pelvis.x) * pelvis_scale)
            raw_dy = float((p0.y - rest_pelvis.y) * pelvis_scale)
            rot_d = yaw_fix @ mathutils.Vector((raw_dx, raw_dy, 0.0))
            if first_xy is None:
                first_xy = (float(rot_d.x), float(rot_d.y))
            dx = float(rot_d.x) - first_xy[0]
            dy = float(rot_d.y) - first_xy[1]
            dz = float((p0.z - rest_pelvis.z) * pelvis_scale)
            p_delta = (dx, dy, dz)
            pelvis_zs.append(float(p0.z))
            xy_travel = max(xy_travel, math.hypot(dx, dy))

            # Calibrated CS2 target world rotations + knee hinge fit
            target_q = [(yaw_fix @ (wq_s[bi] @ rot_offsets[bi])).normalized() for bi in range(len(core_pbones))]
            rot_pos_s = [yaw_fix @ pos_s[bi] for bi in range(21)]
            fit_knee_hinges_world(target_q, rot_pos_s, knee_joints)

            # Check pelvis->thigh twist and knee bend direction
            pfwd = yaw_fix @ mathutils.Vector((math.cos(psi), math.sin(psi), 0.0))
            for th_i, cf_i, an_i in ((15, 16, 17), (18, 19, 20)):
                loc_hip = target_q[0].inverted() @ target_q[th_i]
                rest_hip = cs2_q_inv[0] @ cs2_q[th_i]
                max_hip_twist = max(max_hip_twist, qdist_deg(loc_hip, rest_hip))
                v_th = (rot_pos_s[cf_i] - rot_pos_s[th_i])
                v_cf = (rot_pos_s[an_i] - rot_pos_s[cf_i])
                if v_th.length > 1e-4 and v_cf.length > 1e-4:
                    tu, cu = v_th.normalized(), v_cf.normalized()
                    bend = math.degrees(math.acos(max(-1.0, min(1.0, tu.dot(cu)))))
                    if bend > 18.0:
                        kdir = (tu - cu)
                        if kdir.length > 1e-4 and kdir.normalized().dot(pfwd) < -0.25:
                            max_back_knee = max(max_back_knee, bend)

            r_deltas = [q_to_xyzw((target_q[bi] @ cs2_q_inv[bi]).normalized()) for bi in range(len(core_pbones))]

            dirs = []
            for bi in range(len(core_pbones)):
                c = CORE_CHILD[bi]
                if c < 0:
                    dirs.append((0.0, 0.0, 0.0))
                    continue
                dv = pos_s[c] - pos_s[bi]
                if dv.length > 1e-6:
                    dr = (yaw_fix @ dv).normalized()
                    dirs.append((float(dr.x), float(dr.y), float(dr.z)))
                else:
                    dirs.append((0.0, 0.0, 0.0))

            lift = float((min(pos_s[a].z for a in CORE_ANKLES) - rest_ankle_z) * pelvis_scale)
            frames.append((p_delta, r_deltas, dirs, lift))

        n_fr = len(frames)
        n_bones = len(core_pbones)
        # 2. Standing foot-pinning on stationary clips so planted CS2 ankles never skate
        if xy_travel <= 16.0 and is_stationary_clip_name(aname) and n_fr >= 2:
            # Compute CS2 articulated ankle XY in frame 0..n-1 with raw p_delta
            ankles_xy_z = []
            for fi in range(n_fr):
                p_d, r_d, d_v, _ = frames[fi]
                wp = [None] * 21
                wq = [None] * 21
                wp[0] = cs2_p[0] + mathutils.Vector(p_d)
                wq[0] = (xyzw_to_q(r_d[0]) @ cs2_q[0]).normalized()
                for chain in ((15, 16, 17), (18, 19, 20)):
                    for bi in chain:
                        par = CORE_PARENT[bi]
                        wp[bi] = wp[par] + (wq[par] @ cs2_local_p[bi])
                        q = (xyzw_to_q(r_d[bi]) @ cs2_q[bi]).normalized()
                        c = CORE_CHILD[bi]
                        if c >= 0 and mathutils.Vector(d_v[bi]).length > 0.5:
                            bd = (cs2_p[c] - cs2_p[bi]).normalized()
                            cur_d = xyzw_to_q(r_d[bi]) @ bd
                            corr = swing_between_vecs(cur_d, mathutils.Vector(d_v[bi]))
                            q = (corr @ q).normalized()
                        wq[bi] = q
                ankles_xy_z.append((wp[17].copy(), wp[20].copy()))
            refL, refR = ankles_xy_z[0]
            pinned_frames = []
            for fi in range(n_fr):
                p_d, r_d, d_v, lf = frames[fi]
                aL, aR = ankles_xy_z[fi]
                min_z = min(aL.z, aR.z)
                wL = max(0.0, min(1.0, (min_z + 1.4 - aL.z) / 1.0))
                wR = max(0.0, min(1.0, (min_z + 1.4 - aR.z) / 1.0))
                wsum = wL + wR
                if wsum > 0.05:
                    ex = (wL * (aL.x - refL.x) + wR * (aR.x - refR.x)) / wsum
                    ey = (wL * (aL.y - refL.y) + wR * (aR.y - refR.y)) / wsum
                    em = math.hypot(ex, ey)
                    if em > 8.0:
                        ex *= 8.0 / em
                        ey *= 8.0 / em
                    p_d = (p_d[0] - ex, p_d[1] - ey, p_d[2])
                pinned_frames.append((p_d, r_d, d_v, lf))
            frames = pinned_frames
            xy_travel = max(math.hypot(f[0][0], f[0][1]) for f in frames)

        # 3. Quaternion hemisphere unwrapping
        for bi in range(n_bones):
            for fi in range(1, n_fr):
                prev_q = frames[fi - 1][1][bi]
                cur_q = frames[fi][1][bi]
                if sum(prev_q[k] * cur_q[k] for k in range(4)) < 0.0:
                    frames[fi][1][bi] = (-cur_q[0], -cur_q[1], -cur_q[2], -cur_q[3])

        raw_seam = max(qangle_deg(frames[0][1][b], frames[-1][1][b]) for b in range(21))
        cyclic_clip = is_candidate_loop(aname) and raw_seam <= 25.0

        # 4. Gentle cyclic-aware micro-jitter denoising
        if n_fr >= 5:
            smoothed_rots = [[frames[fi][1][bi] for bi in range(n_bones)] for fi in range(n_fr)]
            n_cycle = n_fr - 1 if cyclic_clip else n_fr
            start_i = 0 if cyclic_clip else 1
            end_i = n_cycle if cyclic_clip else n_fr - 1
            for fi in range(start_i, end_i):
                fa = (fi - 1) % n_cycle if cyclic_clip else (fi - 1)
                fb = (fi + 1) % n_cycle if cyclic_clip else (fi + 1)
                for bi in range(n_bones):
                    qa = frames[fa][1][bi]
                    qb = frames[fi][1][bi]
                    qc = frames[fb][1][bi]
                    step_ang = max(qangle_deg(qa, qb), qangle_deg(qb, qc))
                    if 0.35 < step_ang < 10.0:
                        mid = xyzw_to_q(qa).slerp(xyzw_to_q(qc), 0.5)
                        cur = xyzw_to_q(qb).slerp(mid, 0.28).normalized()
                        smoothed_rots[fi][bi] = q_to_xyzw(cur)
            if cyclic_clip:
                smoothed_rots[n_fr - 1] = list(smoothed_rots[0])
            for fi in range(n_fr):
                p_d, _r_old, d_old, lf = frames[fi]
                frames[fi] = (p_d, smoothed_rots[fi], d_old, lf)

        # 5. Multiplicative full-cycle seam distribution (never slows down end-of-cycle frames)
        if cyclic_clip:
            frames = close_loop_cyclic(frames)

        steps = [max(qangle_deg(frames[i][1][b], frames[i + 1][1][b]) for b in range(21)) for i in range(n_fr - 1)]
        step_med = sorted(steps)[len(steps) // 2] if steps else 0.0
        step_0 = steps[0] if steps else 0.0
        step_end = steps[-1] if steps else 0.0

        pelvis_drop = (pelvis_zs[0] - min(pelvis_zs)) * pelvis_scale
        path_len = sum(math.hypot(fb[0][0] - fa[0][0], fb[0][1] - fa[0][1]) for fa, fb in zip(frames, frames[1:]))
        root_speed = path_len / dur
        seam_deg = max(qangle_deg(frames[0][1][b], frames[-1][1][b]) for b in range(21))
        total_rot_motion = sum(
            qangle_deg(frames[0][1][bi], frames[fi][1][bi])
            for fi in range(1, n_fr, max(1, n_fr // 5))
            for bi in (1, 3, 6, 8, 12, 16, 19)
        )
        still_tpose = (post_arm_max < 24.0) or (post_el_min < 25.0 and post_hdist_min > 34.0) or (post_leg_max < 4.0 and not is_explicit_idle)
        low = aname.lower()
        broken_additive = ("raise_gun_on_turns" in low) or ("zero_gest" in low) or ("shoot_smg_8shots_gest" in low)
        bad_anat = (max_back_knee > 25.0) or (max_hip_twist > 145.0)

        out[aname] = {
            "fps": sample_fps,
            "frames": frames,
            "pelvis_drop": pelvis_drop,
            "root_speed": root_speed,
            "xy_travel": xy_travel,
            "duration": dur,
            "seam_deg": seam_deg,
            "step_med": step_med,
            "step_0": step_0,
            "step_end": step_end,
            "max_back_knee": max_back_knee,
            "max_hip_twist": max_hip_twist,
            "motion": total_rot_motion,
            "dev_arm": post_arm_max,
            "dev_leg": post_leg_max,
            "hand_dist": post_hdist_min,
            "composed": compose_mode,
            "rejected": bool(still_tpose or broken_additive or bad_anat),
        }

    Path(args.out_pkl).write_bytes(pickle.dumps(out, protocol=pickle.HIGHEST_PROTOCOL))
    composed_cnt = sum(1 for v in out.values() if v["composed"])
    rejected_cnt = sum(1 for v in out.values() if v["rejected"])
    cyclic_cnt = sum(1 for k, v in out.items() if is_candidate_loop(k) and v["seam_deg"] < 0.05)
    print(f"[blender-clean] processed={len(out)} composed={composed_cnt} cyclic_ok={cyclic_cnt} rejected={rejected_cnt} -> {args.out_pkl}")


if __name__ == "__main__":
    main()

