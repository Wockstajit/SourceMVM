"""Is a camera position in open space, or inside a wall?

A preflight for automation that places the CS2 camera by coordinates (noclip and setpos). Run it
before every such move; a camera inside a wall still renders a picture (back faces are culled), so
the frame alone does not show the mistake.

X Y Z is the eye, where the camera will be. `setpos` places the player's feet, 64 units under the
eye (`getpos` reports the eye), so send the `setpos` line the result gives, not X Y Z itself.

It reads the map's collision mesh and render triangles from the map-data cache, marks every 16-unit
cell a surface passes through, and fills open space from the cells that lie under the open sky with
ground below them, through doorways and windows, never through a marked cell. A point passes when:

  - it is in filled space: reachable from the open sky without crossing a surface, over ground.
    The inside of a wall, a sealed room and the hollow inside of a building shell (Source 2
    buildings are often shells with a roof and no floor) are not;
  - it is not too close: no surface within --clearance units (default 16, a player's half width).

"Which side of a surface" and "how many surfaces a ray crosses" give wrong answers on real maps,
because map geometry is neither closed nor wound one way. Reachability depends on neither.

Options:
  --sees X Y Z     also report whether a straight line from the point to this target is free
                   (is the subject in view, or behind a wall).
  --nearest        when the point fails, name the closest point that passes.
  --stand          treat X Y Z as a place to stand: report the eye position 64 units above the
                   ground under it.

The filled grid is kept in Automation/runs/camera-clear/ by map digest, so only the first check of a
map takes long (about half a minute). Exit code 0 when the point passes, 1 when it does not, 2 when
the map has no cached map data yet (load the map once with the hook so it is extracted). Geometry
only: the capture still needs a look.

Usage: python Automation/verify/camera-clear.py <map> X Y Z [--sees X Y Z] [--nearest] [--stand]
                                                [--clearance 16] [--cache DIR]
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import mvm_cache  # noqa: E402

CELL = 16.0
# A triangle is cut until no edge is longer than this, then its corners are marked. Any disc of
# half a cell's radius on a surface then holds a marked point, which is what stops the fill from
# slipping between two cells on either side of a slanted wall.
EDGE = CELL * .49
GRID_VERSION = 1


def default_cache():
    return Path(os.environ.get("APPDATA", "")) / "HLAE" / "source_mvm" / "environment" / "occlusion"


def load(cache, name):
    """The map's triangles (collision and render) as three corner arrays, and the cache key."""
    manifest = cache / (name + ".json")
    if not manifest.exists():
        return None
    with open(manifest, encoding="utf-8-sig") as f:
        info = json.load(f)
    corners = []
    physics = cache / (str(info.get("physicsDigest")) + ".mvmocclusion")
    if physics.exists():
        raw = mvm_cache.read_payload(str(physics))
        magic, nv, ni = struct.unpack_from("<8sII", raw)
        if magic != b"MVMOCC01":
            raise ValueError("Invalid collision mesh: " + str(physics))
        v = np.frombuffer(raw, dtype="<f4", count=nv * 3, offset=40).reshape(-1, 3)
        i = np.frombuffer(raw, dtype="<u4", count=ni, offset=40 + nv * 12).reshape(-1, 3)
        corners.append((v[i[:, 0]], v[i[:, 1]], v[i[:, 2]]))
    visual = cache / (str(info.get("visualDigest")) + ".mvmcasters")
    if visual.exists():
        raw = mvm_cache.read_payload(str(visual))
        if raw[:8] != b"MVMCAST6":
            raise ValueError("Invalid visual caster cache: " + str(visual))
        _, _, nv, ni = struct.unpack_from("<iiii", raw, 8)
        v = np.frombuffer(raw, dtype="<f4", count=nv * 3, offset=24).reshape(-1, 3)
        i = np.frombuffer(raw, dtype="<u4", count=ni, offset=24 + nv * 12).reshape(-1, 3)
        corners.append((v[i[:, 0]], v[i[:, 1]], v[i[:, 2]]))
    if not corners:
        return None
    a, b, c = (np.concatenate([part[k] for part in corners]) for k in range(3))
    key = "%s-%s-%s-v%d" % (name, str(info.get("physicsDigest"))[:12], str(info.get("visualDigest"))[:12], GRID_VERSION)
    return {"a": a, "b": b, "c": c, "low": np.minimum(np.minimum(a, b), c), "high": np.maximum(np.maximum(a, b), c), "key": key}


