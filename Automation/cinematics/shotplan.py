"""Plan, check and preview camera-path shots offline, against the map collision and the demo track.

    python Automation/cinematics/shotplan.py plan    <shots.json> [--only NAME ...] [--frames N]
    python Automation/cinematics/shotplan.py scout   <shots.json> --subject NAME --ticks A-B
                                                     [--dist 300,600,900] [--height 40,120,260]
    python Automation/cinematics/shotplan.py probe   <shots.json> x y z
    python Automation/cinematics/shotplan.py install <shots.json> NAME
    python Automation/cinematics/shotplan.py restore <shots.json>

A shots file names the map, the demo track (FilmmakerDemoInfoGo/trackscan), the demo's
project.mvm and a list of shots. Each shot is a set of camera-path keys, the same markers the
Camera Editor places (tick, position, angles, FOV, ease), written either as absolute poses or
relative to a player:

    {"name": "ORBIT", "subject": "ultimate", "fps": 64, "range": [49370, 49420],
     "keys": [{"tick": 49370, "orbit": {"dist": 520, "azim": 200, "z": 70}, "fov": 45},
              {"tick": 49420, "pos": [900, -1500, -560], "aim": "player:JDC", "frameX": -0.3}]}

Key position: "pos" [x y z] (world), "offset" [dx dy dz] (from the subject's feet at that tick), or
"orbit" {dist, azim (world degrees, 0 = +x), z (above the feet)}. Aim: "subject" (default) at
"aimZ" above the feet, "player:NAME", a world point, or "angles" [pitch yaw]. "frameX"/"frameY"
(-1..1) place the aim point off centre, so the subject lands on a third instead of dead centre.

`plan` evaluates each shot exactly the way CamPathEval does (Manual speed, Live timing, Smooth
interp), then for every rendered frame reports wall clearance, frames inside solid geometry,
path segments that cut through a wall, how much of the subject the camera can see and where and
how big the subject sits in the 2.37:1 frame. It writes a top-down plan, a contact sheet of
flat-shaded previews from the camera and the markers JSON. `install` writes the shot's markers
into the demo's project.mvm (backing the original up once) so `mirv_filmmaker marker load`
picks them up; `restore` puts the backup back.
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.interpolate import CubicSpline
from scipy.spatial.transform import Rotation, RotationSpline, Slerp

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'lib'))
import mvm_cache  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpu_mesh  # noqa: E402

TICK_RATE = 64.0
ASPECT = 2560 / 1080
PLAYER_RADIUS = 16.0
CLEARANCE_WARN = 20.0      # closer than this to collision = the lens is about to clip a wall
ANGACC_WARN = 600          # deg/s^2: a steady operator's pan/tilt never changes speed faster than this
ACC_WARN = 2500            # u/s^2: dolly speed changes faster than this read as a bump
FONT = None


def font(size=14):
    try:
        return ImageFont.truetype('consola.ttf', size)
    except OSError:
        return ImageFont.load_default()


# ------------------------------------------------------------------ map collision

class Mesh:
    """Collision triangles of one map region, with a uniform grid for ray and range queries."""

    CELL = 128.0

    def __init__(self, tris):
        self.tris = tris.astype(np.float64)
        a, b, c = self.tris[:, 0], self.tris[:, 1], self.tris[:, 2]
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n, axis=1)
        keep = ln > 1e-6
        self.tris, n, ln = self.tris[keep], n[keep], ln[keep]
        self.normal = n / ln[:, None]
        self.lo = self.tris.min(1)
        self.hi = self.tris.max(1)
        # GPU (Warp/CUDA) BVH when available: every query below runs batched on it. The CPU grid is
        # only built for the fallback path.
        self.gpu = gpu_mesh.GpuMesh(self.tris) if gpu_mesh.AVAILABLE else None
        if self.gpu is not None:
            self.big = np.zeros(0, np.int64)
            self.cells = {}
            self._tri_of_cell = np.zeros(0, np.int64)
            return
        # Uniform grid, built vectorised: every (triangle, cell) pair its bounding box touches.
        # Triangles spanning more than BIG cells (huge floors / skybox walls) go in one list that every
        # query includes, so they cannot blow the pair count up.
        clo = np.floor(self.lo / self.CELL).astype(np.int64)
        chi = np.floor(self.hi / self.CELL).astype(np.int64)
        dims = chi - clo + 1
        count = dims.prod(1)
        big = count > 512
        self.big = np.nonzero(big)[0]
        small = np.nonzero(~big)[0]
        n = count[small]
        tri = np.repeat(small, n)
        local = np.arange(n.sum()) - np.repeat(np.cumsum(n) - n, n)
        d = dims[tri]
        cx = clo[tri, 0] + local % d[:, 0]
        cy = clo[tri, 1] + (local // d[:, 0]) % d[:, 1]
        cz = clo[tri, 2] + local // (d[:, 0] * d[:, 1])
        key = self._key(cx, cy, cz)
        order = np.argsort(key, kind='stable')
        key, tri = key[order], tri[order]
        uniq, start = np.unique(key, return_index=True)
        end = np.append(start[1:], len(key))
        self._tri_of_cell = tri
        self.cells = {int(k): (int(s), int(e)) for k, s, e in zip(uniq, start, end)}

    @staticmethod
    def _key(x, y, z):
        return ((np.asarray(x) + 4096) << 26) | ((np.asarray(y) + 4096) << 13) | (np.asarray(z) + 4096)

    def subset(self, lo, hi):
        """The triangles touching a box: a shot only needs the collision around its own move."""
        lo, hi = np.asarray(lo, float), np.asarray(hi, float)
        near = np.all(self.hi >= lo, axis=1) & np.all(self.lo <= hi, axis=1)
        return Mesh(self.tris[near])

    @staticmethod
    def load(manifest_dir, map_name, center, radius):
        manifest = json.loads((Path(manifest_dir) / (map_name + '.json')).read_text())
        data = mvm_cache.read_payload(Path(manifest_dir) / (manifest['physicsDigest'] + '.mvmocclusion'))
        assert data[:8] == b'MVMOCC01'
        vc, ic = struct.unpack_from('<ii', data, 8)
        o = 8 + 8 + 24
        pos = np.frombuffer(data, np.float32, vc * 3, o).reshape(-1, 3)
        o += vc * 12
        idx = np.frombuffer(data, np.uint32, ic, o).reshape(-1, 3)
        tris = pos[idx]
        lo, hi = tris.min(1), tris.max(1)
        c = np.asarray(center, np.float64)
        near = np.all(hi >= c - radius, axis=1) & np.all(lo <= c + radius, axis=1)
        return Mesh(tris[near])

    def candidates(self, lo, hi):
        clo = np.floor(np.asarray(lo) / self.CELL).astype(np.int64)
        chi = np.floor(np.asarray(hi) / self.CELL).astype(np.int64)
        found = [self.big] if len(self.big) else []
        for x in range(clo[0], chi[0] + 1):
            for y in range(clo[1], chi[1] + 1):
                for z in range(clo[2], chi[2] + 1):
                    se = self.cells.get(int(self._key(x, y, z)))
                    if se is not None:
                        found.append(self._tri_of_cell[se[0]:se[1]])
        if not found:
            return np.zeros(0, np.int64)
        return np.unique(np.concatenate(found))

    def raycast(self, origins, dirs, tmax, cand):
        """Nearest hit per ray against triangles `cand`: (t, triangle index), t = inf on a miss."""
        origins = np.atleast_2d(origins)
        dirs = np.atleast_2d(dirs)
        R = len(origins)
        if len(cand) == 0:
            return np.full(R, np.inf), np.full(R, -1)
        tri = self.tris[cand]
        v0 = tri[:, 0][None]
        e1 = (tri[:, 1] - tri[:, 0])[None]
        e2 = (tri[:, 2] - tri[:, 0])[None]
        d = dirs[:, None, :]
        p = np.cross(d, e2)
        det = np.einsum('rck,rck->rc', np.broadcast_to(e1, p.shape), p)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = origins[:, None, :] - v0
        u = np.einsum('rck,rck->rc', s, p) * inv
        q = np.cross(s, np.broadcast_to(e1, s.shape))
        v = np.einsum('rck,rck->rc', np.broadcast_to(d, q.shape), q) * inv
        t = np.einsum('rck,rck->rc', np.broadcast_to(e2, q.shape), q) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-4) & (t <= np.asarray(tmax).reshape(-1, 1))
        t = np.where(hit, t, np.inf)
        k = np.argmin(t, axis=1)
        best = t[np.arange(R), k]
        return best, np.where(np.isfinite(best), cand[k], -1)

    def segment_hits(self, A, B):
        """Batched segment_hit: first hit distance from each A toward its B (inf = clear)."""
        if self.gpu is not None:
            return self.gpu.segments(A, B)
        return np.array([self.segment_hit(a, b) for a, b in zip(np.atleast_2d(A), np.atleast_2d(B))])

    def clearance_many(self, P, reach=160.0):
        """(clearance capped at reach, inside-solid) for many points."""
        P = np.atleast_2d(np.asarray(P, float))
        if self.gpu is not None:
            d, _, _ = self.gpu.points_query(P, reach)
            return np.minimum(d, reach), self.inside_many(P)
        c = np.array([self.clearance(q, reach) for q in P])
        return c, np.array([c_ < 1.0 or (c_ < reach and self.inside_solid(q)) for c_, q in zip(c, P)])

    AXES = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float)

    def inside_many(self, P, reach=3000.0):
        """Inside closed collision, batched: of six axis rays, at least three hit and most of those
        hit a BACK face. (The GPU closest-point sign test misreads overlapping brushes, so this keeps
        the CPU path's vote, just batched.)"""
        P = np.atleast_2d(np.asarray(P, float))
        n = len(P)
        O = np.repeat(P, 6, 0)
        D = np.tile(self.AXES, (n, 1))
        if self.gpu is not None:
            tt, f = self.gpu.rays(O, D, reach)
        else:
            res = [self.raycast(o, d, reach, self.candidates(np.minimum(o, o + d * reach), np.maximum(o, o + d * reach))) for o, d in zip(O, D)]
            tt = np.array([r[0][0] for r in res]); f = np.array([r[1][0] for r in res])
        hit = f >= 0
        back = np.zeros(len(f), bool)
        back[hit] = np.einsum('ij,ij->i', self.normal[f[hit]], D[hit]) > 0
        hits = hit.reshape(n, 6).sum(1)
        backs = back.reshape(n, 6).sum(1)
        return (hits >= 3) & (backs * 2 > hits)

    def segment_hit(self, a, b):
        """First collision hit between points a and b: distance from a, or inf."""
        if self.gpu is not None:
            return float(self.gpu.segments(a, b)[0])
        a, b = np.asarray(a, float), np.asarray(b, float)
        d = b - a
        L = np.linalg.norm(d)
        if L < 1e-6:
            return np.inf
        cand = self.candidates(np.minimum(a, b), np.maximum(a, b))
        t, _ = self.raycast(a, d / L, L, cand)
        return t[0]

    _dirs = None

    @classmethod
    def sphere_dirs(cls, n=42):
        if cls._dirs is None:
            i = np.arange(n) + 0.5
            phi = np.arccos(1 - 2 * i / n)
            th = math.pi * (1 + 5 ** 0.5) * i
            cls._dirs = np.stack([np.cos(th) * np.sin(phi), np.sin(th) * np.sin(phi), np.cos(phi)], 1)
        return cls._dirs

    def clearance(self, p, reach=160.0):
        """Distance to the nearest collision surface (capped at `reach`)."""
        p = np.asarray(p, float)
        if self.gpu is not None:
            return float(min(self.gpu.points_query(p, reach)[0][0], reach))
        cand = self.candidates(p - reach, p + reach)
        dirs = self.sphere_dirs()
        t, _ = self.raycast(np.repeat(p[None], len(dirs), 0), dirs, reach, cand)
        return float(min(t.min(), reach))

    def inside_solid(self, p, reach=3000.0):
        """True when the point sits inside closed collision: most axis rays first hit a back face."""
        p = np.asarray(p, float)
        if self.gpu is not None:
            return bool(self.inside_many(p[None], reach)[0])
        back = hits = 0
        for d in np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float):
            end = p + d * reach
            cand = self.candidates(np.minimum(p, end), np.maximum(p, end))
            t, k = self.raycast(p, d, reach, cand)
            if k[0] >= 0:
                hits += 1
                if np.dot(self.normal[k[0]], d) > 0:
                    back += 1
        return hits >= 3 and back * 2 > hits


# ------------------------------------------------------------------ demo track

