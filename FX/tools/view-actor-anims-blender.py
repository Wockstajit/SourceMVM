#!/usr/bin/env python3
"""Build & open an interactive Blender 5.x viewer with all 841 retargeted HL:A -> CS2 animations.

Usage:
  1. Build FX/actor-animations/actor_animations_viewer.blend (headless):
     blender.exe --factory-startup --background --python FX/tools/view-actor-anims-blender.py -- --build

  2. Open Blender GUI with the Actor Anims switcher sidebar active:
     blender.exe FX/actor-animations/actor_animations_viewer.blend --python FX/tools/view-actor-anims-blender.py -- --ui
"""

import argparse
import math
import os
import struct
import subprocess
import sys
from pathlib import Path

import bpy
import mathutils


ROOT = Path(__file__).resolve().parents[2]
PACK_PATH = ROOT / "FX" / "actor-animations" / "actor_animations.mvmanim"
AGENT_GLB = ROOT / "FX" / "actor-animations" / "cs2_agent_mesh.glb"
BLEND_PATH = ROOT / "FX" / "actor-animations" / "actor_animations_viewer.blend"
DEFAULT_VRF = os.environ.get("SOURCEMVM_VRF_CLI") or str(
    ROOT.parent / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe"
)

CATEGORIES = [
    ("ALL", "All Categories", ""),
    ("0", "Locomotion", ""),
    ("1", "Crouch", ""),
    ("2", "Aim & Weapon", ""),
    ("3", "Gestures", ""),
    ("4", "Idle", ""),
    ("5", "Reactions", ""),
    ("6", "Cinematic", ""),
]
CAT_NAMES = ["Locomotion", "Crouch", "Aim & Weapon", "Gestures", "Idle", "Reactions", "Cinematic"]
WEAPON_NAMES = ["No weapon", "Rifle", "Pistol", "Grenade"]

CORE_CHILD = [-1, -1, -1, -1, -1, -1, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1, 19, 20, -1] + [-1] * 30
CORE_END_PARENT = [
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 9, -1, -1, -1, 13, -1, -1, 16, -1, -1, 19,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
]
CORE_PARENT = [
    -1, 0, 1, 2, 3, 4, 5,
    4, 7, 8, 9,
    4, 11, 12, 13,
    0, 15, 16,
    0, 18, 19,
] + [10] * 15 + [14] * 15

# Blender armature space (-Y fwd, +X left, +Z up) <-> CS2 (+X fwd, +Y left, +Z up)
Q_B2S = mathutils.Matrix.Rotation(math.pi * 0.5, 4, "Z").to_quaternion()
Q_S2B = Q_B2S.inverted()
IN_PER_M = 39.37007874015748


def xyzw_to_q(t) -> mathutils.Quaternion:
    return mathutils.Quaternion((t[3], t[0], t[1], t[2])).normalized()


def swing_between(u: mathutils.Vector, v: mathutils.Vector) -> mathutils.Quaternion:
    if u.length < 1e-6 or v.length < 1e-6:
        return mathutils.Quaternion((1.0, 0.0, 0.0, 0.0))
    return u.normalized().rotation_difference(v.normalized())


