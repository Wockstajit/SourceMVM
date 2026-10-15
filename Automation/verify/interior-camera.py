"""Check audit cameras against the map's native collision mesh.

This is a geometric preflight, not visual approval. Captures still need inspection.
Usage: python interior-camera.py <map> --eye X Y Z
       python interior-camera.py <map> --plan <output.json>
"""
import argparse
import json
import os
from pathlib import Path
import struct

import numpy as np

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import mvm_cache  # noqa: E402


class Mesh:
    def __init__(self, path, focus=None):
        raw = mvm_cache.read_payload(path)
        magic, nv, ni = struct.unpack_from("<8sII", raw)
        if magic != b"MVMOCC01" or ni % 3 or len(raw) != 40 + nv * 12 + ni * 4:
            raise ValueError("Invalid native collision mesh")
        self.bounds = np.array(struct.unpack_from("<6f", raw, 16)).reshape(2, 3)
        v = np.frombuffer(raw, dtype="<f4", count=nv * 3, offset=40).reshape(-1, 3)
        i = np.frombuffer(raw, dtype="<u4", count=ni, offset=40 + nv * 12).reshape(-1, 3)
        self.low = np.minimum(np.minimum(v[i[:,0]],v[i[:,1]]),v[i[:,2]])
        self.high = np.maximum(np.maximum(v[i[:,0]],v[i[:,1]]),v[i[:,2]])
        if focus is not None:
            # Checking one camera only casts rays within this support envelope.
            # Keep intersecting AABBs, including triangles whose vertices lie
            # outside it; building global bins for a large map wastes memory.
            eye = np.asarray(focus, dtype=float)
            selected = np.all((self.high >= eye + [-528, -528, -256]) &
                              (self.low <= eye + [528, 528, 384]), axis=1)
            self.low, self.high, i = self.low[selected], self.high[selected], i[selected]
        self.a = v[i[:, 0]].astype(np.float64)
        self.e1 = v[i[:, 1]] - self.a
        self.e2 = v[i[:, 2]] - self.a
        if not len(i):
            self.large = np.array([], dtype=np.int64)
            self.cell_low, self.cell_width, self.bins = np.zeros(2, dtype=np.int64), 1, {}
            self.normal = np.empty((0, 3))
            return
        # XY bins accelerate the many vertical floor/ceiling rays without a
        # dependency on an external geometry engine. Large spanning triangles
        # remain in a separate list and participate in every query.
        cell_low = np.floor(self.low[:, :2] / 256).astype(np.int64)
        cell_high = np.floor(self.high[:, :2] / 256).astype(np.int64)
        extent = cell_high - cell_low + 1
        large = extent[:, 0] * extent[:, 1] > 64
        self.large = np.flatnonzero(large)
        self.cell_low = cell_low.min(axis=0)
        self.cell_width = int((cell_high.max(axis=0) - self.cell_low)[1] + 1)
        keys, indices = [], []
        small = np.flatnonzero(~large)
        if len(small):
            for x in range(int(extent[small, 0].max())):
                for y in range(int(extent[small, 1].max())):
                    selected = small[(extent[small, 0] > x) & (extent[small, 1] > y)]
                    if len(selected):
                        cell = cell_low[selected] + [x, y] - self.cell_low
                        keys.append(cell[:, 0] * self.cell_width + cell[:, 1])
                        indices.append(selected)
        self.bins = {}
        if keys:
            keys, indices = np.concatenate(keys), np.concatenate(indices)
            order = np.argsort(keys)
            keys, indices = keys[order], indices[order]
            unique, starts, counts = np.unique(keys, return_index=True, return_counts=True)
            self.bins = {int(key): indices[start:start + count] for key, start, count in zip(unique, starts, counts)}
        self.normal = np.cross(self.e1, self.e2)
        self.normal /= np.maximum(np.linalg.norm(self.normal, axis=1)[:, None], 1e-12)

    def ray(self, origin, direction, limit=1024):
        direction = np.asarray(direction, dtype=float)
        origin = np.asarray(origin, dtype=float)
        end = origin + direction * limit
        low, high = np.minimum(origin, end) - 1e-5, np.maximum(origin, end) + 1e-5
        cells_low = np.floor(low[:2] / 256).astype(int) - self.cell_low
        cells_high = np.floor(high[:2] / 256).astype(int) - self.cell_low
        groups = [self.large]
        for x in range(cells_low[0], cells_high[0] + 1):
            for y in range(cells_low[1], cells_high[1] + 1):
                group = self.bins.get(int(x * self.cell_width + y))
                if group is not None:
                    groups.append(group)
        candidates = np.unique(np.concatenate(groups))
        candidates = candidates[np.all((self.high[candidates] >= low) & (self.low[candidates] <= high), axis=1)]
        if not len(candidates):
            return None
        a, e1, e2 = self.a[candidates], self.e1[candidates], self.e2[candidates]
        h = np.cross(direction, e2)
        determinant = np.einsum("ij,ij->i", e1, h)
        good = np.abs(determinant) > 1e-8
        inverse = np.divide(1, determinant, out=np.zeros_like(determinant), where=good)
        s = origin - a
        u = inverse * np.einsum("ij,ij->i", s, h)
        q = np.cross(s, e1)
        v = inverse * np.einsum("ij,j->i", q, direction)
        t = inverse * np.einsum("ij,ij->i", e2, q)
        hit = good & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1.000001) & (t > .01) & (t <= limit)
        if not hit.any():
            return None
        idx = np.argmin(np.where(hit, t, np.inf))
        return float(t[idx]), self.normal[candidates[idx]]

    def check(self, eye):
        eye = np.asarray(eye, dtype=float)
        errors = []
        if np.any(eye <= self.bounds[0]) or np.any(eye >= self.bounds[1]):
            errors.append("outside collision bounds")
        floor = self.ray(eye, [0, 0, -1], 256)
        ceiling = self.ray(eye, [0, 0, 1], 384)
        if floor is None or floor[1][2] < .7 or not 64 <= floor[0] <= 80:
            errors.append("no walkable floor beneath the full player height")
        if ceiling is None or ceiling[1][2] > -.5 or ceiling[0] < 20:
            errors.append("no interior ceiling with head clearance")
        # Keep the whole player footprint on the floor and under the roof. A
        # centre ray alone accepts a camera straddling a ledge or an entrance.
        for x, y in ((-16,-16),(-16,16),(16,-16),(16,16)):
            corner = eye + [x, y, 0]
            support = self.ray(corner, [0, 0, -1], 256)
            roof = self.ray(corner, [0, 0, 1], 384)
            if (support is None or support[1][2] < .7 or not 64 <= support[0] <= 80 or
                    roof is None or roof[1][2] > -.5 or roof[0] < 20):
                errors.append("player footprint crosses a floor or ceiling boundary")
                break
        enclosed = 0
        for angle in np.arange(0, 360, 45):
            rad = np.deg2rad(angle)
            direction = np.array([np.cos(rad), np.sin(rad), 0])
            wall = self.ray(eye, direction, 512)
            if wall is not None and np.dot(wall[1], direction) < -.2:
                enclosed += 1
        if enclosed < 6:
            errors.append("not enclosed by building walls; possible exterior overhang")
        clearance = 1024.
        for height in (0, -48):
            for angle in np.arange(0, 360, 45):
                rad = np.deg2rad(angle)
                hit = self.ray(eye + [0, 0, height], [np.cos(rad), np.sin(rad), 0], 24)
                if hit is not None:
                    clearance = min(clearance, hit[0])
        if clearance < 20:
            errors.append("player or camera too close to a wall")
        return {"geometryValid": not errors, "errors": errors,
                "floorDistance": None if floor is None else round(floor[0], 3),
                "ceilingDistance": None if ceiling is None else round(ceiling[0], 3),
                "wallClearance": round(clearance, 3), "enclosingDirections": enclosed, "visualReview": "pending"}


