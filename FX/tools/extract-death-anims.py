#!/usr/bin/env python3
"""Bake donor death animations into a CS2 death-animation clip pack.

Standalone: own donors, own output, own entry point. It shares FX/tools/ with the particle
converters but no code or build step -- do not call it from convert-fx-source1.ps1.

Three donors feed the one pack:
- GMod "Brutal Deaths": a Source 1 studiomodel (`model_anim.mdl`, ValveBiped skeleton) holding
  178 death sequences.
- Call of Duty (World at War, Black Ops 1/2, Modern Warfare 3) player deaths: Greyhound SEAnim
  exports posed on the matching player rest skeleton (FX/sources/cod-deaths).
- Modern Warfare 2019 player deaths: DTZxPorter Cast exports posed on the matching player rest
  skeleton (FX/sources/mw2019-deaths).

Every clip's source is written into the pack as an explicit tag (0 = MW2019, 1 = Classic CoD,
2 = Extra), assigned by which donor baked it.

Nothing about a donor MESH is used: this reads the animation tracks only and resamples fifteen
donor bones onto CS2's verified fifteen-body player ragdoll layout. Rotations are calibrated
into CS2 body axes, then positions are articulated from CS2's native joint anchors. The pack
stores a base pose plus per-frame displacement and absolute rotation. Runtime placement fits
facing only: the native joint geometry already has the correct size.

Every clip carries three tags the game selects by: body area (category), the side the hit came
from, and the stance it starts in. They are guessed from names and motion, and the Blender
viewer's corrections in FX/death-animations/clip-tags.json override every guess. Clips listed in
FX/death-animations/removed-clips.txt are left out.

    py -3 FX/tools/extract-death-anims.py [--source MDL] [--cod DIR] [--mw2019-source DIR] [--out FILE] [--report]

See Docs/features/filmmaker_death_animations.md for the runtime contract.
"""

import argparse
import json
import math
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# Source 1 studiomodel animation decoding (MDL v44-v49, animation data only)
# ---------------------------------------------------------------------------

STUDIO_ANIM_RAWPOS, STUDIO_ANIM_RAWROT = 0x01, 0x02
STUDIO_ANIM_ANIMPOS, STUDIO_ANIM_ANIMROT = 0x04, 0x08
STUDIO_ANIM_RAWROT2 = 0x20
STUDIO_DELTA = 0x0004
BONE_SIZE, ANIMDESC_SIZE, SEQDESC_SIZE = 216, 100, 212


def half_to_float(u):
    """Source Vector48 half float: sign, 5-bit exponent (bias 15), 10-bit mantissa."""
    sign, exponent, mantissa = (u >> 15) & 1, (u >> 10) & 0x1f, u & 0x3ff
    if exponent == 0:
        value = mantissa / 1024.0 * 2.0 ** -14 if mantissa else 0.0
    elif exponent == 31:
        value = 65504.0
    else:
        value = (1.0 + mantissa / 1024.0) * 2.0 ** (exponent - 15)
    return -value if sign else value


def quat48(raw):
    x = raw[0] | (raw[1] << 8)
    y = raw[2] | (raw[3] << 8)
    zw = raw[4] | (raw[5] << 8)
    fx = (x - 32768) * (1.0 / 32768.0)
    fy = (y - 32768) * (1.0 / 32768.0)
    fz = ((zw & 0x7fff) - 16384) * (1.0 / 16384.0)
    remainder = 1.0 - fx * fx - fy * fy - fz * fz
    fw = math.sqrt(remainder) if remainder > 0 else 0.0
    return (fx, fy, fz, -fw if (zw >> 15) & 1 else fw)


def quat64(raw):
    packed = int.from_bytes(raw[:8], 'little')
    fx = ((packed & 0x1fffff) - 1048576) * (1.0 / 1048576.5)
    fy = (((packed >> 21) & 0x1fffff) - 1048576) * (1.0 / 1048576.5)
    fz = (((packed >> 42) & 0x1fffff) - 1048576) * (1.0 / 1048576.5)
    remainder = 1.0 - fx * fx - fy * fy - fz * fz
    fw = math.sqrt(remainder) if remainder > 0 else 0.0
    return (fx, fy, fz, -fw if (packed >> 63) & 1 else fw)


def euler_to_quat(rx, ry, rz):
    """Source RadianEuler (X roll, Y pitch, Z yaw) -> quaternion, matching AngleQuaternion."""
    sr, cr = math.sin(rx * 0.5), math.cos(rx * 0.5)
    sp, cp = math.sin(ry * 0.5), math.cos(ry * 0.5)
    sy, cy = math.sin(rz * 0.5), math.cos(rz * 0.5)
    return (sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty,
            vy + w * ty + z * tx - x * tz,
            vz + w * tz + x * ty - y * tx)


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q))
    return tuple(c / n for c in q) if n > 1e-12 else (0.0, 0.0, 0.0, 1.0)


def matrix_quat(m):
    """Row-major 3x4 rigid transform -> xyzw (largest component avoids 180-degree loss)."""
    candidates = [1 + m[0] - m[5] - m[10], 1 - m[0] + m[5] - m[10],
                  1 - m[0] - m[5] + m[10], 1 + m[0] + m[5] + m[10]]
    i = max(range(4), key=candidates.__getitem__)
    s = math.sqrt(max(0, candidates[i])) * 2
    if i == 0: q = (s / 4, (m[1] + m[4]) / s, (m[2] + m[8]) / s, (m[9] - m[6]) / s)
    elif i == 1: q = ((m[1] + m[4]) / s, s / 4, (m[6] + m[9]) / s, (m[2] - m[8]) / s)
    elif i == 2: q = ((m[2] + m[8]) / s, (m[6] + m[9]) / s, s / 4, (m[4] - m[1]) / s)
    else: q = ((m[9] - m[6]) / s, (m[2] - m[8]) / s, (m[4] - m[1]) / s, s / 4)
    return qnorm(q)


def swing_between(a, b):
    """Shortest rotation between directions, preserving twist about the limb."""
    a = tuple(x / math.sqrt(sum(v*v for v in a)) for x in a)
    b = tuple(x / math.sqrt(sum(v*v for v in b)) for x in b)
    dot = sum(x*y for x,y in zip(a,b))
    if dot < -0.999999:
        axis = (1,0,0) if abs(a[0]) < .9 else (0,1,0)
        return qnorm((a[1]*axis[2]-a[2]*axis[1], a[2]*axis[0]-a[0]*axis[2],
                      a[0]*axis[1]-a[1]*axis[0], 0))
    return qnorm((a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2],
                  a[0]*b[1]-a[1]*b[0], 1+dot))


RIGID_CHILD = {0:1, 1:13, 2:3, 3:14, 7:8, 8:11, 9:10, 10:12}


def cs2_reference():
    path = os.path.join(os.path.dirname(__file__), '..', 'sources', 'brutal-deaths', 'cs2-agent-bind.json')
    with open(path, encoding='utf-8') as f: reference = json.load(f)['bones']
    if [b['name'] for b in reference] != [n for n,_ in CS2_BODY_ORDER]:
        raise ValueError('CS2 reference body order mismatch')
    return reference


def cs2_child_offsets():
    reference = cs2_reference()
    result = {}
    for body,child in RIGID_CHILD.items():
        m = reference[body]['bind']
        delta = [reference[child]['bind'][k]-m[k] for k in (3,7,11)]
        result[body] = tuple(sum(m[row*4+col]*delta[row] for row in range(3)) for col in range(3))
    return result


def cs2_joints():
    path = os.path.join(os.path.dirname(__file__), '..', 'sources', 'brutal-deaths', 'cs2-agent-bind.json')
    with open(path, encoding='utf-8') as f: return json.load(f)['joints']


def articulated_positions(root, rotations, joints):
    """Resolve the CS2 joint tree so both sides of every positional anchor coincide."""
    positions = {PELVIS_BODY: tuple(root)}
    pending = list(joints)
    while pending:
        count = len(pending)
        for joint in pending[:]:
            parent,child = joint['parent'],joint['child']
            if parent not in positions: continue
            a = qrot(rotations[parent],joint['frame1'][:3])
            b = qrot(rotations[child],joint['frame2'][:3])
            positions[child] = tuple(positions[parent][k]+a[k]-b[k] for k in range(3))
            pending.remove(joint)
        if len(pending) == count: raise ValueError('CS2 joint reference is not a pelvis-rooted tree')
    if len(positions) != BODY_COUNT: raise ValueError('Incomplete CS2 joint reference')
    return [positions[i] for i in range(BODY_COUNT)]


