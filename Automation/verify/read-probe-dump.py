"""Read the bounce probe dump that `mirv_relight dumpprobes` writes.

  python read-probe-dump.py <x> <y> <z> [nx ny nz]            probes around a world point and the receiver query there
  python read-probe-dump.py --voxels x0 x1 y0 y1 z [z ...]    the static voxel cells of a box, one slice per z

The dump is %APPDATA%\\HLAE\\debuglogs\\relight_probes.bin unless --file names another. The receiver
query is a CPU copy of WorldQuery in RelightIrradianceMath.h: it shows which probes of the cage a
surface point reads, each one's visibility and colour, and the irradiance that comes out. A dark
patch shows up here as probes whose band toward the surface is invalid, or whose visibility is zero.
"""
import argparse, os, struct
import numpy as np

BANDS = ["+x", "+y", "+z", "-x", "-y", "-z"]
ROWS = ("lowSize grid giLowSize giInfo sky ground staticOffset staticDims probeInfo shift voxelTarget voxelDims "
        "reflection visibleInfo reflectionHistory").split()


class Dump:
    def __init__(self, path):
        data = open(path, "rb").read()
        assert data[:8] == b"MVMPRB01", path
        o = 8
        self.rows = dict(zip(ROWS, np.frombuffer(data, np.float32, 60, o).reshape(15, 4))); o += 240
        static = np.frombuffer(data, np.float32, 4, o); o += 16
        self.static_low, self.cell = static[:3].astype(np.float64), float(static[3])
        volumes = []
        for _ in range(4):
            w, h, d, texel = struct.unpack_from("<4I", data, o); o += 16
            if texel == 8:
                volumes.append(np.frombuffer(data, np.float16, w * h * d * 4, o).reshape(d, h, w, 4).astype(np.float32))
            else:
                volumes.append(np.frombuffer(data, np.uint32, w * h * d, o).reshape(d, h, w))
            o += w * h * d * texel
        self.history, self.distance, self.composed, self.voxels = volumes
        self.probes = self.history.shape[1]
        self.low = self.rows["lowSize"][:3].astype(np.float64)
        self.size = float(self.rows["lowSize"][3])
        self.spacing = self.size / self.probes

    def voxel(self, world):
        c = np.floor((np.asarray(world) - self.static_low) / self.cell).astype(int)
        if np.any(c < 0) or c[0] >= self.voxels.shape[2] or c[1] >= self.voxels.shape[1] or c[2] >= self.voxels.shape[0]:
            return 0
        return int(self.voxels[c[2], c[1], c[0]])

    def band(self, volume, q, band):
        return volume[q[2] + band * self.probes, q[1], q[0]]

    def relocation(self, q):
        code = float(self.band(self.distance, q, 0)[3])
        axis, amount = int(np.floor(abs(code))), (abs(code) % 1) * (1 if code >= 0 else -1)
        offset = np.zeros(3)
        if 1 <= axis <= 3:
            offset[axis - 1] = amount
        return offset


def voxel_normal(word):
    """VoxelNormal in RelightProbeGiShaders.h: octahedral, 7 bits per axis."""
    e = np.array([word & 127, (word >> 7) & 127]) / 126 * 2 - 1
    n = np.array([e[0], e[1], 1 - abs(e[0]) - abs(e[1])])
    fold = max(-n[2], 0)
    n[0] += -fold if n[0] >= 0 else fold
    n[1] += -fold if n[1] >= 0 else fold
    return n / np.linalg.norm(n)


def describe(dump, q):
    p = dump.low + (np.array(q) + .5) * dump.spacing
    word = dump.voxel(p)
    state = "solid" if word & 0x80000000 else "open"
    print("probe %s at (%.0f %.0f %.0f) in a %s cell, origin moved by (%.2f %.2f %.2f) probes" % (
        tuple(int(v) for v in q), *p, state, *dump.relocation(q)))
    for b in range(6):
        h, d, c = dump.band(dump.history, q, b), dump.band(dump.distance, q, b), dump.band(dump.composed, q, b)
        if h[3] < -1.5:
            print("   %s invalid (its rays see the back of surfaces, or it has no open origin) | composed (%.3f %.3f %.3f)" % (BANDS[b], *c[:3]))
        else:
            print("   %s bounce (%.3f %.3f %.3f) sky %.3f | mean hit %.2f probes | composed (%.3f %.3f %.3f)" % (
                BANDS[b], *h[:3], h[3], d[0], *c[:3]))