def plan(mesh, lights, limit=0):
    routes = []
    candidates = [light for light in lights if light["kind"] != "light_environment"]
    for light in candidates[:limit or None]:
        if light["kind"] == "light_environment":
            continue
        target = np.array(light["origin"], dtype=float)
        found = None
        for radius in (96, 160, 48, 240):
            for angle in range(0, 360, 45):
                rad = np.deg2rad(angle)
                p = target + [radius * np.cos(rad), radius * np.sin(rad), -24]
                floor = mesh.ray(p, [0, 0, -1], 512)
                if floor is None or floor[1][2] < .7:
                    continue
                eye = p + [0, 0, 66 - floor[0]]
                validation = mesh.check(eye)
                if validation["geometryValid"]:
                    # Prefer room depth rather than a doorway or exterior awning.
                    # Covered outdoor passages still require visual rejection.
                    deep = True
                    for direction_angle in range(0, 360, 45):
                        direction_rad = np.deg2rad(direction_angle)
                        sample = eye + [64*np.cos(direction_rad), 64*np.sin(direction_rad), 0]
                        roof = mesh.ray(sample, [0,0,1], 384)
                        support = mesh.ray(sample, [0,0,-1], 256)
                        if (roof is None or roof[1][2] > -.5 or support is None or
                                support[1][2] < .7 or not 64 <= support[0] <= 80):
                            deep = False
                            break
                    if not deep:
                        continue
                    delta = target - eye
                    angles = [-float(np.rad2deg(np.arctan2(delta[2], np.linalg.norm(delta[:2])))),
                              float(np.rad2deg(np.arctan2(delta[1], delta[0])))]
                    views = []
                    for yaw in range(0, 360, 45):
                        direction = np.array([np.cos(np.deg2rad(yaw)), np.sin(np.deg2rad(yaw)), 0])
                        wall = mesh.ray(eye, direction, 512)
                        if wall is None or wall[0] < 64 or np.dot(wall[1], direction) >= -.2:
                            continue
                        inside = True
                        for reach in (48, min(128, wall[0] * .75)):
                            sample = eye + direction * reach
                            roof = mesh.ray(sample, [0,0,1], 384)
                            floor_sample = mesh.ray(sample, [0,0,-1], 256)
                            if (roof is None or roof[1][2] > -.5 or floor_sample is None or
                                    floor_sample[1][2] < .7 or not 64 <= floor_sample[0] <= 80):
                                inside = False
                                break
                        if inside:
                            views.append((wall[0], yaw))
                    if not views:
                        continue
                    found = {"camera": eye.tolist(), "angles": angles, "viewYaw": max(views)[1], "validation": validation}
                    break
            if found:
                break
        routes.append({"identity": light["identity"], "position": target.tolist(),
                       "kind": light["kind"], "pose": found})
    return routes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--eye", nargs=3, type=float)
    group.add_argument("--plan", type=Path)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    cache = Path(os.environ["APPDATA"]) / "HLAE/source_mvm/environment/occlusion"
    manifest = json.loads((cache / f"{args.map}.json").read_text(encoding="utf-8-sig"))
    mesh = Mesh(cache / (manifest["physicsDigest"] + ".mvmocclusion"), focus=args.eye)
    if args.eye:
        result = mesh.check(args.eye)
        print(json.dumps(result))
        return 0 if result["geometryValid"] else 2
    lights = json.loads((cache / (manifest["lightsDigest"] + ".lights.json")).read_text(encoding="utf-8-sig"))["lights"]
    result = {"map": args.map, "physicsDigest": manifest["physicsDigest"], "poses": plan(mesh, lights, args.limit)}
    args.plan.parent.mkdir(parents=True, exist_ok=True)
    args.plan.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{sum(p['pose'] is not None for p in result['poses'])}/{len(result['poses'])} interior poses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