def _unit(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n > 1e-9 else (0.0, 0.0, 0.0)


def _cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def _signed_angle(a, b, axis):
    """Angle turning a onto b about axis, after projecting both onto its plane."""
    axis = _unit(axis)
    a = [a[k]-axis[k]*sum(a[j]*axis[j] for j in range(3)) for k in range(3)]
    b = [b[k]-axis[k]*sum(b[j]*axis[j] for j in range(3)) for k in range(3)]
    return math.atan2(sum(_cross(a, b)[k]*axis[k] for k in range(3)),
                      sum(a[k]*b[k] for k in range(3)))


def _axis_angle(axis, radians):
    s = math.sin(radians/2)
    return qnorm((axis[0]*s, axis[1]*s, axis[2]*s, math.cos(radians/2)))


def cs2_knee_bend_signs(joints):
    """+1/-1 per knee: which way CS2's local-Z hinge axis faces a forward knee bend."""
    reference = cs2_reference()
    rest = [matrix_quat(b['bind']) for b in reference]
    points = [tuple(b['bind'][k] for k in (3, 7, 11)) for b in reference]
    signs = {}
    for joint in joints:
        parent, child = joint['parent'], joint['child']
        if child not in (1, 3):
            continue
        ankle = 13 if child == 1 else 14
        bend = _cross([points[child][k]-points[parent][k] for k in range(3)],
                      [points[ankle][k]-points[child][k] for k in range(3)])
        hinge = qrot(qmul(rest[parent], joint['frame1'][4:8]), (0, 0, 1))
        signs[child] = 1.0 if sum(hinge[k]*bend[k] for k in range(3)) >= 0 else -1.0
    return signs


def fit_knee_hinges(rotations, joints, legs, bend_signs, child_offsets):
    """Fit CS2's single-axis knees to the donor's hip, knee and ankle positions.

    The donor knee is a positional hinge, but its calf bones carry roll about the shin that a
    CS2 hinge cannot express. Projecting that rotation onto the hinge axis bent the shin into the
    wrong plane and pushed the discarded roll into the foot. Instead, roll the thigh so its hinge
    axis faces the donor's bend normal, then set the hinge angle that points the shin at the donor
    ankle. The foot keeps its own calibrated donor orientation.
    """
    for joint in joints:
        parent, child = joint['parent'], joint['child']
        if child not in (1, 3):
            continue
        frame1, frame2 = joint['frame1'][4:8], joint['frame2'][4:8]
        hip, knee, foot = legs[child]
        thigh = [knee[k]-hip[k] for k in range(3)]
        shin = [foot[k]-knee[k] for k in range(3)]
        cosine = sum(a*b for a, b in zip(_unit(thigh), _unit(shin)))
        bend = math.degrees(math.acos(max(-1.0, min(1.0, cosine))))
        # Nearly straight legs have no measurable bend plane; keep the calibrated thigh roll.
        weight = min(1.0, max(0.0, (bend-3.0)/9.0))
        if weight > 0:
            hinge = [c*bend_signs[child] for c in qrot(qmul(rotations[parent], frame1), (0, 0, 1))]
            normal = _cross(thigh, shin)
            if sum(hinge[k]*normal[k] for k in range(3)) < 0:
                normal = [-c for c in normal]   # hyperextension: same plane, opposite bend
            roll = _signed_angle(hinge, normal, thigh)*weight
            rotations[parent] = qnorm(qmul(_axis_angle(_unit(thigh), roll), rotations[parent]))
        axis = qrot(qmul(rotations[parent], frame1), (0, 0, 1))
        straight = qnorm(qmul(qmul(rotations[parent], frame1), qconj(frame2)))
        angle = _signed_angle(qrot(straight, child_offsets[child]), shin, axis)
        rotations[child] = qnorm(qmul(_axis_angle(_unit(axis), angle), straight))


def rotation_offsets(mdl, bone_ids):
    """Map CS2 body-local axes into ValveBiped axes; this is not an animation blend.

    Both rigs use Z-up, but their facing, bone roll and mirrored right-side axes differ.
    Align the bind poses by hip yaw first, then remove limb swing from the calibration:
    their idle knee/elbow bends differ and must not become an error in every frame.
    """
    reference = cs2_reference()
    target_p = [tuple(b['bind'][k] for k in (3,7,11)) for b in reference]
    target_q = [matrix_quat(b['bind']) for b in reference]
    positions, rotations = {}, {}
    for i,b in enumerate(mdl.bones):
        parent = b['parent']
        if parent < 0: positions[i],rotations[i] = b['pos'],qnorm(b['quat'])
        else:
            delta = qrot(rotations[parent],b['pos'])
            positions[i] = tuple(positions[parent][k]+delta[k] for k in range(3))
            rotations[i] = qnorm(qmul(rotations[parent],b['quat']))
    donor_hips = [positions[bone_ids[0]][k]-positions[bone_ids[2]][k] for k in range(3)]
    cs2_hips = [target_p[0][k]-target_p[2][k] for k in range(3)]
    yaw = math.atan2(cs2_hips[1],cs2_hips[0])-math.atan2(donor_hips[1],donor_hips[0])
    undo_yaw = euler_to_quat(0,0,-yaw)
    offsets = []
    for i,bone in enumerate(bone_ids):
        calibrated = qmul(undo_yaw,target_q[i])
        if i in RIGID_CHILD:
            child = RIGID_CHILD[i]
            cs2_local = qrot(qconj(target_q[i]),[target_p[child][k]-target_p[i][k] for k in range(3)])
            donor_direction = [positions[bone_ids[child]][k]-positions[bone][k] for k in range(3)]
            swing = swing_between(qrot(calibrated,cs2_local),donor_direction)
            calibrated = qmul(swing,calibrated)
        offsets.append(qnorm(qmul(qconj(rotations[bone]),calibrated)))
    return offsets


class StudioModel:
    """Reads bones, sequences and animation tracks out of an IDST .mdl."""

    def __init__(self, path):
        with open(path, 'rb') as handle:
            self.data = handle.read()
        if self.data[:4] != b'IDST':
            raise ValueError('%s is not a Source 1 studiomodel (missing IDST)' % path)
        self.version = self.i32(4)
        if not 44 <= self.version <= 49:
            raise ValueError('unsupported studiomodel version %d' % self.version)
        # studiohdr_t: id, version, checksum, name[64], length, then six vectors.
        base = 12 + 64 + 4 + 72
        self.num_bones, self.bone_index = self.i32(base + 4), self.i32(base + 8)
        self.num_anims, self.anim_index = self.i32(base + 28), self.i32(base + 32)
        self.num_seqs, self.seq_index = self.i32(base + 36), self.i32(base + 40)
        self._read_bones()
        self._read_sequences()

    def i32(self, offset):
        return struct.unpack_from('<i', self.data, offset)[0]

    def f32(self, offset):
        return struct.unpack_from('<f', self.data, offset)[0]

    def vec3(self, offset):
        return struct.unpack_from('<3f', self.data, offset)

    def cstr(self, offset):
        return self.data[offset:self.data.index(b'\0', offset)].decode('ascii', 'replace')

    def _read_bones(self):
        self.bones = []
        for i in range(self.num_bones):
            b = self.bone_index + i * BONE_SIZE
            self.bones.append({
                'name': self.cstr(b + self.i32(b)),
                'parent': self.i32(b + 4),
                'pos': self.vec3(b + 32),
                'quat': struct.unpack_from('<4f', self.data, b + 44),
                'rot': self.vec3(b + 60),
                'posscale': self.vec3(b + 72),
                'rotscale': self.vec3(b + 84),
            })
        self.bone_by_name = {b['name']: i for i, b in enumerate(self.bones)}

    def _read_sequences(self):
        self.sequences = {}
        for s in range(self.num_seqs):
            b = self.seq_index + s * SEQDESC_SIZE
            anim = struct.unpack_from('<h', self.data, b + self.i32(b + 60))[0]
            self.sequences[self.cstr(b + self.i32(b + 4))] = anim

    def animdesc(self, index):
        b = self.anim_index + index * ANIMDESC_SIZE
        return {'name': self.cstr(b + self.i32(b + 4)), 'fps': self.f32(b + 8),
                'flags': self.i32(b + 12), 'frames': self.i32(b + 16),
                'animindex': self.i32(b + 56), 'sectionindex': self.i32(b + 80),
                'sectionframes': self.i32(b + 84), 'base': b}

    def _anim_value(self, base, frame, scale):
        """Source ExtractAnimValue, sampled on an integer frame (no sub-frame interpolation)."""
        data = self.data
        offset, remaining = base, frame
        valid, total = data[offset], data[offset + 1]
        while total <= remaining:
            remaining -= total
            offset += (valid + 1) * 2
            valid, total = data[offset], data[offset + 1]
        index = remaining + 1 if valid > remaining else valid
        return struct.unpack_from('<h', data, offset + index * 2)[0] * scale

    def _section(self, desc, frame):
        """Long animations split their bone chains into fixed-size frame sections."""
        if not (desc['sectionindex'] and desc['sectionframes']):
            return desc['base'] + desc['animindex'], frame
        section = min(frame // desc['sectionframes'],
                      (desc['frames'] - 1) // desc['sectionframes'])
        entry = desc['base'] + desc['sectionindex'] + section * 8
        return desc['base'] + self.i32(entry + 4), frame - section * desc['sectionframes']

    def local_pose(self, anim, frame):
        """One frame of bone-local (position, quaternion) for every bone."""
        desc = self.animdesc(anim)
        frame = max(0, min(frame, desc['frames'] - 1))
        delta = bool(desc['flags'] & STUDIO_DELTA)
        offset, local_frame = self._section(desc, frame)
        data = self.data
        pose = {}
        while True:
            bone, flags = data[offset], data[offset + 1]
            next_offset = struct.unpack_from('<H', data, offset + 2)[0]
            if bone >= self.num_bones:
                break
            info = self.bones[bone]
            raw = offset + 4
            quat = position = None
            if flags & STUDIO_ANIM_RAWROT:
                quat = quat48(data[raw:raw + 6])
                raw += 6
            if flags & STUDIO_ANIM_RAWROT2:
                quat = quat64(data[raw:raw + 8])
                raw += 8
            if flags & STUDIO_ANIM_RAWPOS:
                position = tuple(half_to_float(struct.unpack_from('<H', data, raw + 2 * k)[0])
                                 for k in range(3))
            track = offset + 4
            if flags & STUDIO_ANIM_ANIMROT:
                offsets = struct.unpack_from('<3h', data, track)
                origin = (0.0, 0.0, 0.0) if delta else info['rot']
                quat = euler_to_quat(*[
                    origin[k] + (self._anim_value(track + offsets[k], local_frame,
                                                  info['rotscale'][k]) if offsets[k] else 0.0)
                    for k in range(3)])
                track += 6
            if flags & STUDIO_ANIM_ANIMPOS:
                offsets = struct.unpack_from('<3h', data, track)
                origin = (0.0, 0.0, 0.0) if delta else info['pos']
                position = tuple(
                    origin[k] + (self._anim_value(track + offsets[k], local_frame,
                                                  info['posscale'][k]) if offsets[k] else 0.0)
                    for k in range(3))
            if quat is None:
                quat = (0.0, 0.0, 0.0, 1.0) if delta else info['quat']
            if position is None:
                position = (0.0, 0.0, 0.0) if delta else info['pos']
            pose[bone] = (position, qnorm(quat))
            if not next_offset:
                break
            offset += next_offset
        for i, info in enumerate(self.bones):
            pose.setdefault(i, (info['pos'], qnorm(info['quat'])))
        return pose

    def model_pose(self, anim, frame):
        """Bone-local -> model space, walking parents in declaration order."""
        local = self.local_pose(anim, frame)
        positions, rotations = {}, {}
        for i, info in enumerate(self.bones):
            position, quat = local[i]
            parent = info['parent']
            if parent < 0:
                positions[i], rotations[i] = position, quat
            else:
                base, rotation = positions[parent], rotations[parent]
                turned = qrot(rotation, position)
                positions[i] = (base[0] + turned[0], base[1] + turned[1], base[2] + turned[2])
                rotations[i] = qnorm(qmul(rotation, quat))
        return positions, rotations


# ---------------------------------------------------------------------------
# Call of Duty SEAnim donor (Greyhound exports, IW/Treyarch player skeleton)
# ---------------------------------------------------------------------------

CM_TO_INCH = 1.0 / 2.54
SEANIM_TYPES = {0: 'absolute', 1: 'additive', 2: 'relative', 3: 'delta'}


def read_seanim(path):
    """SEAnim v1: per-bone sparse location/rotation keys. Greyhound writes these player
    deaths as DELTA: rotations are absolute bone-local, locations are offsets from the
    rest skeleton, and tag_origin carries the root motion."""
    with open(path, 'rb') as handle:
        d = handle.read()
    if d[:6] != b'SEAnim':
        raise ValueError('%s is not an SEAnim file' % path)
    anim_type, presence, props = d[10], d[12], d[13]
    fps, frames, bones = struct.unpack_from('<fII', d, 16)
    modifiers = d[28]
    o = 36
    names = []
    for _ in range(bones):
        end = d.index(b'\0', o)
        names.append(d[o:end].decode('ascii', 'replace'))
        o = end + 1
    def index_format(count):
        return ('<B', 1) if count <= 0xff else (('<H', 2) if count <= 0xffff else ('<I', 4))
    bone_fmt, bone_size = index_format(bones)
    frame_fmt, frame_size = index_format(frames)
    if modifiers:
        raise ValueError('%s: per-bone animation type modifiers are not supported' % path)
    vec_fmt, vec_size = ('<3d', 24) if props & 1 else ('<3f', 12)
    quat_fmt, quat_size = ('<4d', 32) if props & 1 else ('<4f', 16)
    tracks = {}
    for name in names:
        o += 1  # per-bone flags (cosmetic)
        track = {}
        for bit, key, fmt, size in ((1, 'loc', vec_fmt, vec_size), (2, 'rot', quat_fmt, quat_size),
                                    (4, 'scale', vec_fmt, vec_size)):
            if not presence & bit:
                continue
            count = struct.unpack_from(frame_fmt, d, o)[0]
            o += frame_size
            keys = []
            for _ in range(count):
                frame = struct.unpack_from(frame_fmt, d, o)[0]
                keys.append((frame, struct.unpack_from(fmt, d, o + frame_size)))
                o += frame_size + size
            track[key] = sorted(keys)
        tracks[name] = track
    return {'type': SEANIM_TYPES.get(anim_type), 'fps': fps, 'frames': frames, 'tracks': tracks}


def _sample(keys, frame, rotation):
    """Linear (nlerp for rotations) between the sparse keys around `frame`, held at the ends."""
    if frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    for (f0, a), (f1, b) in zip(keys, keys[1:]):
        if f0 <= frame <= f1:
            t = (frame - f0) / float(f1 - f0) if f1 > f0 else 0.0
            if rotation and sum(x*y for x, y in zip(a, b)) < 0:
                b = tuple(-x for x in b)
            value = tuple(x + (y - x) * t for x, y in zip(a, b))
            return qnorm(value) if rotation else value
    return keys[-1][1]


class SEAnimDonor:
    """Poses Call of Duty SEAnim clips on a rest skeleton from the matching player model.

    Same interface as StudioModel for bake_clip: bones, bone_by_name, sequences, animdesc
    and model_pose. Everything is converted to inches, the unit of the CS2 pack.
    """

    def __init__(self, skeleton_path, anim_dir, clip_files, reader=None, extension='.seanim'):
        with open(skeleton_path, encoding='utf-8') as handle:
            skeleton = json.load(handle)
        scale = CM_TO_INCH if skeleton.get('units') == 'cm' else 1.0
        self.bones = [{'name': b['name'], 'parent': b['parent'],
                       'pos': tuple(c * scale for c in b['pos']), 'quat': qnorm(tuple(b['quat']))}
                      for b in skeleton['bones']]
        self.bone_by_name = {b['name']: i for i, b in enumerate(self.bones)}
        self.scale = scale
        self.clips, self.sequences, self.notes = [], {}, {}
        for name, stem in clip_files:
            anim = (reader or read_seanim)(os.path.join(anim_dir, stem + extension))
            if anim['type'] not in ('delta', 'absolute', 'relative'):
                raise ValueError('%s: unsupported SEAnim type %s' % (stem, anim['type']))
            self.notes[name] = self._sanitize_root(anim)
            self.sequences[name] = len(self.clips)
            self.clips.append((stem, anim))

    @staticmethod
    def _sanitize_root(anim):
        """CoD root motion is planar: height lives in j_mainroot. Several Greyhound exports
        carry corrupt tag_origin tracks (hundreds of metres, all-zero quaternions), so keep
        root XY and rotation only when they are physically plausible."""
        root = anim['tracks'].get('tag_origin')
        if not root:
            return ''
        notes = []
        loc = root.get('loc') or []
        if loc:
            planar = [(f, (v[0], v[1], 0.0)) for f, v in loc]
            steps = [math.hypot(b[1][0]-a[1][0], b[1][1]-a[1][1]) / max(1, b[0]-a[0])
                     for a, b in zip(planar, planar[1:])]
            if any(abs(c) > 1500.0 for _, v in planar for c in v[:2]) or any(s > 60.0 for s in steps):
                root['loc'] = []
                notes.append('corrupt root translation dropped')
            else:
                root['loc'] = planar
        rot = root.get('rot') or []
        if rot and any(math.sqrt(sum(c * c for c in q)) < 0.9 for _, q in rot):
            root['rot'] = []
            notes.append('corrupt root rotation dropped')
        return '; '.join(notes)

    def animdesc(self, index):
        stem, anim = self.clips[index]
        return {'name': stem, 'fps': anim['fps'], 'frames': anim['frames']}

    def model_pose(self, index, frame):
        _, anim = self.clips[index]
        tracks = anim['tracks']
        relative = anim['type'] == 'relative'
        positions, rotations = {}, {}
        for i, bone in enumerate(self.bones):
            track = tracks.get(bone['name'], {})
            quat = bone['quat']
            if track.get('rot'):
                key = _sample(track['rot'], frame, True)
                quat = qmul(bone['quat'], key) if relative else key
            position = bone['pos']
            if track.get('loc'):
                offset = _sample(track['loc'], frame, False)
                position = tuple(position[k] + offset[k] * self.scale for k in range(3))
            parent = bone['parent']
            if parent < 0:
                positions[i], rotations[i] = position, qnorm(quat)
            else:
                turned = qrot(rotations[parent], position)
                positions[i] = tuple(positions[parent][k] + turned[k] for k in range(3))
                rotations[i] = qnorm(qmul(rotations[parent], quat))
        return positions, rotations


# ---------------------------------------------------------------------------
# Call of Duty Cast donor (DTZxPorter Cast, Modern Warfare 2019 exports)
# ---------------------------------------------------------------------------

CAST_NODE_ANIMATION, CAST_NODE_CURVE = 0x6D696E61, 0x76727563   # 'anim', 'curv'
CAST_VALUE_TYPES = {0x62: ('<B', 1), 0x68: ('<H', 2), 0x69: ('<I', 4), 0x6C: ('<Q', 8),
                    0x66: ('<f', 4), 0x64: ('<d', 8), 0x7632: ('<2f', 8), 0x7633: ('<3f', 12),
                    0x7634: ('<4f', 16)}


def _cast_node(d, o):
    ident, size, _, prop_count, child_count = struct.unpack_from('<IIQII', d, o)
    start, o = o, o + 24
    props = {}
    for _ in range(prop_count):
        kind, name_size, count = struct.unpack_from('<HHI', d, o)
        o += 8
        name = d[o:o + name_size].decode('ascii', 'replace')
        o += name_size
        if kind == 0x73:  # strings, NUL-terminated
            values = []
            for _ in range(count):
                end = d.index(b'\0', o)
                values.append(d[o:end].decode('ascii', 'replace'))
                o = end + 1
        elif kind in CAST_VALUE_TYPES:
            fmt, step = CAST_VALUE_TYPES[kind]
            values = [struct.unpack_from(fmt, d, o + i * step) for i in range(count)]
            values = [v[0] if len(v) == 1 else v for v in values]
            o += count * step
        else:
            raise ValueError('unknown Cast property type 0x%x' % kind)
        props[name] = values
    children = []
    for _ in range(child_count):
        child, o = _cast_node(d, o)
        children.append(child)
    return (ident, props, children), start + size


def read_cast(path):
    """Cast animation as the SEAnim-style clip read_seanim returns. Modern Warfare 2019 exports
    key rotations ('rq') as absolute bone-local quaternions and translations ('tx','ty','tz') as
    'relative' offsets from the rest pose: the same layout as SEAnim 'delta', in centimetres."""
    with open(path, 'rb') as handle:
        d = handle.read()
    if d[:4] != b'cast':
        raise ValueError('%s is not a Cast file' % path)
    _, _, roots, _ = struct.unpack_from('<IIII', d, 0)
    o, nodes = 16, []
    for _ in range(roots):
        node, o = _cast_node(d, o)
        nodes.append(node)
    anims = []
    while nodes:
        ident, props, children = nodes.pop()
        if ident == CAST_NODE_ANIMATION:
            anims.append((props, children))
        nodes += children
    if len(anims) != 1:
        raise ValueError('%s: expected one animation, found %d' % (path, len(anims)))
    props, curves = anims[0]
    tracks, axes, frames = {}, {}, 0
    for ident, curve, _ in curves:
        if ident != CAST_NODE_CURVE:
            continue
        bone, key, mode = curve['nn'][0], curve['kp'][0], curve['m'][0]
        keys = list(zip(curve['kb'], curve['kv']))
        frames = max(frames, max(curve['kb']) + 1)
        if key == 'rq':
            if mode != 'absolute':
                raise ValueError('%s: %s rotation mode %s is not supported' % (path, bone, mode))
            tracks.setdefault(bone, {})['rot'] = sorted((f, tuple(q)) for f, q in keys)
        elif key in ('tx', 'ty', 'tz'):
            if mode != 'relative':
                raise ValueError('%s: %s translation mode %s is not supported' % (path, bone, mode))
            axes.setdefault(bone, {})['xyz'.index(key[1])] = sorted((f, (v,)) for f, v in keys)
    for bone, per_axis in axes.items():
        at = sorted({f for keys in per_axis.values() for f, _ in keys})
        tracks.setdefault(bone, {})['loc'] = [
            (f, tuple(_sample(per_axis[k], f, False)[0] if k in per_axis else 0.0 for k in range(3)))
            for f in at]
    return {'type': 'delta', 'fps': float(props['fr'][0]), 'frames': frames, 'tracks': tracks}


# Modern Warfare 2019 names are long ('sdr_mp_exposed_crouch_death01_torso_med_8_center'); the
# pack holds 31 characters, so they are shortened step by step until they fit.
MW2019_SHORTEN = [('sdr_mp_', ''), ('iw7_mp_', ''), ('hm_', ''), ('mp_', ''), ('exposed_', ''),
                  ('strafe_jog_', 'jog'), ('_death0', '_d'), ('_death_', '_'), ('_death', ''),
                  ('seat0', 'seat'), ('_center', '_c'), ('_left', '_l'), ('_right', '_r'),
                  ('_front', '_f'), ('_back', '_b'), ('_inward', '_in'), ('_outward', '_out'),
                  ('explosion', 'expl'), ('explode', 'expl'), ('_impact', ''), ('electrocution', 'shock')]


def mw2019_clip_name(stem):
    name = stem
    for old, new in MW2019_SHORTEN:
        if len('mw_' + name) <= NAME_BYTES - 1:
            break
        name = name.replace(old, new)
    name = 'mw_' + name
    if len(name) > NAME_BYTES - 1:
        raise ValueError('MW2019 clip name too long for the pack: %s' % name)
    return name


def mw2019_category(stem):
    """From the name: the context a death starts in first (ladder, vehicle seat, skydive, mid-air
    or prone starts cannot be a standing CS2 death), then the cause, then the hit location."""
    if any(t in stem for t in ('ladder', 'seat', 'skydive', 'inair', 'prone', 'electrocution')):
        return 'manual'
    if 'explode' in stem or 'explosion' in stem:
        return 'explosion'
    if 'fire' in stem:
        return 'fire'
    if '_head' in stem:
        return 'head'
    if 'lleg' in stem or 'leg_left' in stem:
        return 'leftleg'
    if 'rleg' in stem or 'leg_right' in stem:
        return 'rightleg'
    if '_leg_' in stem:
        return 'leg'   # a leg hit with no side in the name: mw2019_donor reads it from the motion
    if 'torso' in stem or 'gut' in stem:
        return 'torso'
    return 'generic'


def collapse_side(donor, anim):
    """'left' or 'right': the side the upper body falls toward over the first half of the clip,
    across the frame-0 hips. A leg-shot death collapses toward the leg that gave way; on the
    MW2019 clips whose name states the leg this agrees about seven times in ten (the digit in
    '_leg_lg_8' is the shot direction, not the leg)."""
    b = donor.bone_by_name
    start, _ = donor.model_pose(anim, 0)
    later, _ = donor.model_pose(anim, max(1, donor.animdesc(anim)['frames'] // 2))
    left = [start[b['j_hip_le']][k] - start[b['j_hip_ri']][k] for k in range(2)]
    chest = [later[b['j_spine4']][k] - start[b['j_spine4']][k] for k in range(2)]
    return 'left' if chest[0] * left[0] + chest[1] * left[1] >= 0 else 'right'


def mw2019_donor(source_dir):
    """Every Modern Warfare 2019 death in the source folder, posed on its player rest skeleton."""
    anim_dir = os.path.join(source_dir, 'anims')
    stems = sorted(f[:-5] for f in os.listdir(anim_dir) if f.endswith('.cast'))
    names = [mw2019_clip_name(stem) for stem in stems]
    if len(set(names)) != len(names):
        clashes = sorted({n for n in names if names.count(n) > 1})
        raise SystemExit('MW2019 clip names collide after shortening: %s' % ', '.join(clashes))
    donor = SEAnimDonor(os.path.join(source_dir, 'mw2019-player-skeleton.json'), anim_dir,
                        list(zip(names, stems)), reader=read_cast, extension='.cast')
    clips = []
    for name, stem in zip(names, stems):
        category = mw2019_category(stem)
        if category == 'leg':
            category = collapse_side(donor, donor.sequences[name]) + 'leg'
        clips.append((category, name, name))
    return donor, clips


# ---------------------------------------------------------------------------
# Retarget table and clip selection
# ---------------------------------------------------------------------------

# CS2 body order is AdvancedRagdollMath::Cs2AgentRagdollBoneName. Keep both in step.
CS2_BODY_ORDER = [
    ('leg_upper_l', 'ValveBiped.Bip01_L_Thigh'),
    ('leg_lower_l', 'ValveBiped.Bip01_L_Calf'),
    ('leg_upper_r', 'ValveBiped.Bip01_R_Thigh'),
    ('leg_lower_r', 'ValveBiped.Bip01_R_Calf'),
    ('head_0', 'ValveBiped.Bip01_Head1'),
    ('pelvis', 'ValveBiped.Bip01_Pelvis'),
    ('spine_2', 'ValveBiped.Bip01_Spine2'),
    ('arm_upper_l', 'ValveBiped.Bip01_L_UpperArm'),
    ('arm_lower_l', 'ValveBiped.Bip01_L_Forearm'),
    ('arm_upper_r', 'ValveBiped.Bip01_R_UpperArm'),
    ('arm_lower_r', 'ValveBiped.Bip01_R_Forearm'),
    ('hand_l', 'ValveBiped.Bip01_L_Hand'),
    ('hand_r', 'ValveBiped.Bip01_R_Hand'),
    ('ankle_l', 'ValveBiped.Bip01_L_Foot'),
    ('ankle_r', 'ValveBiped.Bip01_R_Foot'),
]
BODY_COUNT = len(CS2_BODY_ORDER)
# The same fifteen bodies on the Call of Duty player skeleton. j_spine4 is the ribcage the
# clavicles hang from, which is what CS2's spine_2 body carries.
COD_BODY_BONES = ['j_hip_le', 'j_knee_le', 'j_hip_ri', 'j_knee_ri', 'j_head', 'j_mainroot',
                  'j_spine4', 'j_shoulder_le', 'j_elbow_le', 'j_shoulder_ri', 'j_elbow_ri',
                  'j_wrist_le', 'j_wrist_ri', 'j_ankle_le', 'j_ankle_ri']

# Category ids are the runtime enum in DeathAnimationsMath.h. Order is load-bearing. 'moving' is
# retired (speed is the running stance) but keeps its id; 'melee' is knife and other melee kills.
CATEGORY_IDS = ['generic', 'head', 'neck', 'torso', 'leftarm', 'rightarm',
                'leftleg', 'rightleg', 'explosion', 'moving', 'fire', 'melee', 'manual']

# Sequence names copied from the donor's own selection tables (sv_sdohnahyi.lua): the mod author
# already grouped these by what killed the character, so we keep that grouping rather than
# reinventing one from the file names.
# These are candidate lists, not a curated set. Membership decides which hitgroup a sequence
# answers; the collapse filter below decides whether it is good enough to ship, so a donor
# sequence that turns out to be a stagger rather than a death drops out on its own.
CLIP_SETS = {
    'generic': ['Death_01', 'Death_02a', 'Death_02c', 'Death_03', 'Death_05', 'Death_06',
                'Death_07', 'Death_08', 'Death_08b', 'Death_09', 'Death_10ab', 'Death_10b',
                'Death_10c', 'Death_11_01a', 'Death_11_01b', 'Death_11_02a', 'Death_11_02b',
                'Death_11_02c', 'Death_11_02d', 'Death_11_03a', 'Death_11_03b',
                'Death_11_03c', 'dying1', 'dying2', 'dying3', 'dying4', 'dying5', 'dying6',
                'dying7'],
    'melee': ['club1', 'club2', 'club3', 'club4', 'bd_death_slasher_front',
             'bd_death_slasher_left', 'bd_death_slasher_right', 'bd_death_slasher_back'],
    # bd_death_head_03 is byte-identical to bd_death_head_01 in the donor, so it is left out.
    'head': ['bd_death_head_01', 'bd_death_head_02', 'bd_death_head_04',
             'bd_death_head_05', 'bd_death_head_07', 'bd_death_head_08',
             'bd_death_head_multi_01', 'bd_death_head_multi_02', 'bd_death_head_multi_03',
             'bd_death_head_single_01', 'bd_death_head_single_02', 'bd_death_head_single_03',
             'bd_death_head_short_01', 'bd_death_head_short_02', 'bd_death_head_short_03'],
    'neck': ['bd_death_neck_short_01', 'bd_death_neck_short_02', 'bd_death_neck_short_03',
             'bd_death_neck_short_04'],
    'torso': ['bd_death_torso_long_01', 'bd_death_torso_long_02', 'bd_death_torso_long_03']
             + ['bd_death_torso_short_%02d' % n for n in range(1, 22)]
             + ['bd_death_stomach_multi_01', 'bd_death_stomach_single_01',
                'bd_death_stomach_single_02', 'bd_death_stomach_short_01',
                'bd_death_stomach_short_02']
             + ['cod_1_torso_%d' % n for n in range(1, 8)],
    'leftarm': ['bd_death_leftarm_multi_01', 'bd_death_leftarm_multi_02',
                'bd_death_leftarm_multi_03', 'bd_death_leftarm_multi_04',
                'bd_death_leftarm_single_01', 'bd_death_leftarm_single_02',
                'bd_death_leftarm_single_03', 'bd_death_leftarm_short_01',
                'bd_death_leftarm_short_02', 'bd_death_leftarm_short_03'],
    'rightarm': ['bd_death_rightarm_01', 'bd_death_rightarm_02', 'bd_death_rightarm_multi_01',
                 'bd_death_rightarm_multi_02', 'bd_death_rightarm_single_01',
                 'bd_death_rightarm_single_02', 'bd_death_rightarm_single_03',
                 'bd_death_rightarm_single_04'],
    # bd_death_leg_* / bd_death_legs_01 are unsided in the donor; each is filed by the knee
    # that buckles first (mean bend over the first 45%), near-ties split to balance the sides.
    'leftleg': ['bd_death_leftleg_long_01', 'bd_death_leftleg_long_02']
               + ['bd_death_leftleg_short_%02d' % n for n in range(1, 9)]
               + ['bd_death_leg_01', 'bd_death_leg_02', 'bd_death_leg_03', 'bd_death_leg_04',
                  'bd_death_leg_07'],
    'rightleg': ['bd_death_rightleg_multi_01', 'bd_death_rightleg_multi_02',
                 'bd_death_rightleg_multi_03', 'bd_death_rightleg_short_01',
                 'bd_death_rightleg_short_02']
                + ['bd_death_rightleg_single_%02d' % n for n in range(1, 6)]
                + ['bd_death_leg_05', 'bd_death_leg_06', 'bd_death_leg_08', 'bd_death_legs_01'],
    # The donor addon also filed DeathRunning_09/10/11a/11b/11c under explosion; they are
    # running deaths and live in 'moving'.
    'explosion': ['DeathExplosion_%02d' % n for n in range(1, 9)],
    'moving': ['DeathRunning_01', 'DeathRunning_03', 'DeathRunning_04', 'DeathRunning_05',
               'DeathRunning_06', 'DeathRunning_07', 'DeathRunning_08', 'DeathRunning_11d',
               'DeathRunning_11f', 'DeathRunning_11g', 'DeathRunning_12', 'DeathRunning_13',
               'DeathRunning_14', 'DeathRunning_15', 'DeathRunning_16', 'DeathRunning_11e',
               'DeathRunning_09', 'DeathRunning_10', 'DeathRunning_11a', 'DeathRunning_11b',
               'DeathRunning_11c'],
    # GMod fire deaths (sv_sdohnahyi.lua type "fire") — not grenade throws.
    'fire': ['bd_death_fire1'],
    # Deaths that begin on the ground. They ship for forced playback only (see MANUAL below).
    'manual': ['crawling%d' % n for n in range(1, 8)],
}

# Call of Duty player deaths (World at War, Black Ops 1/2, Modern Warfare 3; Greyhound SEAnim
# exports in FX/sources/cod-deaths/anims). Grouped by manner of death first, then hit location,
# matching CLIP_SETS. Shotgun deaths are chest hits: the runtime has no shotgun category.
# Flybacks are blast deaths. 'Explosive round' deaths are explosive-BULLET hits (CS2 has no such
# ammo), so they are filed by where they hit: chest -> torso, jaw -> head, leg -> leg.
# Unsided leg deaths are filed by the knee that buckles first (measured over the first 45%).
COD_CLIP_SETS = {
    'generic': ['pb_crouch_death_falltohands', 'pb_crouch_death_flip', 'pb_crouch_martyrdom_death',
                'pb_death_armslegsforward', 'pb_death_armslegsforward_b', 'pb_death_faceplant',
                'pb_death_flatonback', 'pb_death_forwardtoface', 'pb_death_jackiespin_inplace',
                'pb_death_jackiespin_left', 'pb_death_jackiespin_right',
                'pb_death_jackiespin_vertical', 'pb_death_legsout_left', 'pb_death_legsout_right',
                'pb_death_upontoback', 'pb_stand_death_frontspin', 'pb_stand_death_kickup',
                'pb_stand_death_nervedeath', 'pb_stand_death_stumbleforward',
                'pb_stand_death_tumbleback', 'pb_stand_martyrdom_death'],
    'head': ['pb_crouch_death_headshot_back', 'pb_crouch_death_headshot_front',
             'pb_crouch_death_headshot_left', 'pb_crouch_death_headshot_right',
             'pb_death_headshot_back', 'pb_death_headshot_front', 'pb_death_headshot_front_v2',
             'pb_death_headshot_left', 'pb_death_headshot_right', 'pb_stand_death_head_collapse',
             'pb_stand_death_head_straight_back', 'pb_stand_death_headchest_topple',
             'pb_stand_death_headshot_slowfall', 'pb_explosive_round_death_jaw'],
    'neck': ['pb_stand_death_neckdeath', 'pb_stand_death_neckdeath_thrash'],
    'torso': ['pb_crouch_death_clutchchest', 'pb_crouch_death_fetal',
              'pb_crouch_death_shotgun_back', 'pb_crouch_death_shotgun_front',
              'pb_crouch_death_shotgun_left', 'pb_crouch_death_shotgun_right',
              'pb_death_shotgun_back',
              'pb_death_shotgun_front', 'pb_death_shotgun_left', 'pb_death_shotgun_right',
              'pb_shotgun_death_back1', 'pb_shotgun_death_back2', 'pb_shotgun_death_front',
              'pb_shotgun_death_spinl', 'pb_shotgun_death_spinr', 'pb_stand_death_chest_blowback',
              'pb_stand_death_chest_spin', 'pb_stand_death_lowerback',
              'pb_stand_death_shoulder_stumble', 'pb_stand_death_shoulderback',
              'pb_explosive_round_death_chesta', 'pb_explosive_round_death_chestb'],
    'leftleg': ['pb_stand_death_leg', 'pb_stand_death_leg_kickup'],
    'rightleg': ['pb_stand_death_legs', 'pb_shotgun_death_legs', 'pb_explosive_round_death_leg'],
    'melee': ['pb_crouch_death_melee_back', 'pb_crouch_death_melee_front',
             'pb_crouch_death_melee_left', 'pb_crouch_death_melee_right',
             'pb_crouch_death_shieldbash_back', 'pb_crouch_death_shieldbash_front',
             'pb_crouch_death_shieldbash_left', 'pb_crouch_death_shieldbash_right',
             'pb_death_melee_back', 'pb_death_melee_back_v2', 'pb_death_melee_front',
             'pb_death_melee_left', 'pb_death_melee_right', 'pb_death_shieldbash_back',
             'pb_death_shieldbash_front', 'pb_death_shieldbash_left', 'pb_death_shieldbash_right'],
    'moving': ['pb_crouchrun_death_crumple', 'pb_crouchrun_death_drop', 'pb_death_run_back',
               'pb_death_run_forward_crumple', 'pb_death_run_left', 'pb_death_run_onfront',
               'pb_death_run_right', 'pb_death_run_stumble'],
    'explosion': ['pb_explosion_death_b1', 'pb_explosion_death_b2', 'pb_explosion_death_b3',
                  'pb_explosion_death_f1', 'pb_explosion_death_l1', 'pb_explosion_death_r1', 'pb_explosion_rundeath_b1', 'pb_explosion_rundeath_b2',
                  'pb_explosion_rundeath_f1', 'pb_explosion_rundeath_f2',
                  'pb_explosion_rundeath_f3', 'pb_explosion_rundeath_l1',
                  'pb_explosion_rundeath_r1', 'pb_death_flyback', 'pb_death_flyback_far'],
    # Not deaths a standing CS2 player can have: they start on the ground (last stand, prone),
    # start mid-air (vertigo fall) or never go down (pf_death). pb_explosion_death_l2's exported
    # root track alternates between two impossible positions every frame; without it the body
    # slams down at ~30 m/s and the real arc cannot be recovered.
    'manual': ['pb_death_falling_vertigo', 'pb_laststand_death', 'pb_laststand_suicide_death',
               'pb_prone_death_quickdeath', 'pb_prone_martyrdom_death', 'pf_death',
               'pb_explosion_death_l2'],
}


def cod_clip_name(stem):
    """'pb_death_flyback' -> 'cod_death_flyback'. Pack names hold 31 characters, so longer
    ones drop their first '_death': 'pb_stand_death_head_straight_back' -> 'cod_stand_head_straight_back'."""
    name = 'cod_' + (stem[3:] if stem.startswith(('pb_', 'pf_')) else stem)
    if len(name) > NAME_BYTES - 1:
        name = name.replace('_death', '', 1)
    if len(name) > NAME_BYTES - 1:
        raise ValueError('CoD clip name too long for the pack: %s' % name)
    return name

MAX_FRAMES = 4096        # decoder bound; never silently truncate a death mid-performance
# The donor addon plays these at 0.5-1.0x (hari_ragdoll_animation/init.lua SetPlaybackRate)
# and runs the whole cycle before releasing, so a clip has to carry enough frames for a
# full-length death at half speed. Baking 1.6 s was why deaths looked rushed and cut off.
OUTPUT_FPS = 30.0
PELVIS_BODY = 5          # index into CS2_BODY_ORDER
MIN_PELVIS_DROP = 12.0   # units the hips must lose before a clip counts as a collapse
MAX_RESTING_PELVIS = 16.0  # ...or the pelvis height a clip may end at (crouch-start deaths)
HANDOFF_FRAMES = 45       # ...or it is a short opening beat that native physics finishes
MAGIC = b'MVMDANIM'
FORMAT_VERSION = 9       # per-clip source, category, hit side, stance and WEAPON; all donors in one file
# Clip source ids, order matching DeathAnimationsMath::Source.
SOURCE_MW2019, SOURCE_CLASSIC, SOURCE_EXTRA = 0, 1, 2
SOURCE_NAMES = ['MW2019', 'Classic CoD', 'Extra']


def legacy_source(name):
    """The v8 runtime's name-prefix source classification. Donor-assigned sources must
    reproduce it exactly; main() refuses to write the pack when one does not."""
    if name.startswith('mw_'):
        return SOURCE_MW2019
    if name.startswith('cod_') and not name.startswith('cod_1_'):
        return SOURCE_CLASSIC
    return SOURCE_EXTRA
# Order must match DeathAnimationsMath::HitSide.
HIT_SIDES = ['any', 'front', 'back', 'left', 'right']
# Order must match DeathAnimationsMath::Stance. A crouched victim only ever gets crouched clips and
# a running one only running clips.
STANCES = ['standing', 'crouched', 'prone', 'running']
RUNNING_NAME = re.compile(r'run|jog|sprint')
# Order must match DeathAnimationsMath::WeaponClass. 'any' is universal; weapon clips are tried
# first for kills with that weapon. MW2019 names every hit as '_lg' (large calibre) or '_med'; a
# large-calibre hit is 'heavy', which answers both shotgun and sniper kills.
WEAPONS = ['any', 'shotgun', 'sniper', 'heavy']
NAMED_SIDE = re.compile(r'(headshot|shotgun|melee|shieldbash|slasher)_(front|back|left|right)(_v2)?$')
# Modern Warfare 2019 names end in a numpad direction for where the hit came from: 8 front,
# 2 back, 4 left, 6 right (optionally followed by _center/_left/_right, the fall variant).
# Measured against the fall direction the numbers agree for most clips; the name is authored.
MW_NUMPAD_SIDE = re.compile(r'^mw_.*_([2468])(?:_(?:center|left|right))?$')
MW_NUMPAD = {'8': 'front', '2': 'back', '4': 'left', '6': 'right'}
CROUCH_PELVIS = 30.0     # frame-0 pelvis height below this is a crouch start (standing is 34-42)
# Per-clip corrections made in the Blender viewer (Death Anims panel); they override every guess.
CLIP_TAGS = os.path.join('FX', 'death-animations', 'clip-tags.json')
IN_PLACE_DISTANCE = 10.0  # upper-body travel below this is a crumple in place (side 'any')
NAME_BYTES = 32


def bake_clip(mdl, bone_ids, anim, offsets=None):
    """Frame 0 pose, plus per-frame displacement and ABSOLUTE model-space rotation.

    The frame 0 pose is kept in clip space for facing alignment at runtime. Positions
    use native CS2 joint anchors; applying a posture-derived scale would disconnect them.

    Rotations are absolute, not deltas, because the donor addon drives each physics object to
    the carrier bone's own world position AND angle (GetBonePosition -> ComputeShadowControl).
    Storing a delta would mean reconstructing that absolute orientation at runtime from data we
    do not keep.
    """
    desc = mdl.animdesc(anim)
    if desc['frames'] < 2:
        return None, None, desc
    # Donor clips are authored at 30 fps; resample if a future donor is not.
    step = max(1.0, desc['fps'] / OUTPUT_FPS)
    count = int(math.ceil((desc['frames'] - 1) / step)) + 1
    if count > MAX_FRAMES:
        raise ValueError('clip exceeds decoder frame bound: %s' % desc['name'])
    if count < 2:
        return None, None, desc
    if offsets is None: offsets = rotation_offsets(mdl, bone_ids)
    child_offsets = cs2_child_offsets()
    joints = cs2_joints()
    bend_signs = cs2_knee_bend_signs(joints)
    base = None
    root_offset = None
    frames = []
    for f in range(count):
        positions, rotations = mdl.model_pose(anim, int(round(f * step)))
        target_rotations = []
        for i,bone in enumerate(bone_ids):
            rotation = qnorm(qmul(rotations[bone], offsets[i]))
            if i in RIGID_CHILD:
                # The donor compiles clips from several rigs with slightly different local
                # child axes. Preserve calibrated roll, but align each rigid segment to its
                # actual animated endpoints so position and orientation cannot fight.
                direction = [positions[bone_ids[RIGID_CHILD[i]]][k]-positions[bone][k] for k in range(3)]
                rotation = qnorm(qmul(swing_between(qrot(rotation,child_offsets[i]),direction),rotation))
            target_rotations.append(rotation)
        legs = {child: tuple(positions[bone_ids[i]] for i in chain)
                for child, chain in ((1, (0, 1, 13)), (3, (2, 3, 14)))}
        fit_knee_hinges(target_rotations, joints, legs, bend_signs, child_offsets)
        target_positions = articulated_positions(positions[bone_ids[PELVIS_BODY]],target_rotations,joints)
        if root_offset is None:
            # Carrier XY is the captured pelvis XY. Plant the reference ankles at their
            # native standing height; keep this translation constant throughout the clip.
            reference = cs2_reference()
            ankle_height = min(reference[i]['bind'][11] for i in (13,14))
            root_offset = (-target_positions[PELVIS_BODY][0], -target_positions[PELVIS_BODY][1],
                           ankle_height-min(target_positions[i][2] for i in (13,14)))
        target_positions = [tuple(p[k]+root_offset[k] for k in range(3)) for p in target_positions]
        if base is None: base = target_positions
        row = [tuple(target_positions[i][k]-base[i][k] for k in range(3))+target_rotations[i]
               for i in range(BODY_COUNT)]
        frames.append(row)
    return base, frames, desc


def collapses(base, frames):
    """A death goes down: the pelvis drops far enough, ends close to the floor, or the clip is a
    short opening beat (the DeathRunning_* stumbles) that hands the fall to native physics."""
    drop = -min(row[PELVIS_BODY][2] for row in frames)
    resting = base[PELVIS_BODY][2] + frames[-1][PELVIS_BODY][2]
    return drop >= MIN_PELVIS_DROP or resting <= MAX_RESTING_PELVIS or len(frames) <= HANDOFF_FRAMES


def hit_side(name, base, frames):
    """The side of the victim this death answers. A name that states the hit side wins (CoD
    headshot/shotgun/melee/shield-bash, GMod slasher); otherwise the upper body's travel over
    the clip, relative to the frame-0 facing: falling forward means hit from behind."""
    named = NAMED_SIDE.search(name)
    if named:
        return named.group(2)
    numpad = MW_NUMPAD_SIDE.search(name)
    if numpad:
        return MW_NUMPAD[numpad.group(1)]
    at = lambda f, b: [base[b][k] + frames[f][b][k] for k in range(3)]
    left_hip, right_hip = at(0, 0), at(0, 2)
    lx, ly = left_hip[0] - right_hip[0], left_hip[1] - right_hip[1]
    norm = math.hypot(lx, ly)
    if norm < 1e-3:
        return 'any'
    forward, left = (ly / norm, -lx / norm), (lx / norm, ly / norm)
    start = [(at(0, 4)[k] + at(0, 6)[k]) / 2 for k in range(2)]
    end = [(at(-1, 4)[k] + at(-1, 6)[k]) / 2 for k in range(2)]
    travel = (end[0] - start[0], end[1] - start[1])
    ahead = travel[0] * forward[0] + travel[1] * forward[1]
    aside = travel[0] * left[0] + travel[1] * left[1]
    if math.hypot(ahead, aside) < IN_PLACE_DISTANCE:
        return 'any'
    angle = math.degrees(math.atan2(aside, ahead))
    if -45 <= angle <= 45:
        return 'back'
    if angle >= 135 or angle <= -135:
        return 'front'
    return 'right' if angle > 0 else 'left'   # falls to its left: hit from the right


def stance(name, base):
    """How the character stands at frame 0. The name wins when it says so; otherwise the pelvis
    height, since the bake plants every clip's ankles at the native standing height."""
    lowered = name.lower()
    if 'prone' in lowered or 'crawling' in lowered:
        return 'prone'
    if 'crouch' in lowered:
        return 'crouched'   # crouch-runs too: a crouching CS2 player never reaches running speed
    if RUNNING_NAME.search(lowered):
        return 'running'
    if 'stand' in lowered:
        return 'standing'
    return 'crouched' if base[PELVIS_BODY][2] < CROUCH_PELVIS else 'standing'


def weapon(name):
    """Which weapon a death is for, from its name: CoD shotgun deaths and the shotgun blast
    flybacks; MW2019 large-calibre hits are heavy; everything else is universal."""
    lowered = name.lower()
    if 'shotgun' in lowered or 'flyback' in lowered:
        return 'shotgun'
    if 'sniper' in lowered:
        return 'sniper'
    if lowered.startswith('mw_') and re.search(r'_lg(_|$)', lowered):
        return 'heavy'
    return 'any'


def load_clip_tags(root):
    """{clip name: {"category"|"side"|"stance": value}} from the Blender viewer's corrections."""
    path = os.path.join(root, CLIP_TAGS)
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as handle:
        tags = json.load(handle)
    for name, fields in tags.items():
        for key, value in fields.items():
            allowed = {'category': CATEGORY_IDS, 'side': HIT_SIDES, 'stance': STANCES, 'weapon': WEAPONS}.get(key)
            if allowed is None or value not in allowed:
                raise SystemExit('%s: %s has a bad %s tag %r' % (CLIP_TAGS, name, key, value))
    return tags


def reference_span(mdl, bone_ids):
    """Legacy donor span metadata; retained in the header, unused by runtime placement."""
    positions = {}
    for i, info in enumerate(mdl.bones):
        parent = info['parent']
        if parent < 0:
            positions[i] = info['pos']
        else:
            turned = qrot(qnorm(mdl.bones[parent]['quat']), info['pos'])
            positions[i] = tuple(positions[parent][k] + turned[k] for k in range(3))
    pelvis, head = bone_ids[5], bone_ids[4]
    return math.sqrt(sum((positions[head][k] - positions[pelvis][k]) ** 2 for k in range(3)))


def find_source(explicit):
    candidates = [explicit] if explicit else []
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    candidates += [
        os.path.join(root, 'FX', 'sources', 'brutal-deaths', 'model_anim.mdl'),
        os.path.join(root, 'build', 'research', 'workshop-3173062409', 'models',
                     'brutal_deaths', 'model_anim.mdl'),
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    raise SystemExit('donor model_anim.mdl not found; pass --source (looked in %s)'
                     % ', '.join(c for c in candidates if c))


def cod_donor(source_dir):
    """The CoD clips listed in COD_CLIP_SETS, posed on the matching player rest skeleton."""
    anim_dir = os.path.join(source_dir, 'anims')
    present = {f[:-7] for f in os.listdir(anim_dir) if f.endswith('.seanim')}
    listed = [stem for stems in COD_CLIP_SETS.values() for stem in stems]
    unfiled = sorted(present - set(listed))
    if unfiled:
        raise SystemExit('CoD animations with no category: %s' % ', '.join(unfiled))
    missing = sorted(set(listed) - present)
    if missing:
        raise SystemExit('CoD animations listed but not found: %s' % ', '.join(missing))
    names = [cod_clip_name(stem) for stem in listed]
    if len(set(names)) != len(names):
        raise SystemExit('CoD clip names collide after shortening')
    return SEAnimDonor(os.path.join(source_dir, 'cod-player-skeleton.json'), anim_dir,
                       list(zip(names, listed)))


def main(argv=None):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--source', help='path to the donor model_anim.mdl')
    parser.add_argument('--cod', default=os.path.join(root, 'FX', 'sources', 'cod-deaths'),
                        help='folder holding cod-player-skeleton.json and anims/*.seanim')
    parser.add_argument('--out', default=os.path.join(
        root, 'FX', 'death-animations', 'ragdoll_animations.mvmanim'))
    parser.add_argument('--mw2019-source', default=os.path.join(root, 'FX', 'sources', 'mw2019-deaths'),
                        help='folder holding mw2019-player-skeleton.json and anims/*.cast')
    parser.add_argument('--report', action='store_true', help='print per-clip detail')
    args = parser.parse_args(argv)

    source = find_source(args.source)
    mdl = StudioModel(source)
    cod = cod_donor(args.cod)
    mw, mw2019_clips = mw2019_donor(args.mw2019_source)
    donors = [(mdl, [name for _, name in CS2_BODY_ORDER], SOURCE_EXTRA,
               [(category, name, name) for category, names in CLIP_SETS.items() for name in names]),
              (cod, COD_BODY_BONES, SOURCE_CLASSIC,
               [(category, cod_clip_name(stem), cod_clip_name(stem))
                for category, stems in COD_CLIP_SETS.items() for stem in stems]),
              (mw, COD_BODY_BONES, SOURCE_MW2019, mw2019_clips)]

    # Clips deleted in the Blender viewer (Death Anims > Delete) stay out of every bake.
    removed_path = os.path.join(root, 'FX', 'death-animations', 'removed-clips.txt')
    removed = set()
    if os.path.exists(removed_path):
        with open(removed_path, encoding='utf-8') as handle:
            removed = {line.strip() for line in handle if line.strip() and not line.startswith('#')}
    donors = [(donor, bones, clip_source, [c for c in clip_list if c[1] not in removed])
              for donor, bones, clip_source, clip_list in donors]

    tags = load_clip_tags(root)
    clips, skipped, demoted = [], [], []
    for donor, body_bones, clip_source, clip_list in donors:
        missing = [name for name in body_bones if name not in donor.bone_by_name]
        if missing:
            raise SystemExit('donor skeleton is missing required bones: %s' % ', '.join(missing))
        bone_ids = [donor.bone_by_name[name] for name in body_bones]
        offsets = rotation_offsets(donor, bone_ids)
        for category, name, key in clip_list:
            anim = donor.sequences.get(key)
            if anim is None:
                skipped.append((name, 'no such sequence'))
                continue
            base, frames, desc = bake_clip(donor, bone_ids, anim, offsets)
            if frames is None:
                skipped.append((name, 'too short (%d frames)' % desc['frames']))
                continue
            travel = max(math.sqrt(sum(c * c for c in frames[-1][b][:3]))
                         for b in range(BODY_COUNT))
            # Several donor sequences are the first beat of a death -- a clutch or a stagger
            # meant to hand off to something else -- and the character never actually goes
            # down inside them. Guiding a CS2 corpse with one of those holds it upright, which
            # reads as a bug, so they ship as manual clips that are never picked automatically.
            # Crouch-start deaths go down from a lower pelvis, so ending near the floor counts.
            if category != 'manual' and not collapses(base, frames):
                drop = -min(row[PELVIS_BODY][2] for row in frames)
                demoted.append((name, category, drop))
                category = 'manual'
            # Speed is the running STANCE now, not a body area: a running death is filed by where
            # the hit landed (from its name where it says so) and generic otherwise.
            if category == 'moving':
                category = 'generic'
            fixed = tags.get(name, {})
            clips.append({'name': name, 'source': clip_source,
                          'category': CATEGORY_IDS.index(fixed.get('category', category)),
                          'side': HIT_SIDES.index(fixed.get('side', hit_side(name, base, frames))),
                          'stance': STANCES.index(fixed.get('stance', stance(name, base))),
                          'weapon': WEAPONS.index(fixed.get('weapon', weapon(name))),
                          'base': base, 'frames': frames, 'travel': travel})

    if not clips:
        raise SystemExit('no usable clips were baked')

    # The source tag is now the donor's word, but it must reproduce the old name-prefix rule
    # exactly: a clip that disagrees would silently change family at runtime, so stop instead.
    mismatched = [(c['name'], SOURCE_NAMES[c['source']], SOURCE_NAMES[legacy_source(c['name'])])
                  for c in clips if c['source'] != legacy_source(c['name'])]
    if mismatched:
        for name, donor_source, name_source in mismatched:
            print('source mismatch %-34s donor says %s, name rule says %s'
                  % (name, donor_source, name_source))
        raise SystemExit('%d clip source(s) disagree with the name-prefix rule; fix the donors '
                         'or the names before baking' % len(mismatched))

    payload = bytearray()
    table = bytearray()
    for clip in clips:
        table += clip['name'].encode('ascii')[:NAME_BYTES - 1].ljust(NAME_BYTES, b'\0')
        table += struct.pack('<IIIIIII', clip['source'], clip['category'], len(clip['frames']),
                             len(payload), clip['side'], clip['stance'], clip['weapon'])
        for bone in clip['base']:
            table += struct.pack('<3f', *bone)
        for row in clip['frames']:
            for bone in row:
                payload += struct.pack('<7f', *bone)

    span = reference_span(mdl, [mdl.bone_by_name[name] for _, name in CS2_BODY_ORDER])
    header = MAGIC + struct.pack('<IIIff', FORMAT_VERSION, len(clips), BODY_COUNT, OUTPUT_FPS, span)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as handle:
        handle.write(header)
        handle.write(table)
        handle.write(payload)

    total_frames = sum(len(c['frames']) for c in clips)
    print('donor      %s (mdl v%d, %d bones, %d sequences)'
          % (source, mdl.version, mdl.num_bones, mdl.num_seqs))
    print('donor      %s (%d SEAnim clips, %d bones)' % (args.cod, len(cod.clips), len(cod.bones)))
    print('donor      %s (%d Cast clips, %d bones)' % (args.mw2019_source, len(mw.clips), len(mw.bones)))
    print('baked      %d clips, %d frames, %d bodies -> %s'
          % (len(clips), total_frames, BODY_COUNT, args.out))
    print('sources    mw2019=%d classic=%d extra=%d'
          % (sum(c['source'] == SOURCE_MW2019 for c in clips),
             sum(c['source'] == SOURCE_CLASSIC for c in clips),
             sum(c['source'] == SOURCE_EXTRA for c in clips)))
    print('size       %.2f MB   reference span %.2f units'
          % (os.path.getsize(args.out) / 1048576.0, span))
    for donor in (cod, mw):
        for name, note in sorted(donor.notes.items()):
            if note:
                print('root       %-32s %s' % (name, note))
    for name, category, drop in demoted:
        print('manual     %-32s never collapses (%s; pelvis drops %.1f units)' % (name, category, drop))
    for name, reason in skipped:
        print('skipped    %-32s %s' % (name, reason))
    if args.report:
        for clip in clips:
            print('  %-34s %-10s %-10s %-6s %3d frames  travel %6.1f'
                  % (clip['name'], SOURCE_NAMES[clip['source']], CATEGORY_IDS[clip['category']],
                     HIT_SIDES[clip['side']], len(clip['frames']), clip['travel']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