def query(dump, point, normal, show=10):
    n = normal / np.linalg.norm(normal)
    resolution = dump.probes
    uv = (point - dump.low) / dump.size + n * (.35 / resolution)
    g = uv * resolution
    base, fraction = np.floor(g).astype(int), g - np.floor(g) - .5
    total_weight, total_kernel, total = 0., 0., np.zeros(3)
    lines = []
    for i in range(27):
        offset = np.array([i % 3 - 1, (i // 3) % 3 - 1, i // 9 - 1])
        q = base + offset
        if np.any(q < 0) or np.any(q >= resolution):
            continue
        delta = (uv - (q + .5) / resolution) * resolution - dump.relocation(q)
        squared = float(delta @ delta)
        distance = np.sqrt(max(squared, .0001))
        angular = delta * delta / squared if squared > 1e-8 else np.ones(3) / 3
        visibility, valid = 0., 0.
        for axis in range(3):
            moments = dump.band(dump.distance, q, axis if delta[axis] >= 0 else axis + 3)
            valid = max(valid, float(moments[2]))
            variance = max(float(moments[1] - moments[0] * moments[0]), .0001)
            beyond = max(distance - float(moments[0]), 0)
            chebyshev = variance / (variance + beyond * beyond)
            visibility += chebyshev ** 3 * float(moments[2]) * angular[axis]
        facing = max(.0001, (1 - float(delta @ n) / distance) * .5)
        kernel = 1.
        for axis in range(3):
            f = fraction[axis]
            kernel *= (.75 - f * f) if offset[axis] == 0 else .5 * (f * offset[axis] + .5) ** 2
        weight = visibility * (facing * facing + .2) * kernel
        colour = np.zeros(3)
        for axis in range(3):
            colour += dump.band(dump.composed, q, axis if n[axis] >= 0 else axis + 3)[:3] * n[axis] * n[axis]
        total += colour * weight
        total_weight += weight
        total_kernel += kernel * valid
        lines.append((kernel, "   probe %s kernel %.3f visibility %.3f valid %.0f colour (%.3f %.3f %.3f)" % (
            tuple(int(v) for v in q), kernel, visibility, valid, *colour)))
    confidence = min(1, total_weight / max(total_kernel, .0001) * 10000)
    result = total / max(total_weight, .0001) * confidence
    for _, line in sorted(lines, reverse=True)[:show]:
        print(line)
    print("   weight %.4f over a valid kernel of %.3f -> irradiance (%.3f %.3f %.3f)" % (total_weight, total_kernel, *result))
    return result


def voxel_slices(dump, x0, x1, y0, y1, zs):
    print("each cell: dominant axis of the normal it kept, T = two-sided (faces on opposite sides), p = porous, c = cloth, . = empty")
    for z in zs:
        print("z=%g, rows y from high to low, columns x from %g to %g" % (z, x0, x1))
        for y in np.arange(y1 - dump.cell / 2, y0, -dump.cell):
            row = "  y%6.0f " % y
            for x in np.arange(x0 + dump.cell / 2, x1, dump.cell):
                word = dump.voxel((x, y, z))
                if not word & 0x80000000:
                    row += "  .  "
                    continue
                n = voxel_normal(word)
                axis = int(np.argmax(np.abs(n)))
                # Porous and two-sided together mark cloth (VOXEL_CLOTH in RelightProbeGiShaders.h).
                cloth = word & 0x40004000 == 0x40004000
                row += " %s%s%s%s" % ("+" if n[axis] > 0 else "-", "xyz"[axis], "T" if word & 0x4000 and not cloth else " ",
                                      "c" if cloth else "p" if word & 0x40000000 else " ")
            print(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", default=os.path.expandvars(r"%APPDATA%\HLAE\debuglogs\relight_probes.bin"))
    parser.add_argument("--voxels", action="store_true", help="print voxel slices: x0 x1 y0 y1 z [z ...]")
    parser.add_argument("values", nargs="+", type=float)
    args = parser.parse_args()
    dump = Dump(args.file)
    print("region low (%.0f %.0f %.0f) size %.0f, probes every %.0f units, voxels of %.0f units, static grid %dx%dx%d" % (
        *dump.low, dump.size, dump.spacing, dump.cell, *dump.voxels.shape[::-1]))
    if args.voxels:
        if len(args.values) < 5:
            parser.error("--voxels takes x0 x1 y0 y1 and at least one z")
        voxel_slices(dump, *args.values[:4], args.values[4:])
        return
    if len(args.values) not in (3, 6):
        parser.error("give a world point, and optionally the surface normal")
    point = np.array(args.values[:3])
    normal = np.array(args.values[3:6]) if len(args.values) == 6 else np.array([0., 0., 1.])
    nearest = np.floor((point - dump.low) / dump.spacing).astype(int)
    for dz in (1, 0, -1):
        describe(dump, nearest + np.array([0, 0, dz]))
    print("receiver at (%.0f %.0f %.0f), normal (%.2f %.2f %.2f):" % (*point, *normal))
    query(dump, point, normal)


if __name__ == "__main__":
    main()