def mark_surfaces(mesh, origin, shape):
    """True for every cell a triangle passes through."""
    solid = np.zeros(shape, dtype=bool)
    limit = np.array(shape) - 1

    def mark(points):
        cell = np.clip(np.floor((points - origin) / CELL).astype(np.int64), 0, limit)
        solid[cell[:, 0], cell[:, 1], cell[:, 2]] = True

    for start in range(0, len(mesh["a"]), 100000):
        a, b, c = (mesh[k][start:start + 100000].astype(np.float32) for k in "abc")
        while len(a):
            ab, bc, ca = (np.einsum("ij,ij->i", e, e) for e in (b - a, c - b, a - c))
            longest = np.maximum(np.maximum(ab, bc), ca)
            small = longest <= EDGE * EDGE
            for corner in (a, b, c):
                mark(corner[small])
            a, b, c, ab, bc, ca = (x[~small] for x in (a, b, c, ab, bc, ca))
            if not len(a):
                break
            # Cut each remaining triangle in two across the middle of its longest edge. The
            # corners are turned so that edge is always the first.
            turn = np.where((ab >= bc) & (ab >= ca), 0, np.where(bc >= ca, 1, 2))
            p = np.where((turn == 0)[:, None], a, np.where((turn == 1)[:, None], b, c))
            q = np.where((turn == 0)[:, None], b, np.where((turn == 1)[:, None], c, a))
            r = np.where((turn == 0)[:, None], c, np.where((turn == 1)[:, None], a, b))
            middle = (p + q) * .5
            a, b, c = np.concatenate([p, middle]), np.concatenate([middle, q]), np.concatenate([r, r])
    return solid


def fill(solid):
    """The open cells that can be reached from the open sky, over ground, without crossing a surface."""
    # Something under the cell: a bottomless shell and the void around and under the map have nothing.
    under = np.zeros_like(solid)
    under[:, :, 1:] = np.logical_or.accumulate(solid, axis=2)[:, :, :-1]
    over = np.zeros_like(solid)
    over[:, :, :-1] = np.logical_or.accumulate(solid[:, :, ::-1], axis=2)[:, :, ::-1][:, :, 1:]
    open_cell = ~solid & under
    # One closed layer round the grid, so a step never wraps from one side to the other.
    room = np.zeros(tuple(n + 2 for n in solid.shape), dtype=bool)
    room[1:-1, 1:-1, 1:-1] = open_cell
    seen = np.zeros_like(room)
    seen[1:-1, 1:-1, 1:-1] = open_cell & ~over
    flat_room, flat_seen = room.ravel(), seen.ravel()
    steps = (1, -1, room.shape[2], -room.shape[2], room.shape[1] * room.shape[2], -room.shape[1] * room.shape[2])
    front = np.flatnonzero(flat_seen)
    while len(front):
        reached = np.concatenate([front + step for step in steps])
        reached = reached[flat_room[reached] & ~flat_seen[reached]]
        front = np.unique(reached)
        flat_seen[front] = True
    return seen[1:-1, 1:-1, 1:-1], under


def grid(mesh, store):
    path = store / (mesh["key"] + ".npz")
    if path.exists():
        with np.load(path) as saved:
            return saved["origin"], saved["solid"], saved["reach"], saved["under"]
    origin = np.floor(mesh["low"].min(axis=0) / CELL) * CELL - CELL
    top = mesh["high"].max(axis=0) + [CELL, CELL, 256]
    shape = tuple(int(n) for n in np.ceil((top - origin) / CELL))
    solid = mark_surfaces(mesh, origin, shape)
    reach, under = fill(solid)
    store.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(path, origin=origin, solid=solid, reach=reach, under=under)
    return origin, solid, reach, under