class Track:
    def __init__(self, path):
        doc = json.loads(Path(path).read_text())
        self.events = doc.get('events', [])
        self.players = {}
        for p in doc['players'].values():
            s = p['samples']
            self.players[p['name']] = {
                'tick': np.array([x['tick'] for x in s], float),
                'pos': np.array([x['pos'] for x in s], float),
                'eye': np.array([x['eye'] for x in s], float),
                'ang': np.array([x['angles'] for x in s], float),
                'alive': np.array([x['alive'] for x in s], bool),
                'team': p['team'],
            }

    def at(self, name, tick, field='pos'):
        p = self.players[name]
        t = p['tick']
        return np.array([np.interp(tick, t, p[field][:, k]) for k in range(3)])

    SMOKE_RADII = np.array([150.0, 150.0, 115.0])   # a bloomed CS2 smoke, around its centre
    SMOKE_LIFE = 1180                               # ticks, when the demo has no expiry event

    def smokes(self):
        """[(start tick, end tick, centre, radii)] for every smoke cloud in the track's events."""
        if not hasattr(self, '_smokes'):
            starts, ends = {}, {}
            for e in self.events:
                if e['kind'] == 'smoke_start':
                    starts[e.get('player')] = e
                elif e['kind'] == 'smoke_end':
                    ends[e.get('player')] = e['tick']
            self._smokes = []
            for gid, e in starts.items():
                c = np.array(e['pos'], float) + [0, 0, 60]
                self._smokes.append((e['tick'], ends.get(gid, e['tick'] + self.SMOKE_LIFE), c, self.SMOKE_RADII))
        return self._smokes

    def smoke_check(self, cams, targets, ticks):
        """Per frame: (camera inside a smoke, smoke between camera and target). Ellipsoids in
        unit-sphere space; the sightline test uses 85% of the cloud so a camera can skim its edge."""
        inside = np.zeros(len(ticks), bool)
        block = np.zeros(len(ticks), bool)
        for t0, t1, c, r in self.smokes():
            on = (ticks >= t0) & (ticks <= t1)
            if not on.any():
                continue
            a = (cams[on] - c) / r
            inside[on] |= np.linalg.norm(a, axis=1) < 1.05
            b = (targets[on] - c) / (r * 0.85)
            a2 = (cams[on] - c) / (r * 0.85)
            d = b - a2
            L2 = np.maximum((d * d).sum(1), 1e-9)
            u = np.clip(-(a2 * d).sum(1) / L2, 0, 1)
            closest = a2 + d * u[:, None]
            block[on] |= np.linalg.norm(closest, axis=1) < 1.0
        return inside, block

    def smooth_at(self, name, tick, sigma_xy=10.0, sigma_z=28.0):
        """Low-passed position: what a camera operator follows. A bhop bounces the head 60+ units
        every hop; aiming at the raw head makes the camera nod with every jump."""
        from scipy.ndimage import gaussian_filter1d
        p = self.players[name]
        key = ('_smooth', sigma_xy, sigma_z)
        if key not in p:
            pos = p['pos'].copy()
            alive = np.nonzero(p['alive'])[0]
            last = alive[-1] if len(alive) else len(pos) - 1
            pos[last + 1:] = pos[last]          # hold the death spot, do not smear the ragdoll in
            sm = np.stack([gaussian_filter1d(pos[:, 0], sigma_xy, mode='nearest'),
                           gaussian_filter1d(pos[:, 1], sigma_xy, mode='nearest'),
                           gaussian_filter1d(pos[:, 2], sigma_z, mode='nearest')], 1)
            p[key] = sm
        sm = p[key]
        return np.array([np.interp(tick, p['tick'], sm[:, k]) for k in range(3)])

    def alive(self, name, tick):
        p = self.players[name]
        i = int(np.clip(np.searchsorted(p['tick'], tick), 0, len(p['tick']) - 1))
        return bool(p['alive'][i])


# ------------------------------------------------------------------ camera math (Source angles)

def rot_from_angles(pitch, yaw, roll):
    """Source view angles -> rotation whose columns are forward, left, up."""
    return Rotation.from_euler('ZYX', [yaw, pitch, roll], degrees=True)


def angles_from_rot(r):
    yaw, pitch, roll = r.as_euler('ZYX', degrees=True)
    return pitch, yaw, roll


def look_angles(src, dst):
    d = np.asarray(dst, float) - np.asarray(src, float)
    yaw = math.degrees(math.atan2(d[1], d[0]))
    pitch = -math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))
    return pitch, yaw


def tan_half(fov):
    """Engine FOV is horizontal at 4:3; the render frame is wider. Returns (tan h/2, tan v/2)."""
    t43 = math.tan(math.radians(fov) / 2)
    return t43 * ASPECT / (4 / 3), t43 * 0.75


def project(cam_pos, rot, fov, pts):
    """World points -> (screen x, screen y in -1..1, depth). x right, y up."""
    m = rot.as_matrix()
    rel = np.asarray(pts, float) - cam_pos
    depth = rel @ m[:, 0]
    right = -(rel @ m[:, 1])
    up = rel @ m[:, 2]
    th, tv = tan_half(fov)
    with np.errstate(divide='ignore', invalid='ignore'):
        return right / depth / th, up / depth / tv, depth


# ------------------------------------------------------------------ the path (CamPathEval replica)

EASE = {0: lambda p: p, 1: lambda p: p * p, 2: lambda p: 1 - (1 - p) ** 2, 3: lambda p: p * p * (3 - 2 * p)}


def pchip_slopes(t, s):
    n = len(t)
    m = np.zeros(n)
    d = np.diff(t)
    delta = np.where(d > 1e-12, np.diff(s) / np.where(d > 1e-12, d, 1), 0)
    if n == 2:
        return np.array([delta[0], delta[0]])
    for i in range(1, n - 1):
        a, b = delta[i - 1], delta[i]
        if a * b <= 0:
            continue
        w1, w2 = 2 * d[i] + d[i - 1], d[i] + 2 * d[i - 1]
        m[i] = (w1 + w2) / (w1 / a + w2 / b)

    def end(d0, d1, del0, del1):
        v = ((2 * d0 + d1) * del0 - d0 * del1) / (d0 + d1)
        if v * del0 <= 0:
            v = 0
        elif del0 * del1 <= 0 and abs(v) > abs(3 * del0):
            v = 3 * del0
        return v
    m[0] = end(d[0], d[1], delta[0], delta[1])
    m[-1] = end(d[-1], d[max(n - 3, 0)], delta[-1], delta[max(n - 3, 0)])
    return m


def pchip_at(t, s, m, k, p):
    h = t[k + 1] - t[k]
    p2, p3 = p * p, p * p * p
    v = (2 * p3 - 3 * p2 + 1) * s[k] + (p3 - 2 * p2 + p) * h * m[k] + (-2 * p3 + 3 * p2) * s[k + 1] + (p3 - p2) * h * m[k + 1]
    return min(max(v, min(s[k], s[k + 1])), max(s[k], s[k + 1]))


class Path3:
    """Markers -> pose at any (fractional) tick, matching CamPathEval Live/Manual/Smooth."""

    def __init__(self, markers):
        self.mk = sorted(markers, key=lambda m: m['tick'])
        n = len(self.mk)
        assert n >= 2, 'a path needs at least two keys'
        P = np.array([[m['x'], m['y'], m['z']] for m in self.mk])
        chord = np.maximum(np.linalg.norm(np.diff(P, axis=0), axis=1), 1e-3)
        self.s = np.concatenate([[0], np.cumsum(chord)])
        times = np.array([m['tick'] / TICK_RATE for m in self.mk])
        self.t = np.concatenate([[0], np.cumsum(np.maximum(np.abs(np.diff(times)), 1e-3))])
        rots = Rotation.from_euler('ZYX', [[m['yaw'], m['pitch'], m['roll']] for m in self.mk], degrees=True)
        fov = np.array([m['fov'] for m in self.mk])
        self.cubic = n >= 3
        if self.cubic:
            self.pos = CubicSpline(self.s, P, bc_type='natural')
            self.fov = CubicSpline(self.s, fov, bc_type='natural')
            self.rot = RotationSpline(self.s, rots)
            self.slopes = pchip_slopes(self.t, self.s)
        else:
            self.P, self.F = P, fov
            self.rot = Slerp(self.s, rots)

    def at(self, tick):
        mk, n = self.mk, len(self.mk)
        seg = 0
        while seg < n - 2 and tick >= mk[seg + 1]['tick']:
            seg += 1
        ta, tb = mk[seg]['tick'], mk[seg + 1]['tick']
        p = min(max((tick - ta) / (tb - ta) if tb > ta else 1.0, 0.0), 1.0)
        pe = EASE.get(int(mk[seg].get('ease', 0)), EASE[0])(p)
        if self.cubic:
            s = pchip_at(self.t, self.s, self.slopes, seg, pe)
            return self.pos(s), self.rot(s), float(self.fov(s))
        pe = pe * pe * (3 - 2 * pe)
        s = self.s[0] + pe * (self.s[1] - self.s[0])
        return self.P[0] + pe * (self.P[1] - self.P[0]), self.rot(s), float(self.F[0] + pe * (self.F[1] - self.F[0]))


# ------------------------------------------------------------------ shot spec -> markers

# Composition: where a subject's eye line should sit in the frame (x right, y up, -1..1).
# Thirds lines are at +-1/3, golden-ratio (phi) lines at +-0.236 (0.382 / 0.618 of the frame).
THIRD = 1 / 3
PHI = 2 * 0.381966 - 1  # 0.382 of the width, measured from the centre = -0.236
FRAMES = {
    'center': (0.0, 0.12),            # symmetrical: eyes just above the middle
    'center-high': (0.0, THIRD),
    'thirds-left': (-THIRD, THIRD), 'thirds-right': (THIRD, THIRD),
    'golden-left': (PHI, -PHI), 'golden-right': (-PHI, -PHI),
    'low-left': (-THIRD, 0.0), 'low-right': (THIRD, 0.0),
}

# Director presets: shot defaults plus the rules the solver and the report hold the shot to.
# Every value can be overridden per shot or per key.
STYLES = {
    'wes-anderson': {'fov': 40, 'roll': 0, 'frame': 'center', 'level': True,
                     'notes': 'symmetrical planimetric tableau: level horizon, subject dead centre, camera square to '
                              'the set, constant-speed straight dollies or whip pans between tableaux'},
    'kubrick': {'fov': 34, 'roll': 0, 'frame': 'center', 'level': True,
                'notes': 'one-point perspective down a corridor, slow steady push, vanishing point centred'},
    'edgar-wright': {'fov': 60, 'roll': 0, 'frame': 'center',
                     'notes': 'kinetic: crash zooms (fov 70 -> 20 over 3-4 ticks), whip pans (90-180 deg over '
                              '8-12 ticks), hard snaps on the beat'},
    'tarantino': {'fov': 38, 'roll': 0, 'frame': 'golden-right', 'lead': 'auto',
                  'notes': 'trunk / low-angle hero shot from the floor looking up, golden-ratio placement, slow push'},
    'snyder': {'fov': 50, 'roll': 6, 'frame': 'golden-right', 'lead': 'auto', 'fps': 256,
               'notes': 'speed-ramped slow-motion orbit around the peak moment, slight dutch, golden placement'},
    'dutch-longlens': {'fov': 56, 'roll': 10, 'frame': 'thirds-right', 'lead': 'auto',
                       'notes': "the reference path's style: far away, long lens, 5-15 deg dutch roll, fast "
                                'dolly/whip between the action beats'},
}


def style_of(shot):
    return STYLES.get(shot.get('style', ''), {})


def opt(key, shot, name, default=None):
    """A key's own value, else the shot's, else the shot style's, else the default."""
    if key is not None and name in key:
        return key[name]
    if name in shot:
        return shot[name]
    return style_of(shot).get(name, default)


def frame_target(spec):
    if spec is None:
        return None
    if isinstance(spec, str):
        return FRAMES[spec]
    return tuple(spec)


def subject_points(track, names, tick, on='head', smooth=None):
    """The framing point for one or several players: their eye line (head), chest or feet,
    averaged. smooth = (sigma xy, sigma z) in ticks follows the low-passed track instead."""
    pts = []
    for n in names:
        feet = track.smooth_at(n, tick, *smooth) if smooth else track.at(n, tick)
        pts.append(feet + [0, 0, {'head': 64, 'chest': 40, 'feet': 6}.get(on, 64)])
    return np.mean(pts, axis=0)


def solve_angles(pos, roll, fov, target, sx, sy, level=False):
    """Pitch/yaw that put world point `target` at screen (sx, sy) for this position/roll/FOV."""
    from scipy.optimize import least_squares
    pitch0, yaw0 = look_angles(pos, target)
    if level:
        def f(v):
            x, _, _ = project(pos, rot_from_angles(0.0, v[0], roll), fov, [target])
            return [x[0] - sx]
        r = least_squares(f, [yaw0])
        return 0.0, float(r.x[0])

    def g(v):
        x, y, _ = project(pos, rot_from_angles(v[0], v[1], roll), fov, [target])
        return [x[0] - sx, y[0] - sy]
    r = least_squares(g, [pitch0, yaw0])
    return float(r.x[0]), float(r.x[1])


def lead_side(track, names, tick, pos, rot, fov, span=12):
    """'left' when the subject moves right across this view (leave room ahead), else 'right'."""
    a = subject_points(track, names, tick - span / 2)
    b = subject_points(track, names, tick + span / 2)
    xa, _, _ = project(pos, rot, fov, [a])
    xb, _, _ = project(pos, rot, fov, [b])
    return 'left' if xb[0] > xa[0] else 'right'


