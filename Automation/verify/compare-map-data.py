"""Compare two extracted map-data caches (reference vs candidate) with fixed tolerances.

    python Automation/verify/compare-map-data.py <reference-dir> <candidate-dir> <map> [<map> ...]

Each dir holds <map>.json (format 4 manifest) plus digest-named payloads: .lights.json (compact
format 1), .mvmocclusion (MVMOCC01) and .mvmcasters (MVMCAST6), both in the compressed container (Automation/lib/mvm_cache.py), and any skybox caster payloads.
Use it to check a change to the native map extractor against a previous extraction of the same
game files. Every criterion and tolerance is printed; exits 1 if any check fails.
"""
import json
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'lib'))
import mvm_cache  # noqa: E402

FAILS = []


def check(ok, label, detail):
    print(('  PASS ' if ok else '  FAIL ') + label + ': ' + detail)
    if not ok:
        FAILS.append(label)


def rel(a, b):
    if a == b:
        return 0.0
    return abs(a - b) / max(abs(a), abs(b), 1e-12)


# ------------------------------------------------------------------ geometry stats

def tri_stats(pos, idx):
    """Triangle count, total area, area per dominant-normal bucket (+x,-x,+y,-y,+z,-z), bounds."""
    if idx.size == 0:
        return dict(count=0, area=0.0, buckets=np.zeros(6), area_per_tri=np.zeros(0),
                    lo=np.zeros(3), hi=np.zeros(3))
    t = idx.reshape(-1, 3).astype(np.int64)
    a, b, c = (pos[t[:, k]].astype(np.float64) for k in range(3))
    n = np.cross(b - a, c - a)
    area = 0.5 * np.linalg.norm(n, axis=1)
    axis = np.argmax(np.abs(n), axis=1)
    negative = np.take_along_axis(n, axis[:, None], 1)[:, 0] < 0
    buckets = np.bincount(axis * 2 + negative, weights=area, minlength=6)
    used = np.unique(t)
    return dict(count=len(t), area=float(area.sum()), buckets=buckets, area_per_tri=area,
                lo=pos[used].min(0), hi=pos[used].max(0))


def compare_geometry(label, ref, cand, count_tol, area_tol, bucket_tol, bounds_tol):
    check(rel(ref['count'], cand['count']) <= count_tol, label + ' triangle count',
          '%d vs %d (tol %.2f%%)' % (ref['count'], cand['count'], count_tol * 100))
    check(rel(ref['area'], cand['area']) <= area_tol, label + ' total area',
          '%.1f vs %.1f (tol %.2f%%)' % (ref['area'], cand['area'], area_tol * 100))
    worst = max(abs(ref['buckets'][i] - cand['buckets'][i]) / max(ref['area'], 1e-12) for i in range(6))
    check(worst <= bucket_tol, label + ' area by normal direction',
          'worst bucket diff %.4f%% of total (tol %.2f%%)' % (worst * 100, bucket_tol * 100))
    d = max(float(np.abs(ref['lo'] - cand['lo']).max()), float(np.abs(ref['hi'] - cand['hi']).max()))
    check(d <= bounds_tol, label + ' bounds', 'max diff %.4f units (tol %.2f)' % (d, bounds_tol))


# ------------------------------------------------------------------ payload readers

def read_occlusion(path):
    data = mvm_cache.read_payload(path)
    assert data[:8] == b'MVMOCC01', path
    vc, ic = struct.unpack_from('<ii', data, 8)
    o = 8 + 8 + 24
    pos = np.frombuffer(data, np.float32, vc * 3, o).reshape(-1, 3)
    o += vc * 12
    idx = np.frombuffer(data, np.uint32, ic, o)
    assert o + ic * 4 == len(data), 'trailing bytes in ' + path
    return pos, idx