def load_mvmanim(path: Path):
    data = path.read_bytes()
    if data[:8] != b"MVMACT02":
        raise RuntimeError(f"Unexpected magic in {path}")
    ver, nb, nc = struct.unpack_from("<III", data, 8)
    off = 20
    bone_names = []
    for _ in range(nb):
        bone_names.append(data[off : off + 24].split(b"\0", 1)[0].decode("ascii", "replace"))
        off += 24
    bind = []
    for _ in range(nb):
        p = struct.unpack_from("<3f", data, off)
        q = struct.unpack_from("<4f", data, off + 12)
        off += 28
        bind.append((mathutils.Vector(p), xyzw_to_q(q)))

    clips = []
    for _ in range(nc):
        cid = data[off : off + 64].split(b"\0", 1)[0].decode("ascii", "replace")
        disp = data[off + 64 : off + 128].split(b"\0", 1)[0].decode("ascii", "replace")
        off += 128
        cat, flags, wpn, _, fps, nf, pdrop, rspd = struct.unpack_from("<BBBBfIff", data, off)
        off += 20
        frames = []
        for _ in range(nf):
            pd = struct.unpack_from("<3f", data, off)
            lift = struct.unpack_from("<f", data, off + 12)[0]
            off += 16
            rots = [struct.unpack_from("<4f", data, off + b * 16) for b in range(nb)]
            off += nb * 16
            dirs = [struct.unpack_from("<3f", data, off + b * 12) for b in range(nb)]
            off += nb * 12
            frames.append((pd, lift, rots, dirs))
        clips.append({
            "id": cid,
            "display": disp,
            "category": int(cat),
            "loopable": bool(flags & 1),
            "has_root_motion": bool(flags & 2),
            "weapon": int(wpn),
            "fps": float(fps),
            "frames": frames,
            "pelvis_drop": float(pdrop),
            "root_speed": float(rspd),
        })
    return bone_names, bind, clips


_CACHE = {}


def get_pack_cache(pack_path: Path):
    key = str(pack_path)
    if key not in _CACHE:
        bone_names, bind, clips = load_mvmanim(pack_path)
        by_id = {c["id"]: c for c in clips}
        _CACHE[key] = (bone_names, bind, clips, by_id)
    return _CACHE[key]