def resolve_target(key, shot, track, tick, pos, fov, roll):
    """(world point, screen target) a key frames, honouring style frame + lead room."""
    names = opt(key, shot, 'subjects') or ([opt(key, shot, 'subject')] if opt(key, shot, 'subject') else [])
    aim = opt(key, shot, 'aim', 'subject')
    on = opt(key, shot, 'frameOn', 'head')
    smooth = opt(key, shot, 'aimSmooth', [10, 28])
    smooth = tuple(smooth) if smooth else None
    if aim == 'subject':
        target = subject_points(track, names, tick, on, smooth)
    elif isinstance(aim, str) and aim.startswith('player:'):
        names = aim[7:].split('+')
        target = subject_points(track, names, opt(key, shot, 'aimTick', tick), on, smooth)
    else:
        target = np.array(aim, float)
    if aim == 'subject' and shot.get('lookAt'):
        target = look_at_events(shot['lookAt'], tick, target)
    frame = frame_target(opt(key, shot, 'frame', 'center'))
    if opt(key, shot, 'lead') == 'auto' and frame and abs(frame[0]) > 1e-3 and names:
        # Lead room is a per-shot decision (which side the subject travels toward over the whole
        # shot), never per frame -- re-deciding it every frame snaps him across the frame.
        if '_lead' not in shot:
            a, b = shot.get('range', [tick - 16, tick + 16])
            p, y = look_angles(pos, target)
            shot['_lead'] = lead_side(track, names, (a + b) / 2, pos, rot_from_angles(p, y, roll), fov, span=b - a)
        frame = (-abs(frame[0]) if shot['_lead'] == 'left' else abs(frame[0]), frame[1])
    return target, frame, names


def look_at_events(events, tick, target):
    """Where an operator's eye goes: toward a blast or a body drop for a moment, then back to the
    subject. events = [{"tick", "pos", "pre" (ticks of anticipation), "hold", "weight"}]; the
    loose tracker turns the shifted target into an unhurried pan."""
    w_best, p_best = 0.0, None
    for e in events:
        d = tick - e['tick']
        pre, hold, fall = e.get('pre', 10), e.get('hold', 50), e.get('fall', 40)
        if d < -pre or d > hold + fall:
            continue
        u = 1.0 if 0 <= d <= hold else ((d + pre) / pre if d < 0 else 1 - (d - hold) / fall)
        u = u * u * (3 - 2 * u)
        w = u * e.get('weight', 0.6)
        if w > w_best:
            w_best, p_best = w, np.array(e['pos'], float)
    return target if p_best is None else target * (1 - w_best) + p_best * w_best


def resolve_key(key, shot, track):
    tick = key['tick']
    subject = opt(key, shot, 'subject')
    feet = track.at(subject, tick) if subject else None
    if 'pos' in key:
        pos = np.array(key['pos'], float)
    elif 'offset' in key:
        pos = feet + np.array(key['offset'], float)
    elif 'orbit' in key:
        o = key['orbit']
        a = math.radians(o['azim'])
        pos = feet + np.array([math.cos(a) * o['dist'], math.sin(a) * o['dist'], o.get('z', 60)])
    else:
        raise ValueError('key at tick %d has no position' % tick)
    fov = opt(key, shot, 'fov', 50)
    roll = opt(key, shot, 'roll', 0)
    if 'angles' in key:
        pitch, yaw = key['angles'][:2]
    else:
        target, frame, _ = resolve_target(key, shot, track, tick, pos, fov, roll)
        pitch, yaw = solve_angles(pos, roll, fov, target, frame[0], frame[1], bool(opt(key, shot, 'level', False)))
    return {'x': round(float(pos[0]), 3), 'y': round(float(pos[1]), 3), 'z': round(float(pos[2]), 3),
            'pitch': round(float(pitch), 4), 'yaw': round(float(yaw), 4), 'roll': float(roll), 'fov': float(fov),
            'tick': int(tick), 'time': round(tick / TICK_RATE, 4), 'speedmul': 1, 'ease': int(opt(key, shot, 'ease', 0))}


def shot_markers(shot, track, mesh=None, log=None):
    if 'generate' in shot and not shot.get('_generated'):
        shot['keys'] = generate_keys(shot, track, mesh)
        shot['_generated'] = True
    markers = [resolve_key(k, shot, track) for k in shot['keys']]
    # Key spacing: the engine paces between keys with a PCHIP curve that dips the speed at each key while
    # the speed is changing (scalloped motion); the ripple shrinks with the square of the spacing.
    if shot.get('_generated'):
        shot.setdefault('operator', False)
        shot.setdefault('refine', False)
    every = opt(None, shot, 'aimEvery', 4)
    if every and len(markers) >= 2 and not any('angles' in k or 'aim' in k for k in shot['keys']):
        markers = aim_keys(shot, markers, track, every)
        operator_move = bool(opt(None, shot, 'operator', True))
    else:
        operator_move = False
    # Operator moves are one eased pan by design; inserting framing keys into them breaks the glide
    # (with a subject sweeping past the lens it produced wild re-aims). The report flags drift instead.
    if mesh is not None and shot.get('refine', not operator_move) is not False:
        markers = refine(shot, markers, track, mesh, log)
    # Strategic keys: a smooth move does not need a key every few ticks -- keep only the ones the
    # path actually needs to stay on its line (opt out with "keys": "all").
    if shot.get('keyMode', 'strategic') == 'strategic' and len(markers) > 3:
        markers = simplify_markers(markers, shot.get('keyPosTol', 6.0), shot.get('keyAngTol', 1.5))
    # Shake events: a key AT the blast carrying a hit shake, and a key `hold` ticks later handing back
    # to the shot's own shake (CamPathShake blends between key styles).
    for ev in shot.get('shakeEvents', []):
        markers = insert_marker(markers, ev['tick'], shake_of(ev))
        markers = insert_marker(markers, ev['tick'] + ev.get('hold', 40), None)
    # Path shake (CamPathShake): per-key "shake" or one shot-wide "shake" on every key, which the
    # game treats as one continuous run (ApplyToAll).
    for m in markers:
        if m.get('shake'):
            continue
        src = next((k for k in shot['keys'] if k.get('tick') == m['tick'] and 'shake' in k), None)
        sh = shake_of(src['shake'] if src else shot.get('shake'))
        if sh:
            m['shake'] = sh
    return markers


def insert_marker(markers, tick, shake):
    """A key on the existing curve at `tick` (or the existing key within 3 ticks), with `shake`."""
    tick = int(round(tick))
    if not markers or tick <= markers[0]['tick'] or tick >= markers[-1]['tick']:
        return markers
    near = min(markers, key=lambda m: abs(m['tick'] - tick))
    if abs(near['tick'] - tick) <= 3:
        if shake:
            near['shake'] = shake
        return markers
    pos, rot, fov = Path3(markers).at(tick)
    pitch, yaw, roll = angles_from_rot(rot)
    yaw += 360.0 * round((near['yaw'] - yaw) / 360.0)     # stay on the neighbours' unwrapped yaw
    m = {'x': round(float(pos[0]), 3), 'y': round(float(pos[1]), 3), 'z': round(float(pos[2]), 3),
         'pitch': round(float(pitch), 4), 'yaw': round(float(yaw), 4), 'roll': round(float(roll), 3), 'fov': round(float(fov), 3),
         'tick': tick, 'time': round(tick / TICK_RATE, 4), 'speedmul': 1, 'ease': 0}
    if shake:
        m['shake'] = shake
    return sorted(markers + [m], key=lambda k: k['tick'])


def operator_progress(u, ease):
    """Where a camera operator is along the move at normalised time u: ease in over the first `ease`
    of the shot, glide at constant speed, ease out over the last `ease` (velocity is continuous)."""
    e = min(max(ease, 1e-3), 0.5)
    vmax = 1.0 / (1.0 - e)                      # area under the trapezoid speed profile = 1
    if u <= e:
        return vmax * u * u / (2 * e)
    if u >= 1 - e:
        w = 1 - u
        return 1 - vmax * w * w / (2 * e)
    return vmax * (u - e / 2)


def fit_monotone(t, v, max_degree=3):
    """Smooth operator curve through samples: the highest polynomial degree (<= max_degree) whose
    derivative never changes sign when the overall move goes one way. A pan/tilt that reverses
    mid-move (a "rebound") is what made a shot read as not hand-operated."""
    span = v[-1] - v[0]
    for deg in range(max_degree, 0, -1):
        c = np.polyfit(t, v, deg)
        d = np.polyval(np.polyder(c), t)
        if abs(span) < 0.5 or np.all(d * np.sign(span) >= -1e-6):
            return c
    return np.polyfit(t, v, 1)


def generate_keys(shot, track, mesh):
    """Long camera moves that travel WITH the subject, built from his smoothed route:
      follow   -- steadicam at a fixed offset in his heading frame: [back, side, up] (back < 0 leads)
      orbit    -- circles him from azimuth a0 to a1 (world degrees) at dist/up, eased over the shot
      overhead -- god's-eye view `up` units above him, a touch behind so the frame keeps his heading
    Keys every `every` ticks; each is pushed off nearby walls, then the whole move is smoothed so it
    stays one continuous operator curve. Aim is solved per key onto the smoothed subject."""
    from scipy.ndimage import gaussian_filter1d
    g = shot['generate']
    kind = g.get('type', 'follow')
    subject = shot['subject']
    a, b = g.get('ticks', shot['range'])
    every = g.get('every', 8)
    ticks = np.arange(a, b + 1e-9, every)
    if ticks[-1] < b:
        ticks = np.append(ticks, b)
    sig = g.get('smooth', 24)
    S = np.array([track.smooth_at(subject, tk, sig, sig * 1.5) for tk in ticks])
    # Heading from the smoothed route; held while he stands still, then itself smoothed.
    head = []
    last = 0.0
    for tk in ticks:
        v = track.smooth_at(subject, tk + 12, sig, sig) - track.smooth_at(subject, tk - 12, sig, sig)
        if np.hypot(v[0], v[1]) > 8:
            last = math.atan2(v[1], v[0])
        head.append(last)
    head = np.unwrap(np.array(head))
    head = gaussian_filter1d(head, max(1.0, g.get('headingSmooth', 32) / every), mode='nearest')
    if 'heading' in g:
        head = np.full_like(head, math.radians(g['heading']))
    pos = []
    for i, tk in enumerate(ticks):
        u = (tk - a) / max(b - a, 1)
        if kind == 'follow':
            back, side, up = g.get('offset', [-220, 0, 50])
            c, s = math.cos(head[i]), math.sin(head[i])
            p = S[i] + np.array([c * back - s * side, s * back + c * side, up])
        elif kind == 'route':
            # Rides his own smoothed route `dist` units ahead (positive) or behind (negative), measured
            # ALONG the route: the route is walkable, so the camera stays out of walls and keeps a
            # natural distance through corners -- and holds that distance when he stops (a time
            # offset collapsed onto him while he stood still).
            if i == 0:
                # Lightly smoothed: a heavily smoothed route cuts corners into the walls he walked past.
                rs = g.get('routeSmooth', 8)
                span = np.arange(a - 900, b + 900, 2.0)
                route = np.array([track.smooth_at(subject, x, rs, rs * 1.5) for x in span])
                arc = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(route[:, :2], axis=0), axis=1))])
            s_now = float(np.interp(tk, span, arc))
            s_cam = s_now + g.get('dist', -220)
            ahead = np.array([np.interp(s_cam, arc, route[:, k]) for k in range(3)])
            c, s = math.cos(head[i]), math.sin(head[i])
            side = g.get('side', 0)
            p = ahead + np.array([-s * side, c * side, g.get('up', 50)])
        elif kind == 'walk':
            # A cameraman on foot: starts `start` units back along the subject's route and walks after
            # him at `speed` u/s (eases in and out over `accel` s), never closer than `gap`. He falls
            # behind a sprint and closes in when the push stalls -- the distance breathes, unlike a
            # rig locked to the subject. Eye height `up`, `side` units off the walked line.
            # "routeOf": walk behind another player (a teammate in the stack) while filming the subject.
            if i == 0:
                rs = g.get('routeSmooth', 10)
                span = np.arange(a - 1200, b + 600, 2.0)
                route = np.array([track.smooth_at(g.get('routeOf', subject), x, rs, rs * 1.5) for x in span])
                arc = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(route[:, :2], axis=0), axis=1))])
                fine = np.arange(a, b + 1e-9, 1.0)
                s_sub = np.interp(fine, span, arc)
                vmax, gap, acc = g.get('speed', 150.0), g.get('gap', 170.0), max(0.1, g.get('accel', 0.6))
                s, v = s_sub[0] + g.get('start', -380.0), g.get('v0', 0.6) * vmax
                walk_s = []
                for j in range(len(fine)):
                    want = float(np.clip((s_sub[j] - gap - s) * 2.5, 0.0, vmax))
                    v += (want - v) * min(1.0, (1.0 / TICK_RATE) / acc)
                    s += v / TICK_RATE
                    walk_s.append(s)
                walk_s = np.array(walk_s)
            sc = float(np.interp(tk, fine, walk_s))
            base = np.array([np.interp(sc, arc, route[:, k]) for k in range(3)])
            nxt = np.array([np.interp(sc + 40, arc, route[:, k]) for k in range(3)])
            tan = nxt - np.array([np.interp(sc - 40, arc, route[:, k]) for k in range(3)])
            hd = math.atan2(tan[1], tan[0]) if np.hypot(tan[0], tan[1]) > 4 else head[i]
            side = g.get('side', 0)
            p = base + np.array([-math.sin(hd) * side, math.cos(hd) * side, g.get('up', 62)])
            if mesh is not None and side:
                eye = base + np.array([0, 0, g.get('up', 62)])
                h = mesh.segment_hits(eye[None], p[None])[0]
                if np.isfinite(h):
                    p = eye + (p - eye) * max(0.0, (h - 24) / max(abs(side), 1))
        elif kind == 'orbit':
            e = u * u * (3 - 2 * u)
            ang = math.radians(g['a0'] + (g['a1'] - g['a0']) * e)
            p = S[i] + np.array([math.cos(ang) * g['dist'], math.sin(ang) * g['dist'], g.get('up', 40)])
        elif kind == 'overhead':
            c, s = math.cos(head[i]), math.sin(head[i])
            p = S[i] + np.array([-c * g.get('back', 40), -s * g.get('back', 40), g.get('up', 420)])
        else:
            raise ValueError('unknown generate type ' + kind)
        pos.append(p)
    pos = np.array(pos)
    k = g.get('pathSmooth', 1.5)
    if k and len(pos) > 4:
        sm = np.stack([gaussian_filter1d(pos[:, j], k, mode='nearest') for j in range(3)], 1)
        if kind != 'walk':          # a walker's ends are already smooth; pinning them kinks the pace
            sm[0], sm[-1] = pos[0], pos[-1]
        pos = sm
    # Boom arm: the camera hangs on a line from his (smoothed) head out to the wanted spot. Where a
    # wall cuts that line the arm shortens to stop short of it -- so the lens can never sit inside
    # geometry or lose sight of him. The arm length shortens AHEAD of a wall (window minimum) and
    # eases back out (smoothing), like a third-person camera's spring arm.
    # A route camera sits on ground he actually walked (plus height): it cannot be inside a wall, so
    # it skips the boom -- pushing it around only added bumps.
    if mesh is not None and kind not in ('route', 'walk'):
        from scipy.ndimage import minimum_filter1d
        pivot = S + np.array([0, 0, 56.0])
        arm = pos - pivot
        full = np.linalg.norm(arm, axis=1)
        hits = mesh.segment_hits(pivot, pos)
        frac = np.where(np.isfinite(hits), np.maximum(g.get('minArm', 0.12), (hits - g.get('wallGap', 28)) / np.maximum(full, 1)), 1.0)
        frac = minimum_filter1d(frac, size=max(3, int(g.get('armLookahead', 40) / every)), mode='nearest')
        frac = gaussian_filter1d(frac, max(1.0, 16 / every), mode='nearest')
        pos = pivot + arm * frac[:, None]
        pos = np.array([push_off_walls(mesh, q, CLEARANCE_WARN + 6) for q in pos])
    keys = []
    for i, tk in enumerate(ticks):
        key = {'tick': int(round(tk)), 'pos': [round(float(x), 2) for x in pos[i]]}
        if 'angles' in g:
            key['angles'] = list(g['angles'])     # locked orientation (e.g. a true top-down)
        for f in ('fov', 'roll'):
            if f in g:
                v = g[f]
                key[f] = float(np.interp(i, [0, len(ticks) - 1], v)) if isinstance(v, list) else v
        keys.append(key)
    return keys


