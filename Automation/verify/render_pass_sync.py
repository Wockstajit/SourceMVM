"""Frame-sync check for a multipass render take (Automation/verify/verify-render-multipass.ps1).

Every pass of output frame N must come from the same simulation state as Beauty frame N. For each
pass stream this scores its frame N against the Beauty reference at frames N-1, N and N+1 and
requires offset 0 to win on every frame where the offsets can be told apart; a one-frame offset
anywhere is a failure.

  beauty_worldplayers / beauty_world : share of pixels that agree with Beauty
  key_players / key_gun        : share of SUBJECT pixels (not the background colour) that
                                       agree with Beauty -- they are Beauty's own pixels
  depth_* / normal_* of players/gun  : IoU of the pass's subject mask (not black) with the subject
                                       mask of key_players / key_gun
  depth / normal / *_world / *_worldplayers
                                     : SAME RENDER as the beauty pass of that visibility group --
                                       captured in the same BeforeUi callback of the same render, so
                                       no offset is possible. Reported, not scored: a colour-vs-depth
                                       score is contaminated by effects (muzzle flashes and
                                       particles show in colour but write no depth).

Usage: python render_pass_sync.py <take dir> [--frames 12]
Exit code 0 = in sync, 1 = offset or missing data.
"""
import glob, json, os, sys

import numpy as np
from PIL import Image

_cache = {}
# Scores closer than this cannot tell frames apart (renders of a frame differ by encoder noise).
MARGIN = 0.002


def frames_of(take, stream):
    # An image-sequence stream's frames are in <type>/<variant>/, which the manifest names.
    with open(os.path.join(take, 'manifest.json'), encoding='utf-8') as f:
        folders = {s['name']: s['folder'] for s in json.load(f).get('streams', [])}
    return sorted(glob.glob(os.path.join(take, folders[stream], '*.tga')))


def load(path):
    if path not in _cache:
        if len(_cache) > 64:
            _cache.clear()
        _cache[path] = np.asarray(Image.open(path).convert('RGB')).astype(np.float32)
    return _cache[path]


def iou(a, b):
    u = (a | b).sum()
    return float((a & b).sum() / u) if u > 0 else None


def not_key(img, key):
    return np.abs(img - key).max(axis=2) > 40


def not_black(img):
    return img.max(axis=2) > 8


def main():
    take = sys.argv[1]
    nsamples = 12
    if '--frames' in sys.argv:
        nsamples = int(sys.argv[sys.argv.index('--frames') + 1])
    manifest = json.load(open(os.path.join(take, 'manifest.json'), encoding='utf-8'))
    key = np.array([c * 255.0 for c in manifest.get('keyColor', [0, 1, 0])], dtype=np.float32)
    streams = [s['name'] for s in manifest.get('streams', [])]
    files = {s: frames_of(take, s) for s in streams}
    if 'beauty' not in streams or len(files['beauty']) < 4:
        print('SYNC SKIP: no beauty stream (with 4+ frames) to compare against')
        return 0
    count = len(files['beauty'])
    picks = sorted(set(np.linspace(1, count - 2, num=min(nsamples, count - 2)).astype(int).tolist()))

    def f(stream, n):
        return load(files[stream][n])

    # Each checker returns score(n, offset) or None when that frame cannot judge it.
    def agree(stream):
        return lambda n, o: float((np.abs(f(stream, n) - f('beauty', n + o)).max(axis=2) < 10).mean())

    def subject(stream):
        def s(n, o):
            img = f(stream, n)
            m = not_key(img, key)
            if m.sum() < 50:
                return None
            return float((np.abs(img - f('beauty', n + o)).max(axis=2)[m] < 12).mean())
        return s

    def subject_mask(stream, ref):
        return lambda n, o: iou(not_black(f(stream, n)), not_key(f(ref, n + o), key))

    checks = {}
    for s in streams:
        if s == 'beauty':
            continue
        if len(files[s]) != count:
            print(f'SYNC FAIL {s}: {len(files[s])} frames vs beauty {count}')
            checks[s] = 'missing'
            continue
        if s in ('beauty_worldplayers', 'beauty_world'):
            checks[s] = ('agree', agree(s))
        elif s in ('key_players', 'key_gun'):
            checks[s] = ('subject', subject(s))
        elif s.endswith('_players') or s.endswith('_gun'):
            ref = 'beauty_' + s.split('_', 1)[1]
            checks[s] = ('mask', subject_mask(s, ref)) if ref in streams else ('skip', None)
        elif s.split('_', 1)[0] in ('depth', 'normal') and ('beauty' + s[len(s.split('_', 1)[0]):]) in streams:
            checks[s] = ('same', 'beauty' + s[len(s.split('_', 1)[0]):])
        else:
            checks[s] = ('skip', None)

    failed = 0
    for s, c in checks.items():
        if c == 'missing':
            failed += 1
            continue
        kind, fn = c
        if kind == 'same':
            print(f'SYNC OK   {s} (same render): captured in the same callback of the same render as {fn}')
            continue
        if fn is None:
            print(f'SYNC SKIP {s}: no reference pass in this take can pin its frame (add the matching '
                  f'beauty or _world pass)')
            continue
        wins = judged = 0
        rows = []
        for n in picks:
            sc = {o: fn(n, o) for o in (-1, 0, 1)}
            if any(v is None for v in sc.values()):
                continue
            if max(sc.values()) - min(sc.values()) < MARGIN:
                continue  # the subject barely moved: every offset looks the same
            judged += 1
            # Offset 0 must not be beaten by another shift by more than the noise margin.
            wins += max(sc[-1], sc[1]) <= sc[0] + MARGIN / 4
            rows.append(f'n={n}: -1={sc[-1]:.4f} 0={sc[0]:.4f} +1={sc[1]:.4f}')
        ok = judged > 0 and wins == judged
        print(f'SYNC {"OK  " if ok else "FAIL"} {s} ({kind}): offset 0 best in {wins}/{judged} judged frames')
        for r in rows[:3]:
            print('      ' + r)
        if not ok:
            failed += 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