class RigHelper:
    """Fast per-armature mapping & exact runtime-matched CS2 retarget solver."""

    def __init__(self, arm, bone_names, bind):
        self.arm = arm
        self.bone_names = bone_names
        self.bind = bind
        self.pb_by_name = {pb.name: pb for pb in arm.pose.bones}
        self.core_pb = [self.pb_by_name.get(nm) for nm in bone_names]
        self.rest_arm_q = {}
        self.rest_arm_p = {}
        self.rest_rel_q = {}
        self.rest_rel_m = {}
        for pb in arm.pose.bones:
            pb.rotation_mode = "QUATERNION"
            m = pb.bone.matrix_local
            self.rest_arm_q[pb.name] = m.to_quaternion().normalized()
            self.rest_arm_p[pb.name] = m.to_translation().copy()
            if pb.parent:
                rm = pb.parent.bone.matrix_local.inverted() @ m
                self.rest_rel_m[pb.name] = rm
                self.rest_rel_q[pb.name] = rm.to_quaternion().normalized()
            else:
                self.rest_rel_m[pb.name] = m.copy()
                self.rest_rel_q[pb.name] = m.to_quaternion().normalized()

        # Bind unit limb directions in CS2 space
        self.bind_dir = [None] * 21
        for bi in range(21):
            c = CORE_CHILD[bi]
            if c >= 0:
                dv = bind[c][0] - bind[bi][0]
                self.bind_dir[bi] = dv.normalized() if dv.length > 1e-4 else None
        self.bind_floor_z = min(bind[17][0].z, bind[20][0].z)
        self.core_idx_by_name = {nm: i for i, nm in enumerate(bone_names)}
        self.twist_names = [pb.name for pb in arm.pose.bones if "twist" in pb.name.lower()]
        self.core_local_p_cs2 = [mathutils.Vector((0.0, 0.0, 0.0)) for _ in range(21)]
        for bi in range(1, 21):
            par = CORE_PARENT[bi]
            if par >= 0:
                self.core_local_p_cs2[bi] = bind[par][1].inverted() @ (bind[bi][0] - bind[par][0])

    def build_action(self, clip, apply_root_xy: bool, floor_ground: bool, action_name: str = None):
        act_name = action_name or clip["id"]
        act = bpy.data.actions.get(act_name)
        if act is not None:
            bpy.data.actions.remove(act)
        act = bpy.data.actions.new(act_name)

        if not self.arm.animation_data:
            self.arm.animation_data_create()
        self.arm.animation_data.action = act
        slot = act.slots.new(id_type="OBJECT", name=self.arm.name) if not act.slots else act.slots[0]
        self.arm.animation_data.action_slot = slot
        layer = act.layers.new(name="Layer") if not act.layers else act.layers[0]
        strip = layer.strips.new(type="KEYFRAME") if not layer.strips else layer.strips[0]
        cb = strip.channelbag(slot, ensure=True)

        frames = clip["frames"]
        nf = len(frames)
        nb = len(self.bone_names)

        # Precompute per-frame local rotations and pelvis location
        pelvis_locs = [[] for _ in range(3)]
        bone_quats = {}
        active_core = [(bi, self.core_pb[bi]) for bi in range(nb) if self.core_pb[bi] is not None]
        for _, pb in active_core:
            bone_quats[pb.name] = [[], [], [], []]
        for tw in self.twist_names:
            bone_quats[tw] = [[], [], [], []]

        pelvis_pb = self.core_pb[0]
        pelvis_rest_q = self.rest_arm_q[pelvis_pb.name] if pelvis_pb else mathutils.Quaternion((1, 0, 0, 0))
        prev_q = {}

        for fi, (pd, lift, rots, dirs) in enumerate(frames):
            rot_q_cs2 = [xyzw_to_q(rots[bi]) for bi in range(nb)]
            corr_cs2 = [mathutils.Quaternion((1.0, 0.0, 0.0, 0.0)) for _ in range(nb)]
            for bi in range(21):
                bd = self.bind_dir[bi]
                dv = mathutils.Vector(dirs[bi])
                if bd is not None and dv.length_squared > 0.25:
                    cur = rot_q_cs2[bi] @ bd
                    corr_cs2[bi] = swing_between(cur, dv.normalized())
                elif CORE_END_PARENT[bi] >= 0:
                    corr_cs2[bi] = corr_cs2[CORE_END_PARENT[bi]]

            full_delta_cs2 = [(corr_cs2[bi] @ rot_q_cs2[bi]).normalized() for bi in range(nb)]

            # Target world CS2 pose for 21 humanoid bones so we can ground ankles cleanly
            dz_cs2 = 0.0
            if floor_ground:
                wp_cs2 = [None] * 21
                wq_cs2 = [None] * 21
                wq_cs2[0] = (full_delta_cs2[0] @ self.bind[0][1]).normalized()
                px = self.bind[0][0].x + (pd[0] if apply_root_xy else 0.0)
                py = self.bind[0][0].y + (pd[1] if apply_root_xy else 0.0)
                pz = self.bind[0][0].z + pd[2]
                wp_cs2[0] = mathutils.Vector((px, py, pz))
                for bi in range(1, 21):
                    par = CORE_PARENT[bi]
                    wp_cs2[bi] = wp_cs2[par] + (wq_cs2[par] @ self.core_local_p_cs2[bi])
                    wq_cs2[bi] = (full_delta_cs2[bi] @ self.bind[bi][1]).normalized()
                lowest_z = min(wp_cs2[17].z, wp_cs2[20].z)
                eff_lift = max(-1.5, lift)
                dz_cs2 = self.bind_floor_z + eff_lift - lowest_z

            pel_cs2 = mathutils.Vector((
                pd[0] if apply_root_xy else 0.0,
                pd[1] if apply_root_xy else 0.0,
                pd[2] + dz_cs2,
            ))
            pel_bl = (Q_S2B @ pel_cs2) / IN_PER_M
            pel_loc = pelvis_rest_q.inverted() @ pel_bl
            for k in range(3):
                pelvis_locs[k].append(float(pel_loc[k]))

            # Convert target world rotations in Blender armature space
            target_arm_q = {}
            for bi, pb in active_core:
                dbl = (Q_S2B @ full_delta_cs2[bi] @ Q_B2S).normalized()
                target_arm_q[pb.name] = (dbl @ self.rest_arm_q[pb.name]).normalized()

            for tw in self.twist_names:
                pb = self.pb_by_name[tw]
                par_name = pb.parent.name if pb.parent else None
                par_tq = target_arm_q.get(par_name, self.rest_arm_q.get(par_name, mathutils.Quaternion((1, 0, 0, 0))))
                # Neutralize CS2 twist relative to driven parent
                target_arm_q[tw] = (par_tq @ self.rest_rel_q[tw]).normalized()

            # Compute parent-local PoseBone.rotation_quaternion with hemisphere continuity
            for name, tq in target_arm_q.items():
                pb = self.pb_by_name[name]
                par_name = pb.parent.name if pb.parent else None
                par_tq = target_arm_q.get(par_name)
                if par_tq is None and par_name:
                    par_tq = self.rest_arm_q.get(par_name, mathutils.Quaternion((1, 0, 0, 0)))
                if par_tq is not None:
                    loc_q = (self.rest_rel_q[name].inverted() @ par_tq.inverted() @ tq).normalized()
                else:
                    loc_q = (self.rest_rel_q[name].inverted() @ tq).normalized()
                pq = prev_q.get(name)
                if pq is not None and pq.dot(loc_q) < 0.0:
                    loc_q.negate()
                elif pq is None and loc_q.w < 0.0:
                    loc_q.negate()
                prev_q[name] = loc_q
                bq = bone_quats[name]
                bq[0].append(float(loc_q.w))
                bq[1].append(float(loc_q.x))
                bq[2].append(float(loc_q.y))
                bq[3].append(float(loc_q.z))

        xs = [float(fi) for fi in range(nf)]

        def write_curve(data_path: str, array_index: int, ys):
            fc = cb.fcurves.new(data_path=data_path, index=array_index)
            fc.keyframe_points.add(nf)
            flat = [0.0] * (nf * 2)
            flat[0::2] = xs
            flat[1::2] = ys
            fc.keyframe_points.foreach_set("co", flat)
            fc.update()

        if pelvis_pb is not None:
            p_path = f'pose.bones["{pelvis_pb.name}"].location'
            for k in range(3):
                write_curve(p_path, k, pelvis_locs[k])

        for bname, qs in bone_quats.items():
            q_path = f'pose.bones["{bname}"].rotation_quaternion'
            for k in range(4):
                write_curve(q_path, k, qs[k])

        return act