# ------------------------------------------------------------------ path shake (preview of CamPathShake)

SHAKEDUMP = Path(__file__).resolve().parents[2] / 'build' / 'cinematics' / 'shakedump.exe'
_SHAKE_STYLES = None


def shake_styles():
    """{lower-case label or name: (style index, NAME)} from the game's own style table."""
    global _SHAKE_STYLES
    if _SHAKE_STYLES is None:
        import subprocess
        _SHAKE_STYLES = {}
        if SHAKEDUMP.exists():
            for line in subprocess.run([str(SHAKEDUMP), 'styles'], capture_output=True, text=True).stdout.splitlines():
                idx, name, label, menu = line.split('\t')
                _SHAKE_STYLES[name.lower()] = (int(idx), name)
                _SHAKE_STYLES.setdefault(label.lower(), (int(idx), name))
    return _SHAKE_STYLES


def shake_of(spec):
    """{"style": "Hand"|"Drunk"|"Wobbly"|NAME, "intensity": 1-10, "speed": 10-100} -> (NAME, i, s)."""
    if not spec:
        return None
    st = shake_styles().get(str(spec.get('style', 'Hand')).lower())
    if not st:
        raise ValueError('unknown shake style %r (see: shotplan.py shakes)' % spec.get('style'))
    return st[1], int(max(1, min(10, spec.get('intensity', 5)))), int(max(10, min(100, spec.get('speed', 50))))


def shake_offsets(markers, ticks):
    """Per-tick shake offsets (right fwd up wx wy wz pitch yaw roll fov), exactly as the game's
    CamPathShake evaluates them (shakedump runs the hook's own RageShake code)."""
    import subprocess
    keys = [m for m in markers if m.get('shake')]
    if not keys or not SHAKEDUMP.exists():
        return None
    spec = ','.join('%d:%s:%d:%d' % (m['tick'], m['shake'][0], m['shake'][1], m['shake'][2]) for m in markers if m.get('shake'))
    step = (ticks[1] - ticks[0]) if len(ticks) > 1 else 1.0
    out = subprocess.run([str(SHAKEDUMP), 'eval', spec, '%.4f' % ticks[0], '%.4f' % ticks[-1], '%.6f' % step],
                         capture_output=True, text=True).stdout.split()
    vals = np.array(out, float).reshape(-1, 11)
    return {round(r[0], 3): r[1:] for r in vals}


def apply_shake(pos, rot, fov, off):
    """The game's CamPathShake_ApplyToView, on one pose: camera-basis offsets use the authored
    yaw/pitch (roll 0), angles add in Source euler, fov adds."""
    pitch, yaw, roll = angles_from_rot(rot)
    right, fwd, up, wx, wy, wz, dp, dy, dr, dfov = off
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    f = np.array([cp * cy, cp * sy, -sp]); r = np.array([sy, -cy, 0.0]); u = np.array([sp * cy, sp * sy, cp])
    pos = np.asarray(pos, float) + fwd * f + right * r + up * u + np.array([wx, wy, wz])
    return pos, rot_from_angles(pitch + dp, yaw + dy, roll + dr), fov + dfov


def simplify_markers(markers, pos_tol=6.0, ang_tol=1.5, fov_tol=1.0, acc_tol=500.0):
    """Strategic keys: the fewest of `markers` whose re-evaluated path stays within tolerance of the
    full set everywhere (greedy: start from the ends, add the key at the worst deviation)."""
    if len(markers) <= 3:
        return markers
    full = Path3(markers)
    a, b = markers[0]['tick'], markers[-1]['tick']
    ticks = np.arange(a, b + 1e-9, 2.0)
    ref = [full.at(tk) for tk in ticks]
    dt = 2.0 / TICK_RATE

    def accel(poses):
        P = np.array([q[0] for q in poses])
        return np.linalg.norm(np.diff(P, 2, axis=0), axis=1) / dt / dt
    acc_ref = accel(ref)
    keep = {0, len(markers) - 1, len(markers) // 2}
    while True:
        sub = [markers[i] for i in sorted(keep)]
        path = Path3(sub)
        cur = [path.at(tk) for tk in ticks]
        # Motion counts too: fewer keys must not add bumps the full key set did not have.
        acc_err = np.abs(accel(cur) - acc_ref) / acc_tol
        worst, worst_tick = 0.0, None
        for j, (tk, (p0, r0, f0), (p1, r1, f1)) in enumerate(zip(ticks, ref, cur)):
            e = max(np.linalg.norm(p1 - p0) / pos_tol, np.degrees((r1 * r0.inv()).magnitude()) / ang_tol, abs(f1 - f0) / fov_tol,
                    acc_err[j - 1] if 0 < j < len(ticks) - 1 else 0.0)
            if e > worst:
                worst, worst_tick = e, tk
        if worst <= 1.0 or len(keep) >= len(markers):
            return sub
        cand = min((i for i in range(len(markers)) if i not in keep), key=lambda i: abs(markers[i]['tick'] - worst_tick))
        keep.add(cand)


def aim_keys(shot, markers, track, every):
    """Re-key a tracking shot the way an operator moves a camera. Evenly spaced keys keep the
    rotation spline free of kinks; on top of that ("operator", default on):
      * the dolly runs along the authored curve on ONE speed profile for the whole shot (ease in,
        glide, ease out) instead of re-sampling a per-segment ease, which made the move slow down
        mid-shot and speed up again;
      * pan and tilt follow one smooth, non-reversing fit of the ideal aim over the whole shot, not
        the subject's every hop -- the subject is allowed to drift a little in the frame, exactly as
        with a real operator, and the report says how far."""
    path = Path3(markers)
    a, b = markers[0]['tick'], markers[-1]['tick']
    n = max(1, int(round((b - a) / every)))
    ticks = sorted(set(int(round(x)) for x in np.linspace(a, b, n + 1)))
    operator = opt(None, shot, 'operator', True)
    level = bool(opt(None, shot, 'level', False))
    mk_ticks = [m['tick'] for m in markers]

    def roll_at(tk):
        return float(np.interp(tk, mk_ticks, [m['roll'] for m in markers]))

    if operator:
        # Geometry of the authored move, re-timed on the operator speed profile.
        dense = np.linspace(a, b, 400)
        geo = np.array([path.at(tk)[0] for tk in dense])
        arc = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(geo, axis=0), axis=1))])
        ease = float(opt(None, shot, 'dollyEase', 0.3))
        # A straight move goes out as TWO keys: the engine paces a two-key path with one smoothstep
        # over the whole move (smooth start, glide, smooth stop) and slerps the rotation on the same
        # ease -- an operator's move with no per-key speed ripple. Interior keys always ripple the
        # speed wherever it changes (PCHIP pacing undershoots at every key).
        chord = geo[-1] - geo[0]
        L = np.linalg.norm(chord)
        off = 0.0 if L < 1e-6 else float(np.max(np.linalg.norm(np.cross(geo - geo[0], chord / L), axis=1)))
        two_key = off < 3.0 and opt(None, shot, 'twoKey', True)
        if two_key:
            ticks = [a, b]

        def progress(uu):
            return uu * uu * (3 - 2 * uu) if two_key else operator_progress(uu, ease)

        def pos_at(tk):
            if arc[-1] < 1e-6:
                return geo[0]
            s = progress((tk - a) / (b - a)) * arc[-1]
            return np.array([np.interp(s, arc, geo[:, k]) for k in range(3)])

        # Ideal aim at fine resolution, then one smooth non-reversing curve per axis.
        fine = np.arange(a, b + 1e-9, 2.0)
        ideal = []
        for tk in fine:
            pos = pos_at(tk)
            fov = path.at(tk)[2]
            target, frame, _ = resolve_target({}, shot, track, tk, pos, fov, roll_at(tk))
            ideal.append(solve_angles(pos, roll_at(tk), fov, target, frame[0], frame[1], level))
        ideal = np.array(ideal)
        ideal[:, 1] = np.degrees(np.unwrap(np.radians(ideal[:, 1])))
        u = (fine - a) / max(b - a, 1)
        # Pan/tilt on the same operator profile as the dolly: angle = A + B * progress(u), least
        # squares over the whole shot. It eases in, glides, eases out and can never hesitate or
        # rebound. "aimFit": "curve" uses a non-reversing polynomial instead (follows more closely).
        if opt(None, shot, 'aimFit', 'operator') == 'operator':
            P = np.array([progress(x) for x in u])
            M = np.stack([np.ones_like(P), P], 1)
            kp = np.linalg.lstsq(M, ideal[:, 0], rcond=None)[0]
            ky = np.linalg.lstsq(M, ideal[:, 1], rcond=None)[0]
            aim_at = lambda uu: (kp[0] + kp[1] * progress(uu), ky[0] + ky[1] * progress(uu))
        else:
            cp = fit_monotone(u, ideal[:, 0], 3)
            cy = fit_monotone(u, ideal[:, 1], 3)
            aim_at = lambda uu: (np.polyval(cp, uu), np.polyval(cy, uu))
    loose = None
    if not operator and opt(None, shot, 'track', 'loose') != 'tight':
        loose = loose_aim(shot, path, track, a, b, roll_at, level)
    out = []
    for tk in ticks:
        roll = roll_at(tk)
        if operator:
            pos, fov = pos_at(tk), path.at(tk)[2]
            uu = (tk - a) / max(b - a, 1)
            ap, ay = aim_at(uu)
            pitch, yaw = (0.0 if level else float(ap)), float(ay)
        elif loose is not None:
            pos, rot, fov = path.at(tk)
            pitch = float(np.interp(tk, loose[0], loose[1]))
            yaw = float(np.interp(tk, loose[0], loose[2]))
        else:
            pos, rot, fov = path.at(tk)
            target, frame, _ = resolve_target({}, shot, track, tk, pos, fov, roll)
            pitch, yaw = solve_angles(pos, roll, fov, target, frame[0], frame[1], level)
        out.append({'x': round(float(pos[0]), 3), 'y': round(float(pos[1]), 3), 'z': round(float(pos[2]), 3),
                    'pitch': round(pitch, 4), 'yaw': round(yaw, 4), 'roll': round(roll, 3), 'fov': round(float(fov), 3),
                    'tick': tk, 'time': round(tk / TICK_RATE, 4), 'speedmul': 1, 'ease': 0})
    return out