def read_casters(path):
    data = mvm_cache.read_payload(path)
    assert data[:8] == b'MVMCAST6', path
    cutouts, _, rvc, ric = struct.unpack_from('<iiii', data, 8)
    o = 24
    rpos = np.frombuffer(data, np.float32, rvc * 3, o).reshape(-1, 3); o += rvc * 12
    ridx = np.frombuffer(data, np.uint32, ric, o); o += ric * 4
    ralb = np.frombuffer(data, np.uint32, ric // 3, o); o += (ric // 3) * 4
    cuts = []
    for _ in range(cutouts):
        vc, ic, w, h = struct.unpack_from('<iiii', data, o); o += 16
        cutoff, = struct.unpack_from('<f', data, o); o += 4
        anim = struct.unpack_from('<8f', data, o); o += 32
        au, av, alb = struct.unpack_from('<iiI', data, o); o += 12
        verts = np.frombuffer(data, np.float32, vc * 8, o).reshape(-1, 8); o += vc * 32
        idx = np.frombuffer(data, np.uint32, ic, o); o += ic * 4
        alpha = np.frombuffer(data, np.uint8, w * h, o); o += w * h
        cuts.append(dict(vc=vc, ic=ic, w=w, h=h, cutoff=cutoff, anim=anim, au=au, av=av, albedo=alb,
                         verts=verts, idx=idx, alpha=alpha))
    stained = []
    sc, = struct.unpack_from('<i', data, o); o += 4
    for _ in range(sc):
        vc, ic, cw, ch = struct.unpack_from('<iiii', data, o); o += 16
        verts = np.frombuffer(data, np.float32, vc * 5, o).reshape(-1, 5); o += vc * 20
        idx = np.frombuffer(data, np.uint32, ic, o); o += ic * 4
        color = np.frombuffer(data, np.uint8, cw * ch * 4, o); o += cw * ch * 4
        stained.append(dict(vc=vc, ic=ic, cw=cw, ch=ch, verts=verts, idx=idx, color=color))
    assert o == len(data), 'trailing bytes in ' + path
    return dict(rpos=rpos, ridx=ridx, ralb=ralb, cuts=cuts, stained=stained)


def unpack_albedo(a):
    return np.array([(int(a) >> (8 * i)) & 255 for i in range(3)], np.float64)


def albedo_mean(albedo, weights):
    ch = np.stack([(albedo >> (8 * i)) & 255 for i in range(3)], 1).astype(np.float64)
    w = weights / max(float(weights.sum()), 1e-12)
    return (ch * w[:, None]).sum(0)


def compare_casters(label, ref, cand):
    rs, cs = tri_stats(ref['rpos'], ref['ridx']), tri_stats(cand['rpos'], cand['ridx'])
    compare_geometry(label + ' rigid', rs, cs, 0.005, 0.005, 0.005, 0.05)
    if rs['count'] and cs['count']:
        ra, ca = albedo_mean(ref['ralb'], rs['area_per_tri']), albedo_mean(cand['ralb'], cs['area_per_tri'])
        d = float(np.abs(ra - ca).max())
        check(d <= 2.0, label + ' rigid area-weighted albedo',
              'ref %s cand %s max diff %.2f/255 (tol 2)' % (np.round(ra, 1), np.round(ca, 1), d))

    check(len(ref['cuts']) == len(cand['cuts']), label + ' cutout batch count',
          '%d vs %d' % (len(ref['cuts']), len(cand['cuts'])))

    def sig(c):
        return (c['vc'], c['ic'], c['w'], c['h'], round(c['cutoff'], 4),
                tuple(round(x, 4) for x in c['anim']), c['au'], c['av'])

    groups = {}
    for side, cuts in ((0, ref['cuts']), (1, cand['cuts'])):
        for c in cuts:
            groups.setdefault(sig(c), [[], []])[side].append(c)
    unmatched = [(k, len(v[0]), len(v[1])) for k, v in groups.items() if len(v[0]) != len(v[1])]
    check(not unmatched, label + ' cutout signatures (verts, indices, mask size, cutoff, animation, addressing)',
          'all matched' if not unmatched else '%d mismatched groups, first: %s' % (len(unmatched), unmatched[:3]))

    worst_alb = worst_mean = worst_flip = worst_area = 0.0
    pairs = 0
    for rl, cl in groups.values():
        if len(rl) != len(cl):
            continue
        # Batches with an identical signature (same mesh, other tint) differ only by placement and
        # albedo, so pair each reference batch with its closest unused candidate rather than by
        # sort order, which flips when two areas agree to within float rounding.
        area = lambda c: tri_stats(c['verts'][:, :3], c['idx'])['area']
        remaining = list(range(len(cl)))
        pairs_here = []
        for r in rl:
            ra = area(r)
            best = min(remaining, key=lambda i: (rel(ra, area(cl[i])) > 0.001,
                                                 float(np.abs(unpack_albedo(r['albedo']) - unpack_albedo(cl[i]['albedo'])).max()),
                                                 rel(ra, area(cl[i]))))
            remaining.remove(best)
            pairs_here.append((r, cl[best]))
        for r, c in pairs_here:
            pairs += 1
            worst_alb = max(worst_alb, float(np.abs(unpack_albedo(r['albedo']) - unpack_albedo(c['albedo'])).max()))
            ra, ca = r['alpha'].astype(np.int16), c['alpha'].astype(np.int16)
            worst_mean = max(worst_mean, float(np.abs(ra - ca).mean()))
            threshold = r['cutoff'] * 255.0
            worst_flip = max(worst_flip, float(((ra >= threshold) != (ca >= threshold)).mean()))
            worst_area = max(worst_area, rel(tri_stats(r['verts'][:, :3], r['idx'])['area'],
                                             tri_stats(c['verts'][:, :3], c['idx'])['area']))
    if pairs:
        check(worst_alb <= 2.0, label + ' cutout albedo',
              'worst channel diff %.1f/255 over %d pairs (tol 2)' % (worst_alb, pairs))
        check(worst_mean <= 0.5, label + ' cutout alpha mean diff', 'worst %.3f/255 (tol 0.5)' % worst_mean)
        check(worst_flip <= 0.001, label + ' cutout coverage flips',
              'worst %.4f%% of texels (tol 0.1%%)' % (worst_flip * 100))
        check(worst_area <= 0.001, label + ' cutout batch area', 'worst rel diff %.4f%% (tol 0.1%%)' % (worst_area * 100))

    check(len(ref['stained']) == len(cand['stained']), label + ' stained glass batch count',
          '%d vs %d' % (len(ref['stained']), len(cand['stained'])))
    skey = lambda s: (s['vc'], s['ic'], s['cw'], s['ch'])
    rsg, csg = sorted(ref['stained'], key=skey), sorted(cand['stained'], key=skey)
    if len(rsg) == len(csg):
        bad = [skey(r) for r, c in zip(rsg, csg) if skey(r) != skey(c)]
        check(not bad, label + ' stained glass signatures', 'all matched' if not bad else str(bad[:3]))
        if not bad and rsg:
            worst_c = max(float(np.abs(r['color'].astype(np.int16) - c['color'].astype(np.int16)).mean())
                          for r, c in zip(rsg, csg))
            check(worst_c <= 3.0, label + ' stained glass colour', 'worst mean abs diff %.2f/255 (tol 3)' % worst_c)


# ------------------------------------------------------------------ authored entities

NUM = re.compile(r'-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?')


def same_value(a, b):
    if a == b:
        return True
    sa, sb = str(a), str(b)
    na, nb = NUM.findall(sa), NUM.findall(sb)
    if na and len(na) == len(nb) and NUM.sub('#', sa).replace(' ', '') == NUM.sub('#', sb).replace(' ', ''):
        return all(abs(float(x) - float(y)) <= 1e-4 * max(1.0, abs(float(x))) for x, y in zip(na, nb))
    return False


def vec_close(a, b, tol):
    if a is None or b is None:
        return a is None and b is None
    return len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def compare_entities(label, ref, cand, scalar_keys):
    rmap, cmap = {e['identity']: e for e in ref}, {e['identity']: e for e in cand}
    check(len(ref) == len(cand), label + ' count', '%d vs %d' % (len(ref), len(cand)))
    missing, extra = sorted(set(rmap) - set(cmap)), sorted(set(cmap) - set(rmap))
    check(not missing and not extra, label + ' identities',
          'identical' if not missing and not extra else 'missing %d (e.g. %s), extra %d (e.g. %s)'
          % (len(missing), missing[:2], len(extra), extra[:2]))
    problems = []
    for ident in sorted(set(rmap) & set(cmap)):
        r, c = rmap[ident], cmap[ident]
        if r.get('kind') != c.get('kind'):
            problems.append((ident, 'kind', r.get('kind'), c.get('kind')))
        for k in ('origin', 'angles', 'color'):
            if (k in r or k in c) and not vec_close(r.get(k), c.get(k), 1e-3):
                problems.append((ident, k, r.get(k), c.get(k)))
        for k in scalar_keys:
            a, b = r.get(k), c.get(k)
            if (a is None) != (b is None) or (a is not None and rel(float(a), float(b)) > 1e-5):
                problems.append((ident, k, a, b))
        rp, cp = r.get('properties', {}), c.get('properties', {})
        if set(rp) != set(cp):
            problems.append((ident, 'property keys', sorted(set(rp) ^ set(cp))[:5]))
        for k in set(rp) & set(cp):
            a, b = rp[k], cp[k]
            if isinstance(a, dict) and isinstance(b, dict):
                if a.get('type') != b.get('type') or not same_value(a.get('value'), b.get('value')):
                    problems.append((ident, 'property ' + k, a, b))
            elif not same_value(a, b):
                problems.append((ident, 'property ' + k, a, b))
    check(not problems, label + ' fields and properties',
          'all equal' if not problems else '%d differences, first: %s' % (len(problems), problems[:3]))


# ------------------------------------------------------------------ driver

def load_manifest(folder, m):
    with open(os.path.join(folder, m + '.json'), encoding='utf-8-sig') as f:
        return json.load(f)


def payload(folder, digest, ext):
    return os.path.join(folder, digest + ext)


def load_json(path):
    with open(path, encoding='utf-8-sig') as f:
        return json.load(f)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    refdir, canddir, maps = sys.argv[1], sys.argv[2], sys.argv[3:]
    for m in maps:
        print('== ' + m)
        rm, cm = load_manifest(refdir, m), load_manifest(canddir, m)
        check(cm.get('format') == 4 and cm.get('map') == m, m + ' manifest',
              'format %s map %s' % (cm.get('format'), cm.get('map')))
        # Format 2 (VRF pipeline) records sourceDigest; format 3 (native) records mapDigest.
        digest = lambda d: d.get('mapDigest') or d.get('sourceDigest')
        check(digest(rm) == digest(cm), m + ' map package digest (same game files)', '%s vs %s' % (digest(rm), digest(cm)))

        rl = load_json(payload(refdir, rm['lightsDigest'], '.lights.json'))
        cl = load_json(payload(canddir, cm['lightsDigest'], '.lights.json'))
        check(cl.get('format') == 1 and cl.get('map') == m, m + ' lights header',
              'format %s map %s' % (cl.get('format'), cl.get('map')))
        compare_entities(m + ' lights', rl['lights'], cl['lights'],
                         ('brightness', 'brightnessLumens', 'brightnessScale', 'range'))
        compare_entities(m + ' environment', rl['environment'], cl['environment'], ())

        rp, ri = read_occlusion(payload(refdir, rm['physicsDigest'], '.mvmocclusion'))
        cp, ci = read_occlusion(payload(canddir, cm['physicsDigest'], '.mvmocclusion'))
        compare_geometry(m + ' collision', tri_stats(rp, ri), tri_stats(cp, ci), 0.005, 0.001, 0.005, 0.05)

        compare_casters(m + ' casters', read_casters(payload(refdir, rm['visualDigest'], '.mvmcasters')),
                        read_casters(payload(canddir, cm['visualDigest'], '.mvmcasters')))

        rs, cs = rm.get('skyboxes') or [], cm.get('skyboxes') or []
        check(len(rs) == len(cs), m + ' skybox count', '%d vs %d' % (len(rs), len(cs)))
        for r, c in zip(rs, cs):
            check(r.get('package') == c.get('package'), m + ' skybox package',
                  '%s vs %s' % (r.get('package'), c.get('package')))
            check(r.get('cameraScale') == c.get('cameraScale') and vec_close(r.get('cameraOrigin'), c.get('cameraOrigin'), 1e-3),
                  m + ' skybox camera', '%s %s vs %s %s' % (r.get('cameraScale'), r.get('cameraOrigin'),
                                                            c.get('cameraScale'), c.get('cameraOrigin')))
            compare_casters(m + ' skybox casters', read_casters(payload(refdir, r['visualDigest'], '.mvmcasters')),
                            read_casters(payload(canddir, c['visualDigest'], '.mvmcasters')))
    print()
    print('FAILED %d checks' % len(FAILS) if FAILS else 'ALL CHECKS PASSED')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