def ensure_cs2_agent_glb():
    if AGENT_GLB.exists() and AGENT_GLB.stat().st_size > 100_000:
        return
    sys.path.insert(0, str(ROOT / "Automation" / "lib"))
    import steam_paths

    cs2_vpk = Path(steam_paths.cs2_install_dirs()[0]) / "game" / "csgo" / "pak01_dir.vpk"
    AGENT_GLB.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        DEFAULT_VRF, "-i", str(cs2_vpk),
        "--vpk_filepath", "agents/models/ctm_sas/ctm_sas.vmdl_c",
        "-o", str(AGENT_GLB), "-d", "--gltf_export_format", "glb",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not AGENT_GLB.exists():
        raise RuntimeError(f"Failed exporting CS2 SAS GLB:\n{proc.stdout}\n{proc.stderr}")


def apply_clip_to_scene(scene, clip_id: str):
    arm = bpy.data.objects.get("CS2_Actor")
    if not arm:
        return
    _, _, _, by_id = get_pack_cache(PACK_PATH)
    clip = by_id.get(clip_id)
    if clip is None:
        return
    props = scene.actor_anim_props
    act = bpy.data.actions.get(clip_id)
    if act is None or getattr(props, "force_rebuild", False):
        bone_names, bind, _clips, _by_id = get_pack_cache(PACK_PATH)
        helper = RigHelper(arm, bone_names, bind)
        act = helper.build_action(clip, props.apply_root_xy, props.floor_ground, clip_id)
        props.force_rebuild = False

    if not arm.animation_data:
        arm.animation_data_create()
    arm.animation_data.action = act
    if hasattr(act, "slots") and act.slots:
        arm.animation_data.action_slot = act.slots[0]

    nf = len(clip["frames"])
    scene.render.fps = max(1, int(round(clip["fps"])))
    scene.frame_start = 0
    scene.frame_end = max(1, nf - 1)
    scene.frame_set(0)
    props.active_clip_info = (
        f"{clip['display']} | {CAT_NAMES[clip['category']]} | "
        f"{WEAPON_NAMES[clip['weapon']]} | {'Loop' if clip['loopable'] else 'One-Shot'} | "
        f"{nf}f @ {clip['fps']:.1f}fps | {clip['root_speed']:.1f} u/s"
    )