def near(mesh, low, high):
    """The triangles whose bounds touch the box, as float64."""
    keep = np.all((mesh["high"] >= low) & (mesh["low"] <= high), axis=1)
    return tuple(mesh[k][keep].astype(np.float64) for k in "abc")


def blocked(mesh, start, end):
    """Distance along the segment to the first triangle, or None when the line is free."""
    span = end - start
    length = float(np.linalg.norm(span))
    if length < 1e-3:
        return None
    direction = span / length
    a, b, c = near(mesh, np.minimum(start, end) - 1, np.maximum(start, end) + 1)
    if not len(a):
        return None
    e1, e2 = b - a, c - a
    p = np.cross(direction, e2)
    det = np.einsum("ij,ij->i", e1, p)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    t0 = start - a
    u = np.einsum("ij,ij->i", t0, p) * inv
    q = np.cross(t0, e1)
    w = np.einsum("j,ij->i", direction, q) * inv
    t = np.einsum("ij,ij->i", e2, q) * inv
    hit = ok & (u >= -1e-6) & (w >= -1e-6) & (u + w <= 1.000001) & (t > 1e-4) & (t <= length)
    return float(t[hit].min()) if hit.any() else None


def nearest_surface(mesh, p, reach):
    """Smallest distance from p to any triangle within reach (Ericson, Real-Time Collision Detection, 5.1.5)."""
    a, b, c = near(mesh, p - reach, p + reach)
    if not len(a):
        return float("inf")
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = np.einsum("ij,ij->i", ab, ap), np.einsum("ij,ij->i", ac, ap)
    bp = p - b
    d3, d4 = np.einsum("ij,ij->i", ab, bp), np.einsum("ij,ij->i", ac, bp)
    cp = p - c
    d5, d6 = np.einsum("ij,ij->i", ab, cp), np.einsum("ij,ij->i", ac, cp)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    closest = np.empty_like(a)
    done = np.zeros(len(a), dtype=bool)

    def take(mask, points):
        mask = mask & ~done
        closest[mask] = points[mask]
        done[mask] = True

    take((d1 <= 0) & (d2 <= 0), a)
    take((d3 >= 0) & (d4 <= d3), b)
    take((d6 >= 0) & (d5 <= d6), c)
    with np.errstate(divide="ignore", invalid="ignore"):
        take((vc <= 0) & (d1 >= 0) & (d3 <= 0), a + ab * (d1 / (d1 - d3))[:, None])
        take((vb <= 0) & (d2 >= 0) & (d6 <= 0), a + ac * (d2 / (d2 - d6))[:, None])
        take((va <= 0) & (d4 - d3 >= 0) & (d5 - d6 >= 0), b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))[:, None])
        denom = va + vb + vc
        take(np.ones(len(a), dtype=bool), a + ab * (vb / denom)[:, None] + ac * (vc / denom)[:, None])
    return float(np.sqrt(((closest - p) ** 2).sum(axis=1)).min())


def cell_of(origin, shape, p):
    cell = np.floor((p - origin) / CELL).astype(np.int64)
    return cell if np.all(cell >= 0) and np.all(cell < shape) else None


def centre(origin, cell):
    return origin + (np.asarray(cell, dtype=np.float64) + .5) * CELL


def in_filled_space(mesh, origin, reach, eye):
    """The point's own cell is filled, or a filled cell beside it is joined to it by a free line."""
    cell = cell_of(origin, reach.shape, eye)
    if cell is None:
        return False
    if reach[tuple(cell)]:
        return True
    low, high = np.maximum(cell - 1, 0), np.minimum(cell + 2, reach.shape)
    for offset in np.argwhere(reach[low[0]:high[0], low[1]:high[1], low[2]:high[2]]):
        if blocked(mesh, eye, centre(origin, low + offset)) is None:
            return True
    return False