def loose_aim(shot, path, track, a, b, roll_at, level):
    """A camera operator's aim instead of a lock-on: the camera holds still while the subject stays
    inside a dead zone around the framing point ("deadzone": [x, y] in half-frame units, default
    0.22 x 0.15), eases after them with a lag when they leave it ("lag" seconds, default 0.45, a
    critically damped follow), and never lets them past "hardZone" (default 0.6) of the frame.
    "track": "tight" restores the old per-key lock. Returns (ticks, pitch, yaw) per tick."""
    dzx, dzy = opt(None, shot, 'deadzone', [0.22, 0.15])
    lag = max(0.05, float(opt(None, shot, 'lag', 0.45)))
    hard = float(opt(None, shot, 'hardZone', 0.6))
    ticks = np.arange(a, b + 1e-9, 1.0)
    ideal = []
    fovs = []
    for tk in ticks:
        pos, rot, fov = path.at(tk)
        target, frame, _ = resolve_target({}, shot, track, tk, pos, fov, roll_at(tk))
        ideal.append(solve_angles(pos, roll_at(tk), fov, target, frame[0], frame[1], level))
        fovs.append(fov)
    ideal = np.array(ideal)
    ideal[:, 1] = np.degrees(np.unwrap(np.radians(ideal[:, 1])))
    dt = 1.0 / TICK_RATE
    omega = 2.0 / lag
    cur = ideal[0].copy()
    vel = np.zeros(2)
    out = []
    for i in range(len(ticks)):
        th, tv = tan_half(fovs[i])
        dz = np.array([math.degrees(math.atan(dzy * tv)), math.degrees(math.atan(dzx * th))])
        hz = np.array([math.degrees(math.atan(hard * tv)), math.degrees(math.atan(hard * th))])
        err = ideal[i] - cur
        want = cur + np.sign(err) * np.maximum(np.abs(err) - dz, 0.0)
        acc = omega * omega * (want - cur) - 2.0 * omega * vel
        vel = vel + acc * dt
        cur = cur + vel * dt
        # Hard limit: never let the subject leave the safe part of the frame.
        err = ideal[i] - cur
        over = np.abs(err) - hz
        cur = np.where(over > 0, cur + np.sign(err) * over, cur)
        out.append(cur.copy())
    out = np.array(out)
    if level:
        out[:, 0] = 0.0
    return ticks, out[:, 0], out[:, 1]


def refine(shot, markers, track, mesh, log=None):
    """Adjust the curves the way you would in the graph editor: wherever the evaluated path lets the
    subject drift off its composition target (or out of frame), or the lens gets within the clearance
    limit of a wall, insert a key at that tick on the existing curve -- position pushed off the wall,
    angles re-solved onto the target -- and re-evaluate. Stops when the shot holds the tolerances or
    after `maxKeys`. Keys the shot authored with explicit "angles" are never touched."""
    cfg = shot.get('refine') if isinstance(shot.get('refine'), dict) else {}
    tol = cfg.get('tol', 0.45)   # keep the subject in the safe area; finer framing is the aim keys' job
    max_keys = cfg.get('maxKeys', 3)
    gap = cfg.get('gap', 6)
    if any('angles' in k for k in shot['keys']) and not cfg.get('force'):
        return markers
    fps = 32
    a, b = shot.get('range', [markers[0]['tick'], markers[-1]['tick']])
    lo, hi = max(a, markers[0]['tick']), min(b, markers[-1]['tick'])
    for _ in range(max_keys):
        path = Path3(markers)
        worst, worst_tick, fix = 0.0, None, None
        for tk in np.arange(lo, hi + 1e-9, TICK_RATE / fps):
            if min(abs(tk - m['tick']) for m in markers) < gap:
                continue
            pos, rot, fov = path.at(tk)
            pitch, yaw, roll = angles_from_rot(rot)
            target, frame, names = resolve_target({}, shot, track, tk, pos, fov, roll)
            x, y, d = project(pos, rot, fov, [target])
            err = math.hypot(x[0] - frame[0], 0 if opt(None, shot, 'level') else y[0] - frame[1]) if d[0] > 0 else 9
            clear = mesh.clearance(pos)
            score = max(err - tol, 0)
            if score > worst:
                worst, worst_tick, fix = score, tk, (pos, roll, fov, target, frame, clear, err)
        if worst_tick is None:
            break
        pos, roll, fov, target, frame, clear, err = fix
        pos = np.array(pos, float)
        pitch, yaw = solve_angles(pos, roll, fov, target, frame[0], frame[1], bool(opt(None, shot, 'level', False)))
        tick = int(round(worst_tick))
        prev = max((m for m in markers if m['tick'] <= tick), key=lambda m: m['tick'])
        markers.append({'x': round(float(pos[0]), 3), 'y': round(float(pos[1]), 3), 'z': round(float(pos[2]), 3),
                        'pitch': round(pitch, 4), 'yaw': round(yaw, 4), 'roll': float(roll), 'fov': round(float(fov), 3),
                        'tick': tick, 'time': round(tick / TICK_RATE, 4), 'speedmul': 1, 'ease': prev.get('ease', 0),
                        'auto': True})
        markers.sort(key=lambda m: m['tick'])
        if log is not None:
            log.append('    refine: key @%d  (framing error %.2f, clearance %.0f)' % (tick, err, clear))
    return markers


def push_off_walls(mesh, p, want):
    """Move a point away from the nearest collision until it has `want` units of clearance."""
    if mesh.gpu is not None:
        p = np.asarray(p, float)
        for _ in range(4):
            d, inside, cp = mesh.gpu.points_query(p, want * 2)
            if d[0] >= want or inside[0]:
                break
            away = p - cp[0]
            n = np.linalg.norm(away)
            if n < 1e-6:
                break
            p = p + away / n * (want - d[0] + 1.0)
        return p
    for _ in range(6):
        dirs = Mesh.sphere_dirs()
        cand = mesh.candidates(p - want * 2, p + want * 2)
        t, _ = mesh.raycast(np.repeat(p[None], len(dirs), 0), dirs, want * 2, cand)
        if t.min() >= want:
            break
        close = np.isfinite(t) & (t < want)
        push = -(dirs[close] * (want - t[close])[:, None]).sum(0)
        n = np.linalg.norm(push)
        if n < 1e-6:
            break
        p = p + push / n * min(n, want)
    return p


# ------------------------------------------------------------------ analysis

def analyse(shot, path, track, mesh):
    # One sample per demo tick: the subject only moves per tick. All geometry queries for the whole
    # shot are batched (GPU when available): clearance + inside-solid, wall cuts, line of sight.
    fps = min(shot.get('fps', 64), shot.get('analysisFps', 64))
    a, b = shot.get('range', [path.mk[0]['tick'], path.mk[-1]['tick']])
    ticks = np.arange(a, b + 1e-9, TICK_RATE / fps)
    subject = shot.get('subject')
    others = [n for n in track.players if n != subject]
    rows = []
    shake = shake_offsets(path.mk, ticks)
    for tk in ticks:
        pos, rot, fov = path.at(tk)
        row = {'tick': float(tk), 'pos': np.asarray(pos, float), 'rot': rot, 'fov': fov}
        if shake is not None:
            off = shake.get(round(float(tk), 3))
            if off is not None:
                row['base_pos'], row['base_rot'] = row['pos'], rot
                row['pos'], row['rot'], row['fov'] = apply_shake(pos, rot, fov, off)
                row['shake'] = off
        rows.append(row)
    P = np.array([r['pos'] for r in rows])
    clear, inside = mesh.clearance_many(P)
    cut = np.zeros(len(rows), bool)
    if len(rows) > 1:
        cut[1:] = np.isfinite(mesh.segment_hits(P[:-1], P[1:]))
    if subject:
        feet = np.array([track.at(subject, tk) for tk in ticks])
        pts = np.stack([feet + [0, 0, 8], feet + [0, 0, 36], feet + [0, 0, 64]], 1)       # (N,3,3)
        vis = ~np.isfinite(mesh.segment_hits(np.repeat(P, 3, 0), pts.reshape(-1, 3))).reshape(-1, 3)
    if subject:
        in_smoke, smoke_block = track.smoke_check(P, pts[:, 1], ticks)
    else:
        in_smoke, smoke_block = track.smoke_check(P, P, ticks)
    for i, r in enumerate(rows):
        pos, rot, fov, tk = r['pos'], r['rot'], r['fov'], r['tick']
        r['in_smoke'] = bool(in_smoke[i])
        r['smoke_block'] = bool(smoke_block[i])
        r['clear'] = float(clear[i])
        r['solid'] = bool(clear[i] < 1.0 or inside[i])
        r['cut'] = bool(cut[i])
        if subject:
            sx, sy, depth = project(pos, rot, fov, pts[i])
            r['dist'] = float(np.linalg.norm(pts[i, 1] - pos))
            r['vis'] = float(vis[i].mean())
            r['sx'], r['sy'] = float(sx[1]), float(sy[1])
            r['height'] = float(abs(sy[2] - sy[0]) / 2) if depth.min() > 0 else 0.0
            r['inframe'] = bool(depth.min() > 0 and np.all(np.abs(sx) < 1) and np.all(np.abs(sy) < 1))
            r['alive'] = track.alive(subject, tk)
            pitch, yaw, roll = angles_from_rot(rot)
            target, frame, names = resolve_target({}, shot, track, tk, pos, fov, roll)
            tx, ty, td = project(pos, rot, fov, [target])
            r['frame'], r['tx'], r['ty'] = frame, float(tx[0]), float(ty[0])
            level = bool(opt(None, shot, 'level', False))
            r['comp'] = math.hypot(tx[0] - frame[0], 0 if level else ty[0] - frame[1]) if td[0] > 0 else 9.0
            for n in names:
                if n != subject:
                    fx, fy, fd = project(pos, rot, fov, [track.at(n, tk) + [0, 0, 8], track.at(n, tk) + [0, 0, 64]])
                    r['inframe'] = r['inframe'] and bool(fd.min() > 0 and np.all(np.abs(fx) < 1) and np.all(np.abs(fy) < 1))
        r['others'] = []
        for n in others:
            if not track.alive(n, tk) and n not in shot.get('keepDead', []):
                continue
            ft = track.at(n, tk)
            sx, sy, depth = project(pos, rot, fov, [ft + [0, 0, 36]])
            if depth[0] > 0 and abs(sx[0]) < 1.1 and abs(sy[0]) < 1.1:
                r['others'].append(n)
    motion(rows, fps)
    return rows


def motion(rows, fps):
    """Camera motion per frame: angular speed/acceleration (deg/s, deg/s^2), dolly speed/acceleration
    (u/s, u/s^2), FOV rate (deg/s). Acceleration spikes are what reads as a jump or a hitch."""
    n = len(rows)
    angvel = np.zeros((n, 3))
    vel = np.zeros((n, 3))
    for i in range(1, n):
        rel = rows[i]['rot'] * rows[i - 1]['rot'].inv()
        angvel[i] = np.degrees(rel.as_rotvec()) * fps
        vel[i] = (rows[i]['pos'] - rows[i - 1]['pos']) * fps
    if n > 1:
        angvel[0], vel[0] = angvel[1], vel[1]
    angacc = np.vstack([np.zeros((1, 3)), np.diff(angvel, axis=0) * fps])
    acc = np.vstack([np.zeros((1, 3)), np.diff(vel, axis=0) * fps])
    for i, r in enumerate(rows):
        r['angspeed'] = float(np.linalg.norm(angvel[i]))
        r['angacc'] = float(np.linalg.norm(angacc[i])) if i > 1 else 0.0
        r['speed'] = float(np.linalg.norm(vel[i]))
        r['acc'] = float(np.linalg.norm(acc[i])) if i > 1 else 0.0
        r['fovrate'] = float((rows[i]['fov'] - rows[i - 1]['fov']) * fps) if i else 0.0