def refresh_filtered_list(context):
    scene = context.scene
    props = scene.actor_anim_props
    _, _, clips, _ = get_pack_cache(PACK_PATH)
    q = props.search.strip().lower()
    cat_f = props.category_filter
    loop_only = props.loop_only
    props.items.clear()
    for c in clips:
        if cat_f != "ALL" and str(c["category"]) != cat_f:
            continue
        if loop_only and not c["loopable"]:
            continue
        if q and q not in c["id"].lower() and q not in c["display"].lower():
            continue
        item = props.items.add()
        item.clip_id = c["id"]
        item.name = c["display"]
        item.meta = (
            f"{'L' if c['loopable'] else '1'} • {CAT_NAMES[c['category']][:4]} • "
            f"{len(c['frames'])}f"
        )
    if len(props.items) > 0:
        idx = min(max(0, props.active_index), len(props.items) - 1)
        if props.active_index != idx:
            props.active_index = idx


# ---- Blender UI PropertyGroup, UIList, Operators & Panel -----------------------------------------

class ACTORANIM_Item(bpy.types.PropertyGroup):
    clip_id: bpy.props.StringProperty()
    meta: bpy.props.StringProperty()


def _on_index_changed(self, context):
    if 0 <= self.active_index < len(self.items):
        apply_clip_to_scene(context.scene, self.items[self.active_index].clip_id)


def _on_filter_changed(self, context):
    refresh_filtered_list(context)
    if 0 <= self.active_index < len(self.items):
        apply_clip_to_scene(context.scene, self.items[self.active_index].clip_id)


def _on_retarget_option_changed(self, context):
    self.force_rebuild = True
    if 0 <= self.active_index < len(self.items):
        apply_clip_to_scene(context.scene, self.items[self.active_index].clip_id)


class ACTORANIM_Props(bpy.types.PropertyGroup):
    search: bpy.props.StringProperty(name="Search", default="", update=_on_filter_changed)
    category_filter: bpy.props.EnumProperty(name="Category", items=CATEGORIES, default="ALL", update=_on_filter_changed)
    loop_only: bpy.props.BoolProperty(name="Loops Only", default=False, update=_on_filter_changed)
    apply_root_xy: bpy.props.BoolProperty(name="Apply Root XY", default=True, update=_on_retarget_option_changed)
    floor_ground: bpy.props.BoolProperty(name="Floor Grounding", default=True, update=_on_retarget_option_changed)
    force_rebuild: bpy.props.BoolProperty(default=False)
    items: bpy.props.CollectionProperty(type=ACTORANIM_Item)
    active_index: bpy.props.IntProperty(default=0, update=_on_index_changed)
    active_clip_info: bpy.props.StringProperty(default="")