def check(mesh, cells, eye, clearance):
    origin, solid, reach, under = cells
    reasons = []
    cell = cell_of(origin, reach.shape, eye)
    if cell is None:
        reasons.append("outside the map")
    elif not in_filled_space(mesh, origin, reach, eye):
        if not under[tuple(cell)] and not solid[tuple(cell)]:
            reasons.append("nothing under it: inside a hollow building shell, or out in the void")
        else:
            reasons.append("not reachable from open space: inside a wall or a sealed room")
    nearest = nearest_surface(mesh, eye, max(clearance * 4, 64.0))
    if nearest < clearance:
        reasons.append("too close to a surface (%.1f units, need %.0f)" % (nearest, clearance))
    return {"clear": not reasons, "reasons": reasons, "nearestSurface": None if nearest == float("inf") else round(nearest, 1)}


def nearest_clear(mesh, cells, eye, clearance, reach_units=384.0):
    """The closest point that passes: the centre of a filled cell with no marked cell beside it."""
    origin, solid, reach, _ = cells
    cell = np.clip(np.floor((eye - origin) / CELL).astype(np.int64), 0, np.array(reach.shape) - 1)
    span = int(reach_units / CELL)
    low, high = np.maximum(cell - span, 0), np.minimum(cell + span + 1, reach.shape)
    found = np.argwhere(reach[low[0]:high[0], low[1]:high[1], low[2]:high[2]]) + low
    if not len(found):
        return None
    points = origin + (found + .5) * CELL
    for index in np.argsort(((points - eye) ** 2).sum(axis=1))[:400]:
        if check(mesh, cells, points[index], clearance)["clear"]:
            return [round(float(x), 1) for x in points[index]]
    return None


def stand(mesh, spot):
    """The eye of a player standing on the ground under the point (64 units above it). The ray starts
    8 units above the spot: blocked() skips a hit at the very start of a ray, so a spot given on the
    floor itself would otherwise miss that floor and fall through to whatever lies below."""
    top = spot + [0, 0, 8]
    drop = blocked(mesh, top, top - [0, 0, 2048])
    return None if drop is None else top - [0, 0, drop - 64]


def setpos_for(eye):
    """The command that puts the eye at `eye`: setpos takes the feet, 64 units lower."""
    return "setpos {:.1f} {:.1f} {:.1f}".format(eye[0], eye[1], eye[2] - 64)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("map")
    parser.add_argument("eye", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--sees", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--nearest", action="store_true")
    parser.add_argument("--stand", action="store_true")
    parser.add_argument("--clearance", type=float, default=16.0)
    parser.add_argument("--cache", type=Path, default=None)
    args = parser.parse_args()
    mesh = load(args.cache or default_cache(), args.map)
    if mesh is None:
        print(json.dumps({"clear": False, "reasons": ["no cached map data for " + args.map + "; load the map once with the hook"]}))
        return 2
    cells = grid(mesh, Path(__file__).resolve().parents[1] / "runs" / "camera-clear")
    eye = np.array(args.eye, dtype=np.float64)
    result = {}
    if args.stand:
        stood = stand(mesh, eye)
        if stood is None:
            print(json.dumps({"clear": False, "reasons": ["no ground under the point to stand on"]}))
            return 1
        eye = stood
        result["eye"] = [round(float(x), 1) for x in eye]
    result.update(check(mesh, cells, eye, args.clearance))
    result["setpos"] = setpos_for(eye)
    if args.sees is not None:
        target = np.array(args.sees, dtype=np.float64)
        wall = blocked(mesh, eye, target)
        result["sees"] = wall is None
        if wall is not None:
            result["sightBlockedAt"] = round(wall, 1)
        delta = target - eye
        result["aim"] = {"pitch": round(-float(np.degrees(np.arctan2(delta[2], np.linalg.norm(delta[:2])))), 1),
                         "yaw": round(float(np.degrees(np.arctan2(delta[1], delta[0]))), 1)}
    if args.nearest and not result["clear"]:
        result["nearestClear"] = nearest_clear(mesh, cells, eye, args.clearance)
        if result["nearestClear"] is not None:
            result["nearestSetpos"] = setpos_for(result["nearestClear"])
    print(json.dumps(result))
    return 0 if result["clear"] else 1


if __name__ == "__main__":
    sys.exit(main())