def summarize(shot, rows):
    lines = []
    bad_clear = [r for r in rows if r['clear'] < CLEARANCE_WARN]
    solid = [r for r in rows if r['solid']]
    cut = [r for r in rows if r['cut']]
    lines.append('%s  frames=%d  ticks %.0f-%.0f' % (shot['name'], len(rows), rows[0]['tick'], rows[-1]['tick']))
    lines.append('  min clearance %.0f u  (warn < %.0f: %d frames)  inside solid: %d frames  path cuts a wall: %d segments'
                 % (min(r['clear'] for r in rows), CLEARANCE_WARN, len(bad_clear), len(solid), len(cut)))
    # The engine's natural cubic FOV/position splines overshoot past sharp key changes (a crash zoom
    # 66 -> 16 -> 16 dips to ~10). Flag it so the keys can be eased instead of discovered in the render.
    keys = shot.get('_markers', [])
    if keys:
        kf = [k['fov'] for k in keys]
        ev = [r['fov'] for r in rows]
        if min(ev) < min(kf) - 2 or max(ev) > max(kf) + 2:
            lines.append('  FOV spline overshoot: keys %.0f-%.0f, evaluated %.0f-%.0f' % (min(kf), max(kf), min(ev), max(ev)))
    keys = shot.get('_markers', [])
    if keys:
        lines.append('  keys: %d (%s)' % (len(keys), shot.get('keyMode', 'strategic')))
    sm_in = [r['tick'] for r in rows if r.get('in_smoke')]
    sm_bl = [r['tick'] for r in rows if r.get('smoke_block')]
    if sm_in or sm_bl:
        lines.append('  SMOKE: camera inside a smoke %d frames%s, smoke blocks the subject %d frames%s'
                     % (len(sm_in), (' (' + ranges(sm_in) + ')') if sm_in else '',
                        len(sm_bl), (' (' + ranges(sm_bl) + ')') if sm_bl else ''))
    sh = [r['shake'] for r in rows if 'shake' in r]
    if sh:
        sh = np.array(sh)
        rotm = np.abs(sh[:, 6:9]).max(0)
        posm = np.linalg.norm(sh[:, 0:3] + 0, axis=1).max()
        st = next((k['shake'] for k in keys if k.get('shake')), None)
        lines.append('  shake %s int %d speed %d: max offset pitch %.2f yaw %.2f roll %.2f deg, position %.1f u, fov %.2f'
                     % (st[0], st[1], st[2], rotm[0], rotm[1], rotm[2], posm, np.abs(sh[:, 9]).max()))
    # Smoothness. Thresholds are for a steady operator; whip pans / crash zooms set "allowWhip".
    angacc = [r['angacc'] for r in rows]
    acc = [r['acc'] for r in rows]
    lines.append('  motion: rotation max %.0f deg/s (accel max %.0f, p95 %.0f deg/s^2), dolly max %.0f u/s (accel max %.0f u/s^2), fov rate max %.0f deg/s'
                 % (max(r['angspeed'] for r in rows), max(angacc), float(np.percentile(angacc, 95)),
                    max(r['speed'] for r in rows), max(acc), max(abs(r['fovrate']) for r in rows)))
    for lab, key in (('pan/tilt', 'angspeed'), ('dolly', 'speed')):
        msg = pulse_report([r[key] for r in rows], [r['tick'] for r in rows])
        if msg and not shot.get('allowWhip'):
            lines.append('    %s %s' % (lab, msg))
    if not shot.get('allowWhip'):
        rb = rebounds(rows)
        if rb:
            lines.append('    REBOUND (pan or tilt reverses mid-move) ticks: %s' % ranges(rb))
        jerky = [r for r in rows if r['angacc'] > ANGACC_WARN or r['acc'] > ACC_WARN]
        if jerky:
            lines.append('    JERKY (rotation accel > %d deg/s^2 or dolly accel > %d u/s^2) ticks: %s'
                         % (ANGACC_WARN, ACC_WARN, ranges([r['tick'] for r in jerky])))
    if 'vis' in rows[0]:
        occl = [r for r in rows if r['vis'] < 0.67 and r.get('alive', True)]
        out = [r for r in rows if not r['inframe']]
        h = [r['height'] for r in rows]
        lines.append('  subject: dist %.0f-%.0f u, size %.0f-%.0f%% of frame height, occluded %d frames, out of frame %d frames'
                     % (min(r['dist'] for r in rows), max(r['dist'] for r in rows), 100 * min(h), 100 * max(h), len(occl), len(out)))
        comp = [r['comp'] for r in rows if r.get('alive', True)]
        st = style_of(shot)
        lines.append('  composition: target %s%s, error mean %.2f max %.2f (screen units, 1 = half the frame)  style: %s'
                     % (opt(None, shot, 'frame', 'center'), ' + lead room' if opt(None, shot, 'lead') == 'auto' else '',
                        float(np.mean(comp)) if comp else 0, max(comp) if comp else 0, shot.get('style', '-')))
        if st.get('notes'):
            lines.append('    style rules: ' + st['notes'])
        drift = [r for r in rows if r['comp'] > 0.2 and r.get('alive', True)]
        if drift:
            lines.append('    off target (>0.2) ticks: %s' % ranges([r['tick'] for r in drift]))
        for lab, sel in (('occluded', occl), ('out of frame', out), ('inside solid', solid), ('clearance', bad_clear)):
            if sel:
                lines.append('    %s ticks: %s' % (lab, ranges([r['tick'] for r in sel])))
    return lines