class ACTORANIM_UL_Clips(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.label(text=item.name, icon="ACTION")
        row.label(text=item.meta)


class ACTORANIM_OT_Step(bpy.types.Operator):
    bl_idname = "actoranim.step"
    bl_label = "Step Animation"
    delta: bpy.props.IntProperty(default=1)

    def execute(self, context):
        p = context.scene.actor_anim_props
        if len(p.items) == 0:
            return {"CANCELLED"}
        p.active_index = (p.active_index + self.delta) % len(p.items)
        return {"FINISHED"}


class ACTORANIM_OT_PlayPause(bpy.types.Operator):
    bl_idname = "actoranim.play_pause"
    bl_label = "Play / Pause"

    def execute(self, context):
        bpy.ops.screen.animation_play()
        return {"FINISHED"}


class ACTORANIM_OT_RebuildAll(bpy.types.Operator):
    bl_idname = "actoranim.rebuild_active"
    bl_label = "Re-apply Active Pose"

    def execute(self, context):
        p = context.scene.actor_anim_props
        p.force_rebuild = True
        if 0 <= p.active_index < len(p.items):
            apply_clip_to_scene(context.scene, p.items[p.active_index].clip_id)
        return {"FINISHED"}


class ACTORANIM_PT_Main(bpy.types.Panel):
    bl_label = "Actor Anims (Retargeted)"
    bl_idname = "ACTORANIM_PT_Main"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Actor Anims"

    def draw(self, context):
        layout = self.layout
        p = context.scene.actor_anim_props

        row = layout.row(align=True)
        row.prop(p, "search", text="", icon="VIEWZOOM")
        layout.prop(p, "category_filter", text="Category")

        row = layout.row(align=True)
        row.prop(p, "loop_only")
        row.label(text=f"{len(p.items)} clips")

        layout.template_list("ACTORANIM_UL_Clips", "", p, "items", p, "active_index", rows=14)

        row = layout.row(align=True)
        op_prev = row.operator("actoranim.step", text="Prev", icon="TRIA_LEFT")
        op_prev.delta = -1
        row.operator("actoranim.play_pause", text="Play / Pause", icon="PLAY")
        op_next = row.operator("actoranim.step", text="Next", icon="TRIA_RIGHT")
        op_next.delta = 1

        if p.active_clip_info:
            box = layout.box()
            box.label(text=p.active_clip_info, icon="INFO")
            if 0 <= p.active_index < len(p.items):
                box.label(text=f"ID: {p.items[p.active_index].clip_id}")

        box2 = layout.box()
        box2.label(text="Retarget Options", icon="ARMATURE_DATA")
        box2.prop(p, "apply_root_xy")
        box2.prop(p, "floor_ground")
        box2.operator("actoranim.rebuild_active", icon="FILE_REFRESH")


_CLASSES = (
    ACTORANIM_Item,
    ACTORANIM_Props,
    ACTORANIM_UL_Clips,
    ACTORANIM_OT_Step,
    ACTORANIM_OT_PlayPause,
    ACTORANIM_OT_RebuildAll,
    ACTORANIM_PT_Main,
)


def register():
    for cls in _CLASSES:
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
        bpy.utils.register_class(cls)
    bpy.types.Scene.actor_anim_props = bpy.props.PointerProperty(type=ACTORANIM_Props)


def init_ui_state():
    scene = bpy.context.scene
    props = scene.actor_anim_props
    if len(props.items) == 0:
        refresh_filtered_list(bpy.context)
    arm = bpy.data.objects.get("CS2_Actor")
    if arm and arm.animation_data and arm.animation_data.action:
        cur_id = arm.animation_data.action.name
        for i, it in enumerate(props.items):
            if it.clip_id == cur_id:
                props.active_index = i
                break
    # Ensure sidebar N-panel is visible in 3D view and switched to Actor Anims
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                for space in area.spaces:
                    if space.type == "VIEW_3D":
                        space.show_region_ui = True
                for reg in area.regions:
                    if reg.type == "UI":
                        try:
                            reg.active_panel_category = "Actor Anims"
                        except Exception:
                            pass


def build_blend():
    ensure_cs2_agent_glb()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    register()

    scene = bpy.context.scene
    scene.render.fps = 30
    bpy.ops.import_scene.gltf(filepath=str(AGENT_GLB))

    # Remove first-person viewmodel meshes and unused physics/dummy objects
    arm = None
    for obj in list(bpy.data.objects):
        low = obj.name.lower()
        if obj.type == "ARMATURE" and arm is None:
            arm = obj
            obj.name = "CS2_Actor"
        elif any(k in low for k in ("firstperson", "dummy", "physics")):
            bpy.data.objects.remove(obj, do_unlink=True)
        elif obj.type == "MESH":
            # Ensure X-ray/armature visibility is easy to inspect
            pass

    if arm is None:
        raise RuntimeError(f"No armature found in {AGENT_GLB}")
    arm.show_in_front = True
    arm.data.display_type = "STICK"

    # Studio floor + lighting
    bpy.ops.mesh.primitive_plane_add(size=8.0, location=(0.0, 0.0, 0.0))
    floor = bpy.context.active_object
    floor.name = "GroundPlane"
    mat = bpy.data.materials.new("GroundMat")
    mat.diffuse_color = (0.16, 0.17, 0.19, 1.0)
    floor.data.materials.append(mat)

    bpy.ops.object.light_add(type="SUN", location=(2.0, -3.0, 4.0))
    sun = bpy.context.active_object
    sun.data.energy = 3.0
    sun.rotation_euler = (0.65, 0.2, 0.7)

    # Frame camera / 3D view nicely onto actor
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                for space in area.spaces:
                    if space.type == "VIEW_3D":
                        space.show_region_ui = True
                        space.shading.type = "SOLID"
                        r3d = space.region_3d
                        r3d.view_location = (0.0, 0.0, 0.95)
                        r3d.view_distance = 3.4
                        # Front-right 3/4 view onto character facing -Y
                        r3d.view_rotation = mathutils.Euler((math.radians(76.0), 0.0, math.radians(-32.0))).to_quaternion()
            elif area.type == "DOPESHEET_EDITOR":
                for space in area.spaces:
                    if space.type == "DOPESHEET_EDITOR":
                        try:
                            space.mode = "ACTION"
                        except Exception:
                            pass

    bone_names, bind, clips, _by_id = get_pack_cache(PACK_PATH)
    helper = RigHelper(arm, bone_names, bind)
    print(f"[viewer-build] Baking all {len(clips)} retargeted Actions into Blender...")
    for idx, clip in enumerate(clips):
        # Default root XY enabled for stationary non-locomotion, disabled for forward travel loops so they loop in-place
        use_root = (not clip["has_root_motion"] and clip["category"] != 0)
        act = helper.build_action(clip, apply_root_xy=use_root, floor_ground=True)
        act.use_fake_user = True
        if (idx + 1) % 200 == 0:
            print(f"  baked {idx + 1}/{len(clips)} actions...")

    # Embed startup script text block inside the .blend as well
    txt_name = "actor_anims_ui.py"
    if txt_name in bpy.data.texts:
        bpy.data.texts.remove(bpy.data.texts[txt_name])
    txt = bpy.data.texts.new(txt_name)
    txt.from_string(Path(__file__).read_text(encoding="utf-8"))
    txt.use_module = True

    refresh_filtered_list(bpy.context)
    default_id = "hla_walk_fwd" if "hla_walk_fwd" in _by_id else clips[0]["id"]
    apply_clip_to_scene(scene, default_id)
    for i, it in enumerate(scene.actor_anim_props.items):
        if it.clip_id == default_id:
            scene.actor_anim_props.active_index = i
            break

    bpy.context.view_layer.objects.active = arm
    BLEND_PATH.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND_PATH))
    print(f"[viewer-build] Saved {BLEND_PATH} with {len(bpy.data.actions)} actions ({BLEND_PATH.stat().st_size / (1024*1024):.1f} MiB)")


def main():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--ui", action="store_true")
    args = ap.parse_args(argv)
    if args.build:
        build_blend()
    elif args.ui:
        register()
        bpy.app.timers.register(lambda: (init_ui_state(), None)[1], first_interval=0.15)
    else:
        register()


if __name__ == "__main__":
    main()
elif __name__ == "actor_anims_ui":
    register()
    bpy.app.timers.register(lambda: (init_ui_state(), None)[1], first_interval=0.25)
