"""Builds a realistic procedural Russian thistle (Kali tragus) tumbleweed GLB.

Generates a botanically structured, multi-order tangled dry bush with:
  - A gnarled broken root crown slightly off-centre
  - Four branching orders (woody main stems -> arching secondary basketwork ->
    zig-zag tertiary twigs -> fine curling terminal forklets and dried bract spurs)
  - Watertight manifold quad tubes (8 sides on main stems, 4 sides on twigs,
    capped tips, parallel-transport frames, outward-facing winding)
  - Embedded 512x512 dry-straw / weathered-bark albedo and normal PNG textures,
    with radial self-occlusion baked across U (dark woody core to sun-bleached
    straw tips) and fibrous stem grain along V.

Output: Assets/environment/tumbleweed/tumbleweed.glb (normalized to ~[-1, 1] by
EnvironmentGltfMath.h on load). Pass --preview <path.png> to render a 4-view
shaded inspection sheet.
"""

import argparse
import io
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image


def normalize(v):
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return np.where(n > 1e-8, v / np.maximum(n, 1e-8), np.array([0.0, 1.0, 0.0]))


def ortho_basis(tangent):
    t = tangent / max(float(np.linalg.norm(tangent)), 1e-8)
    ref = np.array([1.0, 0.0, 0.0]) if abs(t[0]) < 0.85 else np.array([0.0, 1.0, 0.0])
    u = np.cross(t, ref)
    u /= max(float(np.linalg.norm(u)), 1e-8)
    v = np.cross(t, u)
    return u, v


def transport_frame(prev_t, prev_u, new_t):
    """Parallel-transport (Bishop) frame along a curve to avoid tube pinching."""
    axis = np.cross(prev_t, new_t)
    s = float(np.linalg.norm(axis))
    c = float(np.clip(np.dot(prev_t, new_t), -1.0, 1.0))
    if s < 1e-6:
        if c < 0.0:
            u = -prev_u
        else:
            u = prev_u
    else:
        axis /= s
        angle = math.atan2(s, c)
        # Rodrigues rotation of prev_u around axis by angle
        u = (
            prev_u * math.cos(angle)
            + np.cross(axis, prev_u) * math.sin(angle)
            + axis * float(np.dot(axis, prev_u)) * (1.0 - math.cos(angle))
        )
    u = u - new_t * float(np.dot(u, new_t))
    nu = float(np.linalg.norm(u))
    if nu < 1e-6:
        u, v = ortho_basis(new_t)
    else:
        u /= nu
        v = np.cross(new_t, u)
    return u, v


def outer_shell_radius(direction, rng_phases):
    """Organically lumpy, slightly oblate Russian thistle silhouette envelope."""
    dx, dy, dz = float(direction[0]), float(direction[1]), float(direction[2])
    # Slightly flattened in Y (glTF Y-up) and gently oval in XZ
    base = 1.0 / math.sqrt((dx / 1.04) ** 2 + (dy / 0.88) ** 2 + (dz / 0.96) ** 2)
    lump = 0.0
    for amp, freq, ph in rng_phases:
        lump += amp * math.sin(dx * freq[0] + dy * freq[1] + dz * freq[2] + ph)
    return max(0.55, base * (1.0 + lump))


def curl_field(p, seed_offset=0.0):
    """Smooth divergence-free-ish 3D organic twist field for natural crooked twigs."""
    x, y, z = p[0] * 3.4 + seed_offset, p[1] * 3.4 - seed_offset * 0.7, p[2] * 3.4 + seed_offset * 1.3
    return np.array([
        math.sin(y * 1.3 + z * 0.9) * 0.6 + math.cos(z * 2.1 - x * 1.1) * 0.4,
        math.sin(z * 1.4 + x * 0.8) * 0.6 + math.cos(x * 2.2 - y * 1.2) * 0.4,
        math.sin(x * 1.2 + y * 1.1) * 0.6 + math.cos(y * 2.0 - z * 1.3) * 0.4,
    ])