def pulse_report(speed, ticks, drop=0.35):
    """A move that slows down by more than `drop` of its pace and then picks up again: reads as an
    operator hesitating. Returns a description or None."""
    v = np.asarray(speed, float)
    if len(v) < 8 or v.max() < 3:
        return None
    k = max(1, len(v) // 40)
    v = np.convolve(v, np.ones(k) / k, mode='same')
    worst = None
    for i in range(1, len(v) - 1):
        if v[i] <= v[i - 1] and v[i] <= v[i + 1]:
            before, after = v[:i].max(), v[i:].max()
            peak = min(before, after)
            if peak > 3 and v[i] < (1 - drop) * peak:
                depth = 1 - v[i] / peak
                if worst is None or depth > worst[0]:
                    worst = (depth, ticks[i], peak, v[i])
    if worst:
        return 'PULSES: slows to %.0f%% of its pace at tick %.0f (%.0f -> %.0f) then speeds up again' % (
            100 * (1 - worst[0]), worst[1], worst[2], worst[3])
    return None


def rebounds(rows, min_swing=1.0):
    """Ticks where the pan or tilt turns around and swings back more than `min_swing` degrees before
    turning again -- the "went back the other way" that reads as not hand-operated."""
    out = []
    ang = np.array([angles_from_rot(r['rot'])[:2] for r in rows])
    ang[:, 1] = np.degrees(np.unwrap(np.radians(ang[:, 1])))
    for k in range(2):
        v = ang[:, k]
        turns = [0]
        direction = 0
        for i in range(1, len(v)):
            dv = v[i] - v[turns[-1]] if direction == 0 else v[i] - v[i - 1]
            if direction == 0:
                if abs(dv) > 0.2:
                    direction = 1 if dv > 0 else -1
            elif dv * direction < 0:
                # candidate turn at i-1; confirm only if it swings back far enough
                j = i
                while j < len(v) and (v[j] - v[i - 1]) * direction <= 0:
                    if abs(v[j] - v[i - 1]) > min_swing:
                        out.append(rows[i - 1]['tick'])
                        direction = -direction
                        break
                    j += 1
                else:
                    pass
    return sorted(set(out))


def ranges(ticks):
    out, start, last = [], None, None
    for t in ticks:
        if start is None:
            start = last = t
        elif t - last > 1.01:
            out.append((start, last))
            start = last = t
        else:
            last = t
    if start is not None:
        out.append((start, last))
    return ', '.join('%.0f' % a if a == b else '%.0f-%.0f' % (a, b) for a, b in out)


# ------------------------------------------------------------------ drawing

def render_view_gpu(mesh, track, shot, row, size):
    """Ray-traced preview (one GPU ray per pixel): flat shading by face normal, depth fog, face-edge
    lines, players drawn with depth-tested occlusion."""
    W, H = size
    pos, rot, fov = row['pos'], row['rot'], row['fov']
    m = rot.as_matrix()
    fwd, left, up = m[:, 0], m[:, 1], m[:, 2]
    th, tv = tan_half(fov)
    depth, face = mesh.gpu.image(pos, fwd, -left, up, th, tv, W, H, 8000.0)
    hit = face >= 0
    nrm = np.zeros((H, W, 3))
    nrm[hit] = mesh.normal[face[hit]]
    light = np.array([0.35, 0.25, 0.9]); light /= np.linalg.norm(light)
    lam = 0.35 + 0.65 * np.clip(nrm @ light, 0, None)
    base = np.where((nrm[..., 2] > 0.7)[..., None], [150, 140, 125], np.where((np.abs(nrm[..., 2]) < 0.3)[..., None], [120, 130, 150], [135, 135, 135]))
    fog = np.clip(np.where(hit, depth, 4000) / 4000.0, 0, 1)[..., None]
    col = base * lam[..., None] * (1 - fog) + np.array([18, 20, 26]) * fog
    edge = np.zeros((H, W), bool)
    edge[:, 1:] |= face[:, 1:] != face[:, :-1]
    edge[1:, :] |= face[1:, :] != face[:-1, :]
    col[edge & hit] *= 0.8
    col[~hit] = [18, 20, 26]
    img = Image.fromarray(np.clip(col, 0, 255).astype(np.uint8))
    return img, depth


def render_view(mesh, track, shot, row, size=(640, 270)):
    """Flat-shaded painter's-algorithm render of the collision from the camera, players on top.
    Culled vectorised first (behind the lens, beyond `far`, back faces, outside the frustum, smaller
    than a fraction of a pixel), so only what can show gets drawn."""
    W, H = size
    pos, rot, fov = row['pos'], row['rot'], row['fov']
    m = rot.as_matrix()
    fwd, left, up = m[:, 0], m[:, 1], m[:, 2]
    th, tv = tan_half(fov)
    if mesh.gpu is not None:
        img, _ = render_view_gpu(mesh, track, shot, row, size)
        draw = ImageDraw.Draw(img)
        return draw_overlay(img, draw, mesh, track, shot, row, size)
    img = Image.new('RGB', size, (18, 20, 26))
    draw = ImageDraw.Draw(img)
    near, far = 2.0, 6000.0
    rel = mesh.tris - pos
    d = rel @ fwd
    X = -(rel @ left)
    Y = rel @ up
    keep = (d.max(1) > near) & (d.min(1) < far)
    keep &= np.einsum('tk,tk->t', mesh.normal, -rel[:, 0]) > 0
    front = d.min(1) > near
    with np.errstate(divide='ignore', invalid='ignore'):
        outside = ((X < -th * d).all(1) | (X > th * d).all(1) | (Y < -tv * d).all(1) | (Y > tv * d).all(1))
        keep &= ~(front & outside)
        sx = W / 2 * (1 + X / d / th)
        sy = H / 2 * (1 - Y / d / tv)
        area = 0.5 * np.abs((sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sx[:, 2] - sx[:, 0]) * (sy[:, 1] - sy[:, 0]))
    keep &= ~(front & (area < 0.2))
    idx = np.nonzero(keep)[0]
    light = np.array([0.35, 0.25, 0.9])
    light /= np.linalg.norm(light)
    nrm = mesh.normal[idx]
    lam = 0.35 + 0.65 * np.clip(nrm @ light, 0, None)
    base = np.where((nrm[:, 2] > 0.7)[:, None], [150, 140, 125], np.where((np.abs(nrm[:, 2]) < 0.3)[:, None], [120, 130, 150], [135, 135, 135]))
    depth = d[idx].mean(1)
    fog = np.clip(depth / 4000.0, 0, 1)[:, None]
    col = (base * lam[:, None] * (1 - fog) + np.array([18, 20, 26]) * fog).astype(int)
    order = np.argsort(-depth)
    for j in order:
        i = idx[j]
        c = tuple(int(v) for v in col[j])
        oc = tuple(max(0, v - 30) for v in c)
        if front[i]:
            draw.polygon(list(zip(sx[i], sy[i])), fill=c, outline=oc)
            continue
        v = clip_near(rel[i], d[i], near, fwd)
        if v is None:
            continue
        dv = v @ fwd
        px = W / 2 * (1 + (-(v @ left)) / dv / th)
        py = H / 2 * (1 - (v @ up) / dv / tv)
        draw.polygon(list(zip(px, py)), fill=c, outline=oc)
    return draw_overlay(img, draw, mesh, track, shot, row, size)


def draw_overlay(img, draw, mesh, track, shot, row, size):
    W, H = size
    pos, rot, fov = row['pos'], row['rot'], row['fov']
    th, tv = tan_half(fov)
    subject = shot.get('subject')
    for name, p in track.players.items():
        tk = row['tick']
        alive = track.alive(name, tk)
        if not alive and name != subject:
            continue
        feet = track.at(name, tk)
        pts = [feet, feet + [0, 0, 72]]
        sx, sy, depth = project(pos, rot, fov, pts)
        if depth.min() <= 1:
            continue
        x = W / 2 * (1 + sx)
        y = H / 2 * (1 - sy)
        w = max(2, PLAYER_RADIUS / depth.mean() / th * W / 2)
        seen = not np.isfinite(mesh.segment_hit(pos, feet + [0, 0, 40]))
        col = (255, 60, 60) if name == subject else (90, 200, 255) if p['team'] == track.players[subject]['team'] else (255, 200, 60)
        if seen:
            draw.rounded_rectangle([x.min() - w, y.min() - w * 0.4, x.max() + w, y.max()], radius=w, fill=col)
        else:
            draw.rounded_rectangle([x.min() - w, y.min() - w * 0.4, x.max() + w, y.max()], radius=w, outline=col, width=2)
        draw.text((x.max() + w + 3, y.min()), name, fill=col, font=font(11))
    # Composition guides: thirds (grey), golden ratio (amber), and the shot's framing target.
    for f in (1 / 3, 2 / 3):
        draw.line([(W * f, 0), (W * f, H)], fill=(80, 80, 80))
        draw.line([(0, H * f), (W, H * f)], fill=(80, 80, 80))
    for f in (0.382, 0.618):
        draw.line([(W * f, 0), (W * f, H)], fill=(110, 90, 30))
        draw.line([(0, H * f), (W, H * f)], fill=(110, 90, 30))
    if 'frame' in row:
        fx, fy = W / 2 * (1 + row['frame'][0]), H / 2 * (1 - row['frame'][1])
        draw.line([(fx - 8, fy), (fx + 8, fy)], fill=(0, 255, 120), width=2)
        draw.line([(fx, fy - 8), (fx, fy + 8)], fill=(0, 255, 120), width=2)
        px, py = W / 2 * (1 + row['tx']), H / 2 * (1 - row['ty'])
        draw.ellipse([px - 4, py - 4, px + 4, py + 4], outline=(0, 255, 120), width=2)
    warn = []
    if row['solid']:
        warn.append('INSIDE SOLID')
    elif row['clear'] < CLEARANCE_WARN:
        warn.append('CLEAR %.0fu' % row['clear'])
    if row['cut']:
        warn.append('CUTS WALL')
    if 'vis' in row and row['vis'] < 0.67 and row.get('alive', True):
        warn.append('OCCLUDED')
    if 'inframe' in row and not row['inframe']:
        warn.append('OUT OF FRAME')
    label = 'tick %.0f  fov %.0f' % (row['tick'], fov)
    if 'dist' in row:
        label += '  dist %.0f  size %.0f%%' % (row['dist'], 100 * row['height'])
    if 'comp' in row:
        label += '  comp %.2f' % row['comp']
    draw.rectangle([0, 0, W, 16], fill=(0, 0, 0))
    draw.text((4, 1), label, fill=(230, 230, 230), font=font(12))
    if warn:
        draw.text((W - 8 - 7 * len(' '.join(warn)), 1), ' '.join(warn), fill=(255, 80, 80), font=font(12))
    return img


def clip_near(v, d, near, fwd):
    out = []
    for k in range(3):
        a, b = v[k], v[(k + 1) % 3]
        da, db = d[k], d[(k + 1) % 3]
        if da >= near:
            out.append(a)
        if (da >= near) != (db >= near):
            t = (near - da) / (db - da)
            out.append(a + (b - a) * t)
    return np.array(out) if len(out) >= 3 else None


def contact_sheet(mesh, track, shot, rows, frames, out):
    idx = np.unique(np.linspace(0, len(rows) - 1, frames).round().astype(int))
    tiles = [render_view(mesh, track, shot, rows[i]) for i in idx]
    cols = 2
    W, H = tiles[0].size
    sheet = Image.new('RGB', (cols * W + (cols - 1) * 4, ((len(tiles) + cols - 1) // cols) * (H + 4)), (0, 0, 0))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * (W + 4), (i // cols) * (H + 4)))
    sheet.save(out)


def slice_segments(tris, z):
    """Where triangles cross the plane at height z, as 2D line segments."""
    d = tris[:, :, 2] - z
    pts, crosses = [], []
    for a, b in ((0, 1), (1, 2), (2, 0)):
        cross = (d[:, a] > 0) != (d[:, b] > 0)
        with np.errstate(divide='ignore', invalid='ignore'):
            f = np.where(cross, d[:, a] / np.where(cross, d[:, a] - d[:, b], 1), 0)
        pts.append(tris[:, a, :2] + (tris[:, b, :2] - tris[:, a, :2]) * f[:, None])
        crosses.append(cross)
    pts = np.stack(pts, 1)            # (T, 3 edges, 2)
    crosses = np.stack(crosses, 1)    # (T, 3)
    two = crosses.sum(1) == 2
    if not two.any():
        return np.zeros((0, 2, 2))
    first = np.argmax(crosses[two], axis=1)
    last = 2 - np.argmax(crosses[two][:, ::-1], axis=1)
    rows = np.nonzero(two)[0]
    return np.stack([pts[rows, first], pts[rows, last]], 1)


def plan_image(mesh, track, shot, rows, out, extra=None):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import LineCollection, PolyCollection

    subject = shot.get('subject')
    cams = np.array([r['pos'] for r in rows])
    pts = [cams]
    if subject:
        sel = [r['tick'] for r in rows]
        pts.append(np.array([track.at(subject, t) for t in sel]))
    allp = np.concatenate(pts)
    lo, hi = allp.min(0) - 350, allp.max(0) + 350
    # Floor height under the subject (his lowest point in the shot, not mid-jump).
    zref = min(track.at(subject, r['tick'])[2] for r in rows) if subject else cams[:, 2].mean()
    for p in shot.get('_bounds', []):
        lo, hi = np.minimum(lo, np.asarray(p) - 150), np.maximum(hi, np.asarray(p) + 150)
    asp = float(np.clip((hi[1] - lo[1]) / max(hi[0] - lo[0], 1), 0.45, 1.5))
    fig, ax = plt.subplots(figsize=(12, 12 * asp), dpi=110)
    t = mesh.tris
    inside = (t[:, :, 0].max(1) > lo[0]) & (t[:, :, 0].min(1) < hi[0]) & (t[:, :, 1].max(1) > lo[1]) & (t[:, :, 1].min(1) < hi[1])
    # Floor plan: walkable surfaces up to just above the subject's feet (no ceilings or pipes),
    # shaded by height, then the walls cut at waist and head height like an architectural plan.
    floor = inside & (mesh.normal[:, 2] > 0.6) & (t[:, :, 2].max(1) < zref + 30) & (t[:, :, 2].min(1) > zref - 400)
    order = np.argsort(t[floor][:, :, 2].mean(1))
    fz = t[floor][order]
    pc = PolyCollection(fz[:, :, :2], array=fz[:, :, 2].mean(1), cmap='Greys_r', edgecolors='none', alpha=0.9)
    pc.set_clim(zref - 300, zref + 60)
    ax.add_collection(pc)
    for zc, col, lw in ((zref + 130, '#9a9a9a', 0.7), (zref + 40, '#101010', 1.3)):
        seg = slice_segments(t[inside], zc)
        if len(seg):
            ax.add_collection(LineCollection(seg, colors=col, linewidths=lw))
    for name, p in track.players.items():
        m = (p['tick'] >= rows[0]['tick'] - 64) & (p['tick'] <= rows[-1]['tick'] + 32)
        if not m.any():
            continue
        xy = p['pos'][m]
        col = 'red' if name == subject else '#2a9df4' if subject and p['team'] == track.players[subject]['team'] else '#e8a400'
        ax.plot(xy[:, 0], xy[:, 1], '-', color=col, lw=2 if name == subject else 1)
        ax.text(xy[-1, 0], xy[-1, 1], ' ' + name, color=col, fontsize=8)
        if name == subject:
            for tk in range(int(rows[0]['tick']) // 16 * 16, int(rows[-1]['tick']) + 1, 16):
                q = track.at(name, tk)
                ax.plot(q[0], q[1], 'o', color='red', ms=3)
                ax.text(q[0], q[1], ' %d' % tk, color='darkred', fontsize=6)
    for e in track.events:
        if rows[0]['tick'] - 32 <= e['tick'] <= rows[-1]['tick'] + 32 and e['kind'] == 'kill':
            ax.plot(e['pos'][0], e['pos'][1], 'x', color='black', ms=10, mew=2)
            ax.text(e['pos'][0], e['pos'][1], '  kill %d %s' % (e['tick'], e['victim']), fontsize=7)
    ax.plot(cams[:, 0], cams[:, 1], '-', color='#00b050', lw=2)
    step = max(1, len(rows) // 10)
    for i in ([] if shot.get('_nofrustum') else list(range(0, len(rows), step)) + [len(rows) - 1]):
        r = rows[i]
        m = r['rot'].as_matrix()
        fwd = m[:, 0][:2]
        fwd = fwd / (np.linalg.norm(fwd) + 1e-9)
        th, _ = tan_half(r['fov'])
        a = math.atan(th)
        L = r.get('dist', 300)
        for s in (-1, 1):
            c, sn = math.cos(s * a), math.sin(s * a)
            d = np.array([fwd[0] * c - fwd[1] * sn, fwd[0] * sn + fwd[1] * c])
            ax.plot([r['pos'][0], r['pos'][0] + d[0] * L], [r['pos'][1], r['pos'][1] + d[1] * L], '-', color='#00b050', lw=0.5, alpha=0.7)
        ax.text(r['pos'][0], r['pos'][1], ' %.0f' % r['tick'], color='#006020', fontsize=6)
    bad = [r for r in rows if r['solid'] or r['cut'] or r['clear'] < CLEARANCE_WARN]
    if bad:
        b = np.array([r['pos'] for r in bad])
        ax.plot(b[:, 0], b[:, 1], 'o', color='magenta', ms=4)
    for k in shot.get('_markers', []):
        ax.plot(k['x'], k['y'], 's', color='#00b050', ms=6)
    if extra:
        extra(ax)
    ax.set_xlim(lo[0], hi[0])
    ax.set_ylim(lo[1], hi[1])
    ax.set_aspect('equal')
    ax.set_title('%s  (green: camera path + frustum, red: subject, magenta: clearance/solid/wall-cut)' % shot['name'], fontsize=9)
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)


def curves_image(shot, rows, markers, out):
    """Graph-editor view of the evaluated shot: angles, motion, FOV and where the subject sits."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    tk = np.array([r['tick'] for r in rows])
    ang = np.array([angles_from_rot(r['rot']) for r in rows])
    ang[:, 1] = np.degrees(np.unwrap(np.radians(ang[:, 1])))
    fig, ax = plt.subplots(5, 1, figsize=(11, 11), sharex=True, dpi=100)
    for k, lab in enumerate(('pitch', 'yaw', 'roll')):
        ax[0].plot(tk, ang[:, k], label=lab)
    ax[0].legend(fontsize=8); ax[0].set_ylabel('deg')
    ax[1].plot(tk, [r['angspeed'] for r in rows], label='rotation speed deg/s')
    ax[1].plot(tk, [r['angacc'] / 10 for r in rows], label='rotation accel /10')
    ax[1].axhline(ANGACC_WARN / 10, color='r', lw=0.6, ls='--')
    ax[1].legend(fontsize=8)
    ax[2].plot(tk, [r['speed'] for r in rows], label='dolly speed u/s')
    ax[2].plot(tk, [r['acc'] / 10 for r in rows], label='dolly accel /10')
    ax[2].axhline(ACC_WARN / 10, color='r', lw=0.6, ls='--')
    ax[2].legend(fontsize=8)
    ax[3].plot(tk, [r['fov'] for r in rows], label='fov'); ax[3].legend(fontsize=8)
    if 'tx' in rows[0]:
        ax[4].plot(tk, [r['tx'] for r in rows], label='subject x'); ax[4].plot(tk, [r['ty'] for r in rows], label='subject y')
        ax[4].plot(tk, [r['frame'][0] for r in rows], 'k--', lw=0.6); ax[4].plot(tk, [r['frame'][1] for r in rows], 'k:', lw=0.6)
        for v in (-1 / 3, 1 / 3):
            ax[4].axhline(v, color='grey', lw=0.4)
        ax[4].set_ylim(-1.1, 1.1); ax[4].legend(fontsize=8)
    for a in ax:
        for m in markers:
            a.axvline(m['tick'], color='green' if not m.get('auto') else 'orange', lw=0.5, alpha=0.6)
    ax[0].set_title('%s  (green: keys, orange: refine keys)' % shot['name'], fontsize=9)
    fig.tight_layout(); fig.savefig(out); plt.close(fig)


# ------------------------------------------------------------------ project.mvm

MAGIC = b'SRCMVM\x1a\x00'


def read_project(path):
    data = Path(path).read_bytes()
    assert data[:8] == MAGIC, 'not a project.mvm'
    version, flags, count = struct.unpack_from('<III', data, 8)
    sections = []
    for i in range(count):
        e = 32 + 32 * i
        tag = data[e:e + 16].rstrip(b'\0').decode()
        enc, crc, off, size = struct.unpack_from('<IIII', data, e + 16)
        sections.append([tag, data[off:off + size]])
    return flags, sections


def write_project(path, flags, sections):
    live = [s for s in sections if s[1]]
    table, body, off = b'', b'', 32 + 32 * len(live)
    for tag, payload in live:
        table += tag.encode().ljust(16, b'\0') + struct.pack('<IIII', 1, zlib.crc32(payload) & 0xffffffff, off + len(body), len(payload))
        body += payload
    tmp = Path(str(path) + '.tmp')
    tmp.write_bytes(MAGIC + struct.pack('<IIIIQ', 1, flags, len(live), 0, 0) + table + body)
    os.replace(tmp, path)


def campath_payload(markers):
    doc = {'version': 2, 'speedMode': 0, 'timing': 0, 'interp': 1, 'constSpeed': 1, 'autoSnap': 0,
           'selected': 0, 'markers': markers}
    return json.dumps(doc, separators=(',', ':')).encode()


# ------------------------------------------------------------------ commands

def load_setup(path):
    spec = json.loads(Path(path).read_text())
    base = Path(path).parent
    track = Track(base / spec['track'] if not os.path.isabs(spec['track']) else spec['track'])
    center = np.mean([p['pos'][len(p['pos']) // 2] for p in track.players.values()], axis=0)
    if spec.get('center'):
        center = np.array(spec['center'], float)
    cache = spec.get('mapCache', os.path.expandvars(r'%APPDATA%\HLAE\source_mvm\environment\occlusion'))
    mesh = Mesh.load(cache, spec['map'], center, spec.get('radius', 3500))
    out = Path(spec.get('out', base / 'plans'))
    out.mkdir(parents=True, exist_ok=True)
    return spec, track, mesh, out


def shot_region(shot, track, mesh, margin=1500.0):
    """Only the collision a shot can touch or see: a box around its keys and the players over its
    range, plus `margin` for what the lens looks at. Everything else in the map is skipped."""
    pts = []
    for k in shot['keys']:
        if 'pos' in k:
            pts.append(k['pos'])
    a, b = shot.get('range', [shot['keys'][0]['tick'], shot['keys'][-1]['tick']])
    for name in [shot.get('subject')] + list(shot.get('subjects', [])):
        if name and name in track.players:
            for tk in np.linspace(a, b, 12):
                pts.append(track.at(name, tk))
    pts = np.array(pts, float)
    return mesh.subset(pts.min(0) - margin, pts.max(0) + margin)


def cmd_plan(args):
    spec, track, mesh, out = load_setup(args.shots)
    report = []
    for shot in spec['shots']:
        if args.only and shot['name'] not in args.only:
            continue
        log = []
        if 'generate' in shot and not shot.get('_generated'):
            shot['keys'] = generate_keys(shot, track, mesh)
            shot['_generated'] = True
        region = shot_region(shot, track, mesh)
        markers = shot_markers(shot, track, region, log)
        shot['_markers'] = markers
        path = Path3(markers)
        mesh_all, mesh = mesh, region
        rows = analyse(shot, path, track, mesh)
        lines = summarize(shot, rows) + log
        report += lines + ['']
        print('\n'.join(lines), flush=True)
        (out / (shot['name'] + '.markers.json')).write_text(json.dumps(markers, indent=1))
        plan_image(mesh, track, shot, rows, out / (shot['name'] + '.plan.png'))
        curves_image(shot, rows, markers, out / (shot['name'] + '.curves.png'))
        contact_sheet(mesh, track, shot, rows, args.frames, out / (shot['name'] + '.sheet.png'))
        mesh = mesh_all
    (out / 'report.txt').write_text('\n'.join(report))
    print('wrote', out)


def cmd_scout(args):
    spec, track, mesh, out = load_setup(args.shots)
    a, b = (int(x) for x in args.ticks.split('-'))
    ticks = np.arange(a, b + 1, 4)
    mid = track.at(args.subject, (a + b) / 2)
    res = []
    for dist in (float(x) for x in args.dist.split(',')):
        for z in (float(x) for x in args.height.split(',')):
            for az in range(0, 360, args.step):
                p = mid + [math.cos(math.radians(az)) * dist, math.sin(math.radians(az)) * dist, z]
                c = mesh.clearance(p)
                if c < 24 or mesh.inside_solid(p):
                    res.append((az, dist, z, p, c, 0.0))
                    continue
                vis = np.mean([not np.isfinite(mesh.segment_hit(p, track.at(args.subject, t) + [0, 0, 40])) for t in ticks])
                res.append((az, dist, z, p, c, vis))
    res.sort(key=lambda r: -r[5])
    lines = ['scout %s ticks %d-%d around (%.0f %.0f %.0f)' % (args.subject, a, b, *mid)]
    for az, dist, z, p, c, vis in res:
        if vis > 0:
            lines.append('  azim %3d dist %4.0f z %+4.0f  pos (%.0f %.0f %.0f)  clear %3.0f  visible %3.0f%%' % (az, dist, z, *p, c, vis * 100))
    print('\n'.join(lines[:60]))
    (out / ('scout_%s_%d_%d.txt' % (args.subject, a, b))).write_text('\n'.join(lines))

    def extra(ax):
        for az, dist, z, p, c, vis in res:
            ax.plot(p[0], p[1], 'o', ms=4, color=(1 - vis, vis, 0) if vis > 0 else (0.3, 0.3, 0.3))
    shot = {'name': 'scout_%s_%d_%d' % (args.subject, a, b), 'subject': args.subject, '_nofrustum': True,
            '_bounds': [r[3] for r in res if r[5] > 0]}
    rows = [{'tick': float(t), 'pos': track.at(args.subject, t) + [0, 0, 64], 'rot': rot_from_angles(0, 0, 0), 'fov': 50,
             'clear': 999, 'solid': False, 'cut': False} for t in ticks]
    plan_image(mesh, track, shot, rows, out / (shot['name'] + '.png'), extra)


def cmd_probe(args):
    spec, track, mesh, out = load_setup(args.shots)
    p = np.array([args.x, args.y, args.z])
    print('clearance %.1f  inside solid %s' % (mesh.clearance(p), mesh.inside_solid(p)))


def cmd_pose(args):
    """The shot's pose at a tick as mirv_input commands (for an in-game still at that tick)."""
    spec, track, mesh, out = load_setup(args.shots)
    shot = next(s for s in spec['shots'] if s['name'] == args.name)
    pos, rot, fov = Path3(shot_markers(shot, track, mesh)).at(args.tick)
    pitch, yaw, roll = angles_from_rot(rot)
    print('mirv_input position %.2f %.2f %.2f;;mirv_input angles %.3f %.3f %.3f;;mirv_input fov %.2f'
          % (pos[0], pos[1], pos[2], pitch, yaw, roll, fov))
    if args.still:
        row = {'tick': args.tick, 'pos': pos, 'rot': rot, 'fov': fov, 'clear': mesh.clearance(pos), 'solid': False, 'cut': False}
        render_view(mesh, track, shot, row, size=(1280, 540)).save(args.still)


def cmd_takecheck(args):
    """Motion metrics of a rendered take, from the camera the render actually used (frames.csv)."""
    import csv
    take = Path(args.take)
    rows = []
    with open(take / 'frames.csv', newline='') as fh:
        for r in csv.DictReader(fh):
            rows.append({'tick': float(r['demo_tick']), 'pos': np.array([float(r['camera_x']), float(r['camera_y']), float(r['camera_z'])]),
                         'rot': rot_from_angles(float(r['pitch']), float(r['yaw']), float(r['roll'])), 'fov': float(r['fov'])})
    manifest = json.loads((take / 'manifest.json').read_text())
    fps = manifest.get('captureFps', 64)
    motion(rows, fps)
    angacc = [r['angacc'] for r in rows]
    acc = [r['acc'] for r in rows]
    frozen = sum(1 for i in range(1, len(rows)) if np.allclose(rows[i]['pos'], rows[i - 1]['pos']) and rows[i]['angspeed'] < 1e-3)
    print('%s: %d frames @ %d fps, camera %s' % (take.name, len(rows), fps, manifest.get('cameraSource')))
    print('  rotation max %.0f deg/s (accel max %.0f, p95 %.0f), dolly max %.0f u/s (accel max %.0f), fov %.0f-%.0f, frozen frames %d'
          % (max(r['angspeed'] for r in rows), max(angacc), float(np.percentile(angacc, 95)), max(r['speed'] for r in rows),
             max(acc), min(r['fov'] for r in rows), max(r['fov'] for r in rows), frozen))
    jerky = [r for r in rows if r['angacc'] > ANGACC_WARN or r['acc'] > ACC_WARN]
    if jerky and not args.allow_whip:
        print('  JERKY ticks: %s' % ranges([r['tick'] for r in jerky]))
    if args.shots and args.shot:
        # The take must be the shot that was planned: compare the camera it used with the plan.
        spec, track, mesh, out = load_setup(args.shots)
        shot = next(s for s in spec['shots'] if s['name'] == args.shot)
        markers = shot_markers(shot, track, mesh)
        path = Path3(markers)
        a = manifest.get('startTick', rows[0]['tick'])
        # The plan INCLUDING its path shake: a take rendered without the shake must not pass.
        fticks = np.array([a + i * TICK_RATE / fps for i in range(len(rows))])
        shake = shake_offsets(markers, fticks)
        perr, aerr = [], []
        for i, r in enumerate(rows):
            pos, rot, fov = path.at(fticks[i])
            if shake is not None and round(float(fticks[i]), 3) in shake:
                pos, rot, fov = apply_shake(pos, rot, fov, shake[round(float(fticks[i]), 3)])
            perr.append(float(np.linalg.norm(pos - r['pos'])))
            aerr.append(float(np.degrees((r['rot'] * rot.inv()).magnitude())))
        bad = max(perr) > 30 or max(aerr) > (0.5 if shake is not None else 8)
        print('  vs plan %s: position error max %.1f u, angle error max %.1f deg -> %s'
              % (args.shot, max(perr), max(aerr), 'MISMATCH (wrong path or frozen camera)' if bad else 'ok'))
        if bad:
            sys.exit(3)


def cmd_install(args):
    spec, track, mesh, out = load_setup(args.shots)
    shot = next(s for s in spec['shots'] if s['name'] == args.name)
    full = shot_markers(shot, track, mesh)
    markers = [{k: v for k, v in m.items() if k not in ('auto', 'shake')} for m in full]
    proj = Path(spec['project'])
    backup = Path(str(proj) + '.user-backup')
    if proj.exists():
        if not backup.exists():
            shutil.copy2(proj, backup)
        flags, sections = read_project(proj)
    else:
        # A demo never opened in the editor has no project yet: start one the way the hook does
        # (CS2 game flag + the "project" info section naming the demo).
        proj.parent.mkdir(parents=True, exist_ok=True)
        info = {'game': 'cs2', 'demoPath': spec.get('demoPath', '')}
        flags, sections = 1, [['project', json.dumps(info, separators=(',', ':')).encode()]]
    for s in sections:
        if s[0] == 'campath':
            s[1] = campath_payload(markers)
            break
    else:
        sections.append(['campath', campath_payload(markers)])
    write_project(proj, flags, sections)
    print('installed %s: %d markers, ticks %d-%d -> %s' % (shot['name'], len(markers), markers[0]['tick'], markers[-1]['tick'], proj))
    # The driver applies these live after `marker load` (pathshake set <marker index> ...).
    shakes = [(i, m['shake']) for i, m in enumerate(full) if m.get('shake')]
    print('SHAKE ' + ';'.join('%d %s %d %d' % (i, s[0], s[1], s[2]) for i, s in shakes))


def cmd_restore(args):
    spec = json.loads(Path(args.shots).read_text())
    proj = Path(spec['project'])
    backup = Path(str(proj) + '.user-backup')
    shutil.copy2(backup, proj)
    print('restored', proj, 'from', backup)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('plan')
    p.add_argument('shots')
    p.add_argument('--only', nargs='*')
    p.add_argument('--frames', type=int, default=8)
    p.set_defaults(fn=cmd_plan)
    p = sub.add_parser('scout')
    p.add_argument('shots')
    p.add_argument('--subject', required=True)
    p.add_argument('--ticks', required=True)
    p.add_argument('--dist', default='250,450,700,1000')
    p.add_argument('--height', default='20,90,220')
    p.add_argument('--step', type=int, default=15)
    p.set_defaults(fn=cmd_scout)
    p = sub.add_parser('probe')
    p.add_argument('shots')
    p.add_argument('x', type=float)
    p.add_argument('y', type=float)
    p.add_argument('z', type=float)
    p.set_defaults(fn=cmd_probe)
    p = sub.add_parser('pose')
    p.add_argument('shots')
    p.add_argument('name')
    p.add_argument('tick', type=float)
    p.add_argument('--still')
    p.set_defaults(fn=cmd_pose)
    p = sub.add_parser('shakes')
    p.set_defaults(fn=lambda a: print('\n'.join('%-18s %s' % (k, v[1]) for k, v in sorted(shake_styles().items(), key=lambda kv: kv[1][0]) if not k.isupper() and k != v[1].lower())))
    p = sub.add_parser('takecheck')
    p.add_argument('take')
    p.add_argument('--allow-whip', action='store_true')
    p.add_argument('--shots')
    p.add_argument('--shot')
    p.set_defaults(fn=cmd_takecheck)
    p = sub.add_parser('install')
    p.add_argument('shots')
    p.add_argument('name')
    p.set_defaults(fn=cmd_install)
    p = sub.add_parser('restore')
    p.add_argument('shots')
    p.set_defaults(fn=cmd_restore)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
