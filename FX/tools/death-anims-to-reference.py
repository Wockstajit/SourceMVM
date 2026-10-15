#!/usr/bin/env python3
"""Put the ORIGINAL animation behind every clip in the death-animation packs into browsable folders.

    py -3 FX/tools/death-anims-to-reference.py [--out reference/death-animations]

Reads the baked library (so clips deleted or retagged in the Blender viewer are reflected) and
writes <out>/<Source>/<Stance>/<Body area>/<clip name>.<ext>, the same grouping the viewer uses:

- Classic CoD: the Greyhound SEAnim export from FX/sources/cod-deaths/anims (copied).
- MW2019: the Cast export from FX/sources/mw2019-deaths/anims (copied).
- Extra (GMod): every clip is a sequence inside one studiomodel (FX/sources/brutal-deaths/
  model_anim.mdl), so each one is written out as a Source .smd animation on the ValveBiped skeleton.

<out>/index.csv lists every clip with its tags and where its original lives. The output folder is
replaced on every run; the default lives under reference/, which is not tracked.
"""

import argparse
import csv
import importlib.util
import math
import os
import shutil
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIBRARY = os.path.join(ROOT, 'FX', 'death-animations', 'ragdoll_animations.mvmanim')

spec = importlib.util.spec_from_file_location('extract_death_anims',
                                              os.path.join(ROOT, 'FX', 'tools', 'extract-death-anims.py'))
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)


def read_library():
    """[(name, source, category, side, stance, weapon, frames)] for every clip in the library."""
    clips = []
    data = open(LIBRARY, 'rb').read()
    version, count, bodies, _, _ = struct.unpack_from('<IIIff', data, 8)
    if version != bake.FORMAT_VERSION:
        raise SystemExit('%s is format %d; rebuild it with extract-death-anims.py' % (LIBRARY, version))
    entry = 60 + 12 * bodies
    for i in range(count):
        e = data[28 + i * entry:28 + (i + 1) * entry]
        name = e[:32].split(b'\0')[0].decode()
        source, category, frames, _, side, stance, weapon = struct.unpack_from('<IIIIIII', e, 32)
        clips.append((name, source, bake.CATEGORY_IDS[category], bake.HIT_SIDES[side],
                      bake.STANCES[stance], bake.WEAPONS[weapon], frames))
    return clips


def quat_to_source_euler(q):
    """Inverse of AngleQuaternion: xyzw -> RadianEuler (x roll, y pitch, z yaw), R = Rz * Ry * Rx."""
    x, y, z, w = q
    m00 = 1 - 2 * (y * y + z * z)
    m10 = 2 * (x * y + w * z)
    m20 = 2 * (x * z - w * y)
    m21 = 2 * (y * z + w * x)
    m22 = 1 - 2 * (x * x + y * y)
    pitch = math.asin(max(-1.0, min(1.0, -m20)))
    return math.atan2(m21, m22), pitch, math.atan2(m10, m00)


def write_smd(mdl, anim, path):
    desc = mdl.animdesc(anim)
    with open(path, 'w', encoding='ascii', newline='\n') as out:
        out.write('version 1\nnodes\n')
        for i, bone in enumerate(mdl.bones):
            out.write('%d "%s" %d\n' % (i, bone['name'], bone['parent']))
        out.write('end\nskeleton\n')
        for frame in range(desc['frames']):
            out.write('time %d\n' % frame)
            pose = mdl.local_pose(anim, frame)
            for i in range(len(mdl.bones)):
                p, q = pose[i]
                r = quat_to_source_euler(q)
                out.write('%d %.6f %.6f %.6f %.6f %.6f %.6f\n' % (i, p[0], p[1], p[2], r[0], r[1], r[2]))
        out.write('end\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', default=os.path.join(ROOT, 'reference', 'death-animations'))
    args = parser.parse_args()

    # The Euler conversion must round-trip the builder's own AngleQuaternion.
    for angles in ((0.3, -0.7, 2.1), (-1.2, 0.4, -2.9), (0.0, 1.2, 0.5)):
        back = bake.euler_to_quat(*quat_to_source_euler(bake.euler_to_quat(*angles)))
        want = bake.euler_to_quat(*angles)
        if abs(abs(sum(a * b for a, b in zip(back, want))) - 1) > 1e-9:
            raise SystemExit('Euler conversion does not round-trip')

    cod_files = {bake.cod_clip_name(stem): os.path.join(ROOT, 'FX', 'sources', 'cod-deaths', 'anims', stem + '.seanim')
                 for stems in bake.COD_CLIP_SETS.values() for stem in stems}
    mw_dir = os.path.join(ROOT, 'FX', 'sources', 'mw2019-deaths', 'anims')
    mw_files = {bake.mw2019_clip_name(f[:-5]): os.path.join(mw_dir, f) for f in os.listdir(mw_dir) if f.endswith('.cast')}
    mdl_path = os.path.join(ROOT, 'FX', 'sources', 'brutal-deaths', 'model_anim.mdl')
    mdl = bake.StudioModel(mdl_path)

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    rows, missing = [], []
    for name, source_id, category, side, stance, weapon, frames in read_library():
        source = ['MW2019', 'Classic CoD', 'Extra (GMod)'][source_id]
        area = 'manual (never picked)' if category == 'manual' else category
        folder = os.path.join(args.out, source, stance.capitalize(), area)
        os.makedirs(folder, exist_ok=True)
        if source == 'Extra (GMod)':
            anim = mdl.sequences.get(name)
            if anim is None:
                missing.append(name); continue
            target = os.path.join(folder, name + '.smd')
            write_smd(mdl, anim, target)
            original = '%s (sequence "%s")' % (os.path.relpath(mdl_path, ROOT), name)
        else:
            src = (cod_files if source == 'Classic CoD' else mw_files).get(name)
            if not src or not os.path.exists(src):
                missing.append(name); continue
            target = os.path.join(folder, name + os.path.splitext(src)[1])
            shutil.copy2(src, target)
            original = os.path.relpath(src, ROOT)
        rows.append({'clip': name, 'source': source, 'stance': stance, 'body area': category,
                     'hit from': side, 'weapon': weapon, 'frames': frames, 'seconds': round((frames - 1) / 30.0, 2),
                     'file': os.path.relpath(target, args.out).replace('\\', '/'), 'original': original.replace('\\', '/')})
    with open(os.path.join(args.out, 'index.csv'), 'w', encoding='utf-8', newline='') as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(sorted(rows, key=lambda r: (r['source'], r['stance'], r['body area'], r['clip'])))
    print('wrote %d clips to %s (%d without an original: %s)' % (len(rows), args.out, len(missing), ', '.join(missing) or '-'))


if __name__ == '__main__':
    main()