class MeshBuilder:
    """Builds watertight manifold quad tubes (triangulated on export)."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []
        self.quad_count = 0

    def add_tube(self, pts, radii, sides, u_band, v_scale=2.2):
        """Append a closed, watertight tapered tube with `sides` (4 or 8) radial edges."""
        n = len(pts)
        if n < 2:
            return
        pts = [np.asarray(p, dtype=np.float64) for p in pts]
        radii = [float(r) for r in radii]

        # Compute tangents
        tangents = []
        for i in range(n):
            if i == 0:
                t = pts[1] - pts[0]
            elif i == n - 1:
                t = pts[-1] - pts[-2]
            else:
                t = pts[i + 1] - pts[i - 1]
            ln = float(np.linalg.norm(t))
            if ln < 1e-7:
                t = tangents[-1] if tangents else np.array([0.0, 1.0, 0.0])
            else:
                t = t / ln
            tangents.append(t)

        # Parallel-transport frames along tube
        us, vs = [], []
        u, v = ortho_basis(tangents[0])
        us.append(u)
        vs.append(v)
        for i in range(1, n):
            u, v = transport_frame(tangents[i - 1], us[-1], tangents[i])
            us.append(u)
            vs.append(v)

        # Cumulative arc length for V coordinate
        arc = [0.0]
        for i in range(1, n):
            arc.append(arc[-1] + float(np.linalg.norm(pts[i] - pts[i - 1])))

        # Create rings (no UV seam split needed if we stay within a horizontal band of U,
        # or wrap cleanly: here U encodes radial distance from core [0..1] for baked AO + straw gradient,
        # while V runs along stem arc length for longitudinal woody striations!)
        base_vtx = len(self.positions)
        ring_starts = []
        for i in range(n):
            ring_starts.append(len(self.positions))
            p = pts[i]
            r = radii[i]
            u_vec, v_vec = us[i], vs[i]
            # Radial distance from bush centre drives U (0.04 = dark core, 0.96 = sunlit bleached outer tips)
            rad = float(np.linalg.norm(p))
            u_coord = float(np.clip(0.05 + 0.88 * ((rad / 1.02) ** 1.15) + u_band, 0.02, 0.98))
            v_coord = arc[i] * v_scale
            for s in range(sides):
                ang = (2.0 * math.pi * s) / sides
                ca, sa = math.cos(ang), math.sin(ang)
                nrm = u_vec * ca + v_vec * sa
                pos = p + nrm * r
                self.positions.append(pos)
                self.normals.append(nrm)
                self.uvs.append((u_coord, v_coord))

        # Side quads (outward CCW when looking from outside along +nrm)
        # Note: cross(u_vec, v_vec) = tangent? Let's verify:
        # ortho_basis: v = cross(t, u), so (u, v, t) is right-handed: cross(u, v) = t!
        # Along ring angle increasing from u toward v: tangent around ring is (-sin*u + cos*v).
        # Going forward along tube is +t. cross(ring_dir, +t) = +nrm (outward)!
        # Wait: let's check for s=0 (nrm = u): ring_dir = +v, tube_dir = +t.
        # cross(v, t) = +u = +nrm! So triangle (a0, a1, b1) has edges (a1-a0 ~ +v) and (b1-a0 ~ +v + t),
        # cross(v, v+t) = cross(v, t) = +u = OUTWARD! Let's verify mathematically and assert in validator!
        for i in range(n - 1):
            r0 = ring_starts[i]
            r1 = ring_starts[i + 1]
            for s in range(sides):
                s_next = (s + 1) % sides
                a0 = r0 + s
                a1 = r0 + s_next
                b0 = r1 + s
                b1 = r1 + s_next
                self.indices.extend([a0, a1, b1, a0, b1, b0])
                self.quad_count += 1

        # Watertight end caps (all-quad topology: 1 quad for 4-sided, 4 quads for 8-sided via centre vertex)
        def add_cap(ring_idx, flip):
            r = ring_starts[ring_idx]
            if sides == 4:
                # Single quad using the 4 ring vertices directly (manifold, zero duplicate vertices!)
                if flip:
                    # Start cap facing -tangent: reverse ring order
                    self.indices.extend([r + 0, r + 3, r + 2, r + 0, r + 2, r + 1])
                else:
                    # End cap facing +tangent: forward ring order
                    self.indices.extend([r + 0, r + 1, r + 2, r + 0, r + 2, r + 3])
                self.quad_count += 1
            elif sides == 8:
                # 4-quad cap using a slightly domed centre vertex (keeps quad count clean & manifold)
                c_idx = len(self.positions)
                t_dir = -tangents[ring_idx] if flip else tangents[ring_idx]
                c_pos = pts[ring_idx] + t_dir * (radii[ring_idx] * 0.45)
                rad = float(np.linalg.norm(c_pos))
                u_coord = float(np.clip(0.05 + 0.88 * ((rad / 1.02) ** 1.15) + u_band, 0.02, 0.98))
                self.positions.append(c_pos)
                self.normals.append(t_dir)
                self.uvs.append((u_coord, arc[ring_idx] * v_scale))
                for q in range(4):
                    s0 = (q * 2) % 8
                    s1 = (q * 2 + 1) % 8
                    s2 = (q * 2 + 2) % 8
                    if flip:
                        self.indices.extend([c_idx, r + s2, r + s1, c_idx, r + s1, r + s0])
                    else:
                        self.indices.extend([c_idx, r + s0, r + s1, c_idx, r + s1, r + s2])
                    self.quad_count += 1

        add_cap(0, flip=True)
        add_cap(n - 1, flip=False)


def build_tumbleweed_geometry(seed=42):
    rng = np.random.default_rng(seed)
    builder = MeshBuilder()

    # Low-frequency lump phases for authentic organic bush shape (never a perfect sphere)
    lump_phases = [
        (0.075, (2.1, 1.7, -1.9), 0.8),
        (0.055, (-1.6, 2.5, 2.3), 2.1),
        (0.045, (3.1, -2.2, 1.4), 4.0),
    ]

    # Small broken woody crown slightly below centre (Y-up in glTF)
    crown = np.array([0.02, -0.14, -0.01])

    def grow_curve(start, init_dir, length, steps, r_start, r_end, curl_amp, inward_pull, kink_amp, u_band, sides):
        pts = [start.copy()]
        radii = [r_start]
        d = init_dir / max(float(np.linalg.norm(init_dir)), 1e-8)
        step_len = length / steps
        phase = float(rng.uniform(0.0, 20.0))

        for k in range(1, steps + 1):
            t = k / steps
            cur = pts[-1]
            rad = float(np.linalg.norm(cur))
            radial = cur / max(rad, 1e-6) if rad > 1e-4 else d
            shell = outer_shell_radius(radial, lump_phases)

            # Organic 3D curl + alternating botanical zig-zag kink
            c_force = curl_field(cur, phase) * curl_amp
            kink_dir = rng.normal(0.0, 1.0, 3)
            kink_dir -= d * float(np.dot(kink_dir, d))
            kn = float(np.linalg.norm(kink_dir))
            if kn > 1e-6:
                kink_dir /= kn
            kink = kink_dir * kink_amp * (0.4 + 0.6 * t)

            # Russian thistle stems arch outward, then curve tangentially and curl inward near the shell!
            shell_ratio = rad / shell
            pull = np.zeros(3)
            if shell_ratio > 0.68:
                # Blend away from pure outward radial toward tangential/inward basketwork
                outward = max(0.0, float(np.dot(d, radial)))
                strength = inward_pull * ((shell_ratio - 0.68) / 0.32) ** 1.35
                pull = -radial * (strength * (0.55 + outward))
            if shell_ratio > 1.02:
                # Hard soft-wall keep-inside so stray spikes don't distort bounding box
                pull -= radial * (shell_ratio - 1.02) * 2.8

            d = d + c_force + kink + pull
            d /= max(float(np.linalg.norm(d)), 1e-8)
            nxt = cur + d * step_len
            pts.append(nxt)
            # Smooth botanical taper (slightly thicker near base, finer at tip)
            r = r_start * ((1.0 - t) ** 0.78) + r_end * (t ** 0.78)
            radii.append(r)

        builder.add_tube(pts, radii, sides=sides, u_band=u_band)
        return pts, radii

    # Broken woody root base (slightly gnarled and off-centre, never a symmetrical hub)
    stub_pts = [
        crown + np.array([0.0, -0.065, 0.0]),
        crown + np.array([0.012, -0.02, -0.008]),
        crown + np.array([-0.006, 0.03, 0.010]),
        crown + np.array([-0.015, 0.075, 0.006]),
    ]
    builder.add_tube(stub_pts, [0.022, 0.025, 0.022, 0.015], sides=8, u_band=-0.06)

    # Order 1: 18 crooked woody stems emerging from staggered offsets along the crown & curving early
    # so there is no artificial wheel-hub symmetry.
    n_primary = 18
    golden = math.pi * (3.0 - math.sqrt(5.0))
    primary_branches = []

    for i in range(n_primary):
        y = -0.82 + 1.68 * ((i + 0.5) / n_primary)
        y = max(-0.88, min(0.92, y))
        r_xz = math.sqrt(max(0.0, 1.0 - y * y))
        theta = golden * i + rng.uniform(-0.28, 0.28)
        d0 = np.array([math.cos(theta) * r_xz, y, math.sin(theta) * r_xz])
        # Twist initial direction tangentially so stems spiral/curve out rather than spiking straight
        tang_init = np.cross(d0, np.array([0.0, 1.0, 0.0]) if abs(d0[1]) < 0.85 else np.array([1.0, 0.0, 0.0]))
        tang_init /= max(float(np.linalg.norm(tang_init)), 1e-8)
        d0 = d0 * 0.82 + tang_init * float(rng.uniform(-0.45, 0.45)) + rng.normal(0.0, 0.14, 3)
        d0 /= max(float(np.linalg.norm(d0)), 1e-8)

        start = crown + np.array([rng.uniform(-0.07, 0.07), rng.uniform(-0.05, 0.07), rng.uniform(-0.07, 0.07)])
        length = float(rng.uniform(0.95, 1.32))
        u_band = float(rng.uniform(-0.06, 0.06))
        pts, radii = grow_curve(
            start=start,
            init_dir=d0,
            length=length,
            steps=9,
            r_start=float(rng.uniform(0.014, 0.019)),
            r_end=0.0052,
            curl_amp=0.34,
            inward_pull=0.82,
            kink_amp=0.22,
            u_band=u_band,
            sides=8,
        )
        primary_branches.append((pts, radii))

    # Order 2: ~108 arching secondary branches weaving through mid and outer shell
    secondary_branches = []
    for pts, radii in primary_branches:
        for k in [1, 2, 3, 4, 6, 7]:
            if k >= len(pts) - 1:
                continue
            origin = pts[k]
            parent_dir = pts[k + 1] - pts[k - 1]
            parent_dir /= max(float(np.linalg.norm(parent_dir)), 1e-8)
            u_ax, v_ax = ortho_basis(parent_dir)

            az = float(rng.uniform(0.0, 2.0 * math.pi))
            fork_angle = math.radians(float(rng.uniform(36.0, 64.0)))
            side_vec = u_ax * math.cos(az) + v_ax * math.sin(az)
            d0 = parent_dir * math.cos(fork_angle) + side_vec * math.sin(fork_angle)
            rad = float(np.linalg.norm(origin))
            if rad > 0.28:
                radial = origin / rad
                tang = np.cross(radial, side_vec)
                tn = float(np.linalg.norm(tang))
                if tn > 1e-5:
                    d0 = d0 * 0.64 + (tang / tn) * 0.36
            d0 /= max(float(np.linalg.norm(d0)), 1e-8)

            length = float(rng.uniform(0.54, 0.86)) * (1.12 - 0.32 * (k / len(pts)))
            r0 = max(0.0060, radii[k] * float(rng.uniform(0.66, 0.82)))
            u_band = float(rng.uniform(-0.08, 0.08))
            s_pts, s_radii = grow_curve(
                start=origin,
                init_dir=d0,
                length=length,
                steps=6,
                r_start=r0,
                r_end=0.0036,
                curl_amp=0.38,
                inward_pull=1.02,
                kink_amp=0.26,
                u_band=u_band,
                sides=4,
            )
            secondary_branches.append((s_pts, s_radii))

    # Order 3: ~324 fine zig-zag tertiary twigs forming the dense lacy Russian thistle basketwork
    tertiary_branches = []
    for s_pts, s_radii in secondary_branches:
        for idx_idx, k in enumerate([1, 3, 5]):
            if k >= len(s_pts) - 1:
                continue
            origin = s_pts[k]
            parent_dir = s_pts[k + 1] - s_pts[k - 1]
            parent_dir /= max(float(np.linalg.norm(parent_dir)), 1e-8)
            u_ax, v_ax = ortho_basis(parent_dir)

            az = idx_idx * 2.15 + float(rng.uniform(-0.55, 0.55))
            fork_angle = math.radians(float(rng.uniform(38.0, 68.0)))
            d0 = parent_dir * math.cos(fork_angle) + (u_ax * math.cos(az) + v_ax * math.sin(az)) * math.sin(fork_angle)
            d0 /= max(float(np.linalg.norm(d0)), 1e-8)

            length = float(rng.uniform(0.26, 0.46))
            r0 = max(0.0038, s_radii[k] * float(rng.uniform(0.68, 0.84)))
            u_band = float(rng.uniform(-0.10, 0.10))
            t_pts, t_radii = grow_curve(
                start=origin,
                init_dir=d0,
                length=length,
                steps=4,
                r_start=r0,
                r_end=0.0024,
                curl_amp=0.44,
                inward_pull=1.22,
                kink_amp=0.36,
                u_band=u_band,
                sides=4,
            )
            tertiary_branches.append((t_pts, t_radii))

    # Order 4: Fine curling terminal forklets & tiny bristly dry-bract spurs along Order 3 twigs
    for t_pts, t_radii in tertiary_branches:
        for k in [1, 3]:
            if k >= len(t_pts):
                continue
            origin = t_pts[k]
            parent_dir = t_pts[min(k + 1, len(t_pts) - 1)] - t_pts[max(k - 1, 0)]
            parent_dir /= max(float(np.linalg.norm(parent_dir)), 1e-8)
            u_ax, v_ax = ortho_basis(parent_dir)

            az = float(rng.uniform(0.0, 2.0 * math.pi))
            fork = math.radians(float(rng.uniform(34.0, 68.0)))
            d0 = parent_dir * math.cos(fork) + (u_ax * math.cos(az) + v_ax * math.sin(az)) * math.sin(fork)
            d0 /= max(float(np.linalg.norm(d0)), 1e-8)

            seg_len = float(rng.uniform(0.09, 0.18))
            p1 = origin + d0 * (seg_len * 0.55)
            d1 = d0 + rng.normal(0.0, 0.42, 3)
            d1 /= max(float(np.linalg.norm(d1)), 1e-8)
            p2 = p1 + d1 * (seg_len * 0.45)
            r0 = max(0.0028, t_radii[k] * 0.85)
            builder.add_tube([origin, p1, p2], [r0, r0 * 0.70, 0.0018], sides=4, u_band=float(rng.uniform(-0.04, 0.12)))

    # Inner tangled weave: breaks up radial spokes so the interior reads as dense interwoven dry brush
    for i in range(52):
        d0 = rng.normal(0.0, 1.0, 3)
        d0 /= max(float(np.linalg.norm(d0)), 1e-8)
        start = crown + d0 * float(rng.uniform(0.10, 0.52))
        d_tan = np.cross(d0, rng.normal(0.0, 1.0, 3))
        d_tan /= max(float(np.linalg.norm(d_tan)), 1e-8)
        grow_curve(
            start=start,
            init_dir=d_tan,
            length=float(rng.uniform(0.36, 0.68)),
            steps=5,
            r_start=float(rng.uniform(0.0052, 0.0080)),
            r_end=0.0026,
            curl_amp=0.40,
            inward_pull=0.98,
            kink_amp=0.30,
            u_band=float(rng.uniform(-0.10, 0.06)),
            sides=4,
        )

    positions = np.asarray(builder.positions, dtype=np.float32)
    normals = normalize(np.asarray(builder.normals, dtype=np.float32)).astype(np.float32)
    uvs = np.asarray(builder.uvs, dtype=np.float32)
    indices = np.asarray(builder.indices, dtype=np.uint32)

    # Recentre tightly on bounding box and scale to radius 1.0
    lo = positions.min(axis=0)
    hi = positions.max(axis=0)
    centre = (lo + hi) * 0.5
    extent = float(np.max(hi - lo))
    positions = (positions - centre) * (2.0 / extent)

    return positions, normals, uvs, indices, builder.quad_count


def validate_topology(positions, normals, indices):
    """Verify clean manifold topology: watertight closed shells, outward normals, zero degenerates."""
    assert np.all(np.isfinite(positions)), "non-finite positions"
    assert np.all(np.isfinite(normals)), "non-finite normals"
    n_tris = len(indices) // 3
    tris = indices.reshape(n_tris, 3)

    p0 = positions[tris[:, 0]]
    p1 = positions[tris[:, 1]]
    p2 = positions[tris[:, 2]]
    face_n = np.cross(p1 - p0, p2 - p0)
    areas = 0.5 * np.linalg.norm(face_n, axis=1)
    degen = int(np.sum(areas < 1e-10))
    assert degen == 0, f"{degen} degenerate triangles"

    # Manifold half-edge check: every directed edge (a,b) must match exactly one opposite (b,a)
    half_edges = {}
    for ti, (a, b, c) in enumerate(tris):
        for u, v in ((int(a), int(b)), (int(b), int(c)), (int(c), int(a))):
            half_edges[(u, v)] = half_edges.get((u, v), 0) + 1
    non_manifold = sum(1 for cnt in half_edges.values() if cnt != 1)
    open_boundary = sum(1 for (u, v) in half_edges if (v, u) not in half_edges)
    assert non_manifold == 0, f"{non_manifold} non-manifold directed edges"
    assert open_boundary == 0, f"{open_boundary} open boundary edges"

    # Per-shell outward winding check via connected components & signed volume
    parent = list(range(len(positions)))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for a, b, c in tris:
        ra, rb, rc = find(int(a)), find(int(b)), find(int(c))
        parent[rb] = ra
        parent[rc] = ra
    shell_vol = {}
    signed_tets = np.einsum("ij,ij->i", p0, np.cross(p1, p2)) / 6.0
    for ti, (a, _, _) in enumerate(tris):
        r = find(int(a))
        shell_vol[r] = shell_vol.get(r, 0.0) + float(signed_tets[ti])
    inverted_shells = sum(1 for v in shell_vol.values() if v <= 0.0)
    assert inverted_shells == 0, f"{inverted_shells} of {len(shell_vol)} closed shells are inside-out"

    # Vertex normals must agree with area-weighted incident face normals
    accum = np.zeros_like(positions, dtype=np.float64)
    np.add.at(accum, tris[:, 0], face_n)
    np.add.at(accum, tris[:, 1], face_n)
    np.add.at(accum, tris[:, 2], face_n)
    dot_vn = np.sum(accum * normals, axis=1)
    bad_vn = int(np.sum(dot_vn <= 0))
    assert bad_vn == 0, f"{bad_vn} vertex normals oppose their incident faces"

    vol = float(sum(shell_vol.values()))
    print(
        f"topology OK: {len(positions)} verts, {n_tris} tris, {len(shell_vol)} closed shells, "
        f"0 degenerate, 0 flipped, 0 non-manifold, 0 open edges, signed vol={vol:.5f}"
    )


def build_textures(size=512):
    """Synthesize dry Russian thistle bark/straw albedo + tangent-space normal PNGs.

    U axis: core (0) -> sun-bleached outer tips (1), baking natural inner self-AO and hue shift.
    V axis: along each twig (seamless vertical tiling), carrying woody fibre striations and node bands.
    """
    rng = np.random.default_rng(12345)

    def tileable_noise(w, h, scale_x, scale_y, seed):
        r = np.random.default_rng(seed)
        gx = max(2, int(scale_x))
        gy = max(2, int(scale_y))
        grid = r.random((gy, gx))
        ys = (np.arange(h) / h) * gy
        xs = (np.arange(w) / w) * gx
        y0 = np.floor(ys).astype(int) % gy
        x0 = np.floor(xs).astype(int) % gx
        y1 = (y0 + 1) % gy
        x1 = (x0 + 1) % gx
        fy = ys - np.floor(ys)
        fx = xs - np.floor(xs)
        fy = fy * fy * (3 - 2 * fy)
        fx = fx * fx * (3 - 2 * fx)
        v00 = grid[np.ix_(y0, x0)]
        v10 = grid[np.ix_(y0, x1)]
        v01 = grid[np.ix_(y1, x0)]
        v11 = grid[np.ix_(y1, x1)]
        top = v00 * (1 - fx[None, :]) + v10 * fx[None, :]
        bot = v01 * (1 - fx[None, :]) + v11 * fx[None, :]
        return top * (1 - fy[:, None]) + bot * fy[:, None]

    # Fibrous woody striations stretched along V (high X frequency, low Y frequency)
    fibre = (
        0.50 * tileable_noise(size, size, 96, 12, 1)
        + 0.32 * tileable_noise(size, size, 192, 24, 2)
        + 0.18 * tileable_noise(size, size, 48, 6, 3)
    )
    # Stem node rings (subtle horizontal darkening along V)
    nodes = tileable_noise(size, size, 16, 48, 4)
    flecks = tileable_noise(size, size, 128, 128, 5)

    u = np.linspace(0.0, 1.0, size)[None, :]  # 0 = dark inner core, 1 = sunlit outer tips
    # Baked radial self-occlusion + colour gradient of real dried Russian thistle (sRGB):
    # Inner woody stems: warm shaded umber (#6b573e, stays rich brown in linear space, never black)
    # Mid basketwork: dry desert straw-tan (#b89c70)
    # Outer sunlit tips: warm bleached golden straw (#e0ca99)
    core_col = np.array([0.42, 0.34, 0.24])
    mid_col = np.array([0.72, 0.61, 0.44])
    tip_col = np.array([0.88, 0.79, 0.60])

    t1 = np.clip(u * 2.0, 0.0, 1.0)[..., None]
    t2 = np.clip((u - 0.5) * 2.0, 0.0, 1.0)[..., None]
    base = np.where(u[..., None] < 0.5, core_col * (1.0 - t1) + mid_col * t1, mid_col * (1.0 - t2) + tip_col * t2)

    # Subtle weathered sage-grey patches (characteristic of dried Salsola/Kali stems)
    sage_mask = np.clip((tileable_noise(size, size, 10, 10, 6) - 0.56) * 2.8, 0.0, 0.32)[..., None]
    sage_col = np.array([0.62, 0.60, 0.52])
    albedo = base * (1.0 - sage_mask) + sage_col * sage_mask

    # Apply longitudinal fibre light/dark modulation & subtle stem node darkening
    shade = 0.82 + 0.34 * fibre - 0.10 * np.clip((nodes - 0.65) * 3.0, 0.0, 1.0) + 0.06 * (flecks - 0.5)
    albedo = np.clip(albedo * shade[..., None], 0.0, 1.0)

    albedo_img = Image.fromarray(np.round(albedo * 255).astype(np.uint8))
    albedo_buf = io.BytesIO()
    albedo_img.save(albedo_buf, format="PNG", optimize=True)

    # Matching tangent-space normal map from fibre height (OpenGL +Y convention for glTF)
    height = fibre * 0.75 + flecks * 0.25
    gy = np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)
    gx = np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)
    strength = 2.6
    nx = -gx * strength
    ny = gy * strength
    nz = np.ones_like(nx)
    inv_l = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nrm_rgb = np.stack([nx * inv_l * 0.5 + 0.5, ny * inv_l * 0.5 + 0.5, nz * inv_l * 0.5 + 0.5], axis=-1)
    normal_img = Image.fromarray(np.round(np.clip(nrm_rgb, 0, 1) * 255).astype(np.uint8))
    normal_buf = io.BytesIO()
    normal_img.save(normal_buf, format="PNG", optimize=True)

    return albedo_buf.getvalue(), normal_buf.getvalue(), albedo


def write_glb(path, positions, normals, uvs, indices, albedo_png, normal_png):
    """Write a clean glTF 2.0 binary (.glb) compatible with EnvironmentGltfMath.h."""
    bin_chunks = []
    views = []

    def append_chunk(raw_bytes, target=None):
        # 4-byte align every bufferView per glTF 2.0 spec
        offset = sum(len(c) for c in bin_chunks)
        pad = (4 - (offset % 4)) % 4
        if pad:
            bin_chunks.append(b"\x00" * pad)
            offset += pad
        bin_chunks.append(raw_bytes)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(raw_bytes)}
        if target is not None:
            view["target"] = target
        idx = len(views)
        views.append(view)
        return idx

    pos_bytes = positions.astype("<f4").tobytes()
    nrm_bytes = normals.astype("<f4").tobytes()
    uv_bytes = uvs.astype("<f4").tobytes()
    idx_bytes = indices.astype("<u4").tobytes()

    bv_pos = append_chunk(pos_bytes, target=34962)
    bv_nrm = append_chunk(nrm_bytes, target=34962)
    bv_uv = append_chunk(uv_bytes, target=34962)
    bv_idx = append_chunk(idx_bytes, target=34963)
    bv_alb = append_chunk(albedo_png)
    bv_nrm_img = append_chunk(normal_png)

    bin_blob = b"".join(bin_chunks)
    bin_pad = (4 - (len(bin_blob) % 4)) % 4
    if bin_pad:
        bin_blob += b"\x00" * bin_pad

    gltf = {
        "asset": {"version": "2.0", "generator": "SOURCE:MVM build-tumbleweed.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Tumbleweed", "mesh": 0}],
        "meshes": [
            {
                "name": "RussianThistle",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                        "material": 0,
                    }
                ],
            }
        ],
        "materials": [
            {
                "name": "DryRussianThistle",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "roughnessFactor": 0.92,
                    "metallicFactor": 0.0,
                    "baseColorTexture": {"index": 0},
                },
                "normalTexture": {"index": 1},
            }
        ],
        "textures": [{"source": 0}, {"source": 1}],
        "images": [
            {"bufferView": bv_alb, "mimeType": "image/png"},
            {"bufferView": bv_nrm_img, "mimeType": "image/png"},
        ],
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": views,
        "accessors": [
            {
                "bufferView": bv_pos,
                "componentType": 5126,
                "count": len(positions),
                "type": "VEC3",
                "min": [float(v) for v in positions.min(axis=0)],
                "max": [float(v) for v in positions.max(axis=0)],
            },
            {
                "bufferView": bv_nrm,
                "componentType": 5126,
                "count": len(normals),
                "type": "VEC3",
            },
            {
                "bufferView": bv_uv,
                "componentType": 5126,
                "count": len(uvs),
                "type": "VEC2",
            },
            {
                "bufferView": bv_idx,
                "componentType": 5125,
                "count": len(indices),
                "type": "SCALAR",
            },
        ],
    }

    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_pad = (4 - (len(json_bytes) % 4)) % 4
    if json_pad:
        json_bytes += b" " * json_pad

    total_len = 12 + 8 + len(json_bytes) + 8 + len(bin_blob)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total_len))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_blob), 0x004E4942))
        f.write(bin_blob)
    print(f"wrote {path} ({total_len:,} bytes)")


def render_preview(path, positions, normals, uvs, indices, albedo_tex):
    """Fast numpy z-buffer 4-view preview sheet so the model can be visually judged."""
    res = 420
    tris = indices.reshape(-1, 3)
    h_tex, w_tex, _ = albedo_tex.shape

    def render_view(yaw_deg, pitch_deg, zoom=1.22, bg_sandy=True):
        ay, ap = math.radians(yaw_deg), math.radians(pitch_deg)
        cy, sy = math.cos(ay), math.sin(ay)
        cp, sp = math.cos(ap), math.sin(ap)
        ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], dtype=np.float32)
        rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], dtype=np.float32)
        rot = rx @ ry

        vp = positions @ rot.T
        vn = normals @ rot.T

        # Sample albedo per vertex from texture
        ui = np.clip((uvs[:, 0] * (w_tex - 1)).astype(int), 0, w_tex - 1)
        vi = (np.floor(uvs[:, 1] * h_tex).astype(int)) % h_tex
        vc = albedo_tex[vi, ui]

        # Directional warm desert sunlight + cool sky fill + back rim light
        sun_dir = np.array([0.55, 0.68, 0.48], dtype=np.float32)
        sun_dir /= np.linalg.norm(sun_dir)
        rim_dir = np.array([-0.6, 0.35, -0.7], dtype=np.float32)
        rim_dir /= np.linalg.norm(rim_dir)
        ndotl = np.clip(vn @ sun_dir, 0.0, 1.0)
        ndotrim = np.clip(vn @ rim_dir, 0.0, 1.0) ** 2
        hemi = 0.5 + 0.5 * vn[:, 1]
        # Extra subtle radial AO to preview depth
        radial_ao = np.clip(0.38 + 0.62 * np.linalg.norm(positions, axis=1), 0.35, 1.0)
        lit = (
            vc
            * (
                np.array([1.05, 0.94, 0.78]) * ndotl[:, None] * 0.95
                + np.array([0.48, 0.52, 0.60]) * hemi[:, None] * 0.42
                + np.array([0.95, 0.78, 0.52]) * ndotrim[:, None] * 0.25
                + 0.16
            )
            * radial_ao[:, None]
        )
        lit = np.clip(lit, 0.0, 1.0)

        # Perspective projection
        cam_z = 3.2
        z_cam = cam_z - vp[:, 2]
        scale = (res * 0.5) * zoom * 1.45 / np.maximum(z_cam, 0.1)
        sx = res * 0.5 + vp[:, 0] * scale
        sy = res * 0.5 - vp[:, 1] * scale

        # Background: warm desert horizon gradient + soft ground shadow ellipse
        yy, xx = np.mgrid[0:res, 0:res]
        if bg_sandy:
            t_bg = yy / float(res)
            img = np.zeros((res, res, 3), dtype=np.float32)
            sky = np.array([0.72, 0.80, 0.89]) * (1 - np.clip(t_bg * 1.8, 0, 1))[..., None] + np.array([0.88, 0.81, 0.69]) * np.clip(t_bg * 1.8, 0, 1)[..., None]
            sand = np.array([0.78, 0.66, 0.50]) * (1 - np.clip((t_bg - 0.55) * 2.2, 0, 1))[..., None] + np.array([0.66, 0.54, 0.39]) * np.clip((t_bg - 0.55) * 2.2, 0, 1)[..., None]
            img = np.where((t_bg < 0.56)[..., None], sky, sand)
            # Soft contact shadow under tumbleweed on sand
            sh_d = ((xx - res * 0.52) / (res * 0.30)) ** 2 + ((yy - res * 0.78) / (res * 0.08)) ** 2
            img *= (1.0 - 0.38 * np.exp(-sh_d * 1.8))[..., None]
        else:
            img = np.full((res, res, 3), 0.14, dtype=np.float32)

        zbuf = np.full((res, res), 1e9, dtype=np.float32)

        # Painter / per-triangle bounding-box rasterizer (fast for thin twigs because bboxes are tiny!)
        p0x, p0y, p0z = sx[tris[:, 0]], sy[tris[:, 0]], z_cam[tris[:, 0]]
        p1x, p1y, p1z = sx[tris[:, 1]], sy[tris[:, 1]], z_cam[tris[:, 1]]
        p2x, p2y, p2z = sx[tris[:, 2]], sy[tris[:, 2]], z_cam[tris[:, 2]]
        tri_c = (lit[tris[:, 0]] + lit[tris[:, 1]] + lit[tris[:, 2]]) / 3.0

        # Backface cull in screen space
        area2 = (p1x - p0x) * (p2y - p0y) - (p1y - p0y) * (p2x - p0x)
        # Note: screen Y is flipped (down), so CCW in view space has area2 < 0
        valid = np.where(area2 < -1e-4)[0]

        for ti in valid:
            x0, y0, z0 = float(p0x[ti]), float(p0y[ti]), float(p0z[ti])
            x1, y1, z1 = float(p1x[ti]), float(p1y[ti]), float(p1z[ti])
            x2, y2, z2 = float(p2x[ti]), float(p2y[ti]), float(p2z[ti])
            minx = max(0, int(math.floor(min(x0, x1, x2))))
            maxx = min(res - 1, int(math.ceil(max(x0, x1, x2))))
            miny = max(0, int(math.floor(min(y0, y1, y2))))
            maxy = min(res - 1, int(math.ceil(max(y0, y1, y2))))
            if minx > maxx or miny > maxy:
                continue
            inv_a = 1.0 / float(area2[ti])
            col = tri_c[ti]
            for py in range(miny, maxy + 1):
                pyf = py + 0.5
                for px in range(minx, maxx + 1):
                    pxf = px + 0.5
                    w0 = ((p1x[ti] - pxf) * (p2y[ti] - pyf) - (p1y[ti] - pyf) * (p2x[ti] - pxf)) * inv_a
                    if w0 < -0.05:
                        continue
                    w1 = ((p2x[ti] - pxf) * (p0y[ti] - pyf) - (p2y[ti] - pyf) * (p0x[ti] - pxf)) * inv_a
                    if w1 < -0.05:
                        continue
                    w2 = 1.0 - w0 - w1
                    if w2 < -0.05:
                        continue
                    z = w0 * z0 + w1 * z1 + w2 * z2
                    if z < zbuf[py, px]:
                        zbuf[py, px] = z
                        img[py, px] = col

        return np.round(np.clip(img, 0, 1) * 255).astype(np.uint8)

    v1 = render_view(20, 12, zoom=1.18, bg_sandy=True)
    v2 = render_view(115, 8, zoom=1.18, bg_sandy=True)
    v3 = render_view(220, 58, zoom=1.18, bg_sandy=False)
    v4 = render_view(45, 10, zoom=2.15, bg_sandy=False)  # close-up twig detail

    sheet = np.vstack([np.hstack([v1, v2]), np.hstack([v3, v4])])
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    Image.fromarray(sheet).save(path)
    print(f"wrote preview {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "..", "..", "Assets", "environment", "tumbleweed", "tumbleweed.glb"),
    )
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--preview", default="", help="optional 4-view shaded PNG path for visual inspection")
    args = parser.parse_args()

    positions, normals, uvs, indices, quads = build_tumbleweed_geometry(seed=args.seed)
    validate_topology(positions, normals, indices)
    albedo_png, normal_png, albedo_arr = build_textures(size=512)
    write_glb(args.out, positions, normals, uvs, indices, albedo_png, normal_png)
    if args.preview:
        render_preview(args.preview, positions, normals, uvs, indices, albedo_arr)


if __name__ == "__main__":
    main()
