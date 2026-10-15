"""Smooth View live-vs-demo comparison.

Turns one or more mvm_debug logs captured with `mirv_filmmaker smoothtest start` (or the plain
`smoothview trace` / `aimresample trace` commands) into the standard metrics for judging whether
demo playback matches live: how much the viewmodel/crosshair move relative to the aim (the "gun
sensitivity" complaint), how well the camera tracks the aim, and whether the free-aim residual
actually decays to ~0 when the player stops moving their view.

Everything here reads mvm_debug log lines directly -- no engine access, no dependency on the DLL
beyond the log format. Safe to run against old captures too (tolerates the pre-2026-09-01 log
format, which has one `residual=` field instead of `residualRaw=`+`residual=`).

Usage:
    python Automation/verify/smoothview-compare.py <label>=<path> [<label>=<path> ...]

With one file: prints that leg's metrics.
With two or more: prints them side by side AND flags any settings mismatch found in a
`smoothtest.session` line (if present) -- a tightness/vmSmooth/faMax difference between legs
invalidates the comparison, and mismatches have silently wasted whole sessions before.
"""
import json
import math
import re
import statistics
import sys

SESSION = re.compile(
    r'\[smoothtest\.session\].*?leg=(\S+) tick=(\d+) smoothview=(\{.*?\}) aimresample=(\{.*?\})')
SV = re.compile(
    r'\[camera\.smoothview\.trace\].*?frame=(\d+) renderDt=([\d.]+) paused=(\d) tightness=([\d.]+) '
    r'fov=([\d.]+) aim=\(([-\d.]+) ([-\d.]+)\) camera=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\).*?'
    r'(?:residualRaw=\(([-\d.]+) ([-\d.]+)\) )?residual=\(([-\d.]+) ([-\d.]+)\) '
    r'pitchClamp=(\d) coneClamp=(\d)')
GUN = re.compile(
    r'\[camera\.smoothview\.gun\].*?frame=(\d+) smoothing=([\d.]+) residual=\(([-\d.]+) ([-\d.]+)\) '
    r'raw=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\) out=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)')
VMAIM = re.compile(
    r'\[viewfx\.vm\.aim\].*?frame=(\d+) pov=(\d) pawn=(-?\d+) requested=\(([-\d.]+) ([-\d.]+)\) '
    r'applied=\(([-\d.]+) ([-\d.]+)\) strength=([\d.]+) smoothing=([\d.]+)')

MOVE_SPEED_GATE = 50.0   # deg/s -- "moving" frames for the ratio metrics, matches the original
                         # live-vs-demo A/B so numbers stay comparable to past sessions.
LOW_SPEED_GATE = 15.0    # deg/s -- "holding still" frames for the decay check
HOLD_FRAMES = 60         # consecutive low-speed frames before a hold counts (~0.3-0.4s at 150-200fps)


def wrap(d):
    while d > 180: d -= 360
    while d < -180: d += 360
    return d


def load(path):
    session = None
    sv, gun, vmaim = {}, {}, {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SESSION.search(line)
        if m:
            try:
                session = dict(
                    leg=m.group(1), tick=int(m.group(2)),
                    smoothview=json.loads(m.group(3)), aimresample=json.loads(m.group(4)))
            except ValueError:
                pass
            continue
        m = SV.search(line)
        if m:
            frame = int(m.group(1))
            rawP = float(m.group(11)) if m.group(11) is not None else float(m.group(13))
            rawY = float(m.group(12)) if m.group(12) is not None else float(m.group(14))
            sv[frame] = dict(
                dt=float(m.group(2)), paused=int(m.group(3)), tight=float(m.group(4)),
                ap=float(m.group(6)), ay=float(m.group(7)),
                resRawP=rawP, resRawY=rawY,
                resP=float(m.group(13)), resY=float(m.group(14)),
                pitchClamp=int(m.group(15)), coneClamp=int(m.group(16)))
            continue
        m = GUN.search(line)
        if m:
            gun[int(m.group(1))] = dict(
                smooth=float(m.group(2)), ox=float(m.group(8)), oy=float(m.group(9)),
                oz=float(m.group(10)))
            continue
        m = VMAIM.search(line)
        if m:
            vmaim[int(m.group(1))] = dict(
                pov=int(m.group(2)), pawn=int(m.group(3)),
                reqP=float(m.group(4)), reqY=float(m.group(5)),
                appP=float(m.group(6)), appY=float(m.group(7)),
                strength=float(m.group(8)), smoothing=float(m.group(9)))
    return session, sv, gun, vmaim


def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def ratio_metric(frames, sv, other, key, wrap_delta, speedgate):
    """sum(|delta other[key]|) / sum(|delta aim yaw|) over consecutive moving frames."""
    sum_num = sum_den = 0.0
    n = 0
    for a, b in zip(frames, frames[1:]):
        if b - a != 1 or sv[b]['paused'] or sv[b]['dt'] <= 0:
            continue
        if a not in other or b not in other:
            continue
        daim = wrap(sv[b]['ay'] - sv[a]['ay'])
        speed = abs(daim) / sv[b]['dt']
        if speed <= speedgate:
            continue
        d = other[b][key] - other[a][key]
        if wrap_delta:
            d = wrap(d)
        sum_num += abs(d)
        sum_den += abs(daim)
        n += 1
    if n < 10 or sum_den <= 0:
        return None, 0
    return sum_num / sum_den, n


def sparse_ratio_metric(sv, other, key, wrap_delta, speedgate, max_gap_frames=60):
    """Same ratio as ratio_metric, but for a THROTTLED channel (e.g. viewfx.vm.aim, logged
    1-in-15 frames) where consecutive samples are never frame-adjacent. Pairs consecutive
    entries in `other` directly; skips a pair if the frame gap is too large (a scene cut /
    pause / reconnect) or if `sv` has no aim data at both ends to compute a speed gate from.
    """
    oframes = sorted(other)
    sum_num = sum_den = 0.0
    n = 0
    for a, b in zip(oframes, oframes[1:]):
        if b - a > max_gap_frames or a not in sv or b not in sv:
            continue
        if sv[b]['paused'] or sv[a]['paused']:
            continue
        daim = wrap(sv[b]['ay'] - sv[a]['ay'])
        dt = sv[b]['dt'] * (b - a)  # approx elapsed time across the throttled gap
        if not (dt > 0):
            continue
        speed = abs(daim) / dt
        if speed <= speedgate:
            continue
        d = other[b][key] - other[a][key]
        if wrap_delta:
            d = wrap(d)
        sum_num += abs(d)
        sum_den += abs(daim)
        n += 1
    if n < 10 or sum_den <= 0:
        return None, 0
    return sum_num / sum_den, n


def camera_tracking_ratio(frames, sv, speedgate):
    """How much of the aim's motion the CAMERA reproduces -- 1.0 = perfect 1:1 tracking.

    Reconstructed as aim - RAW residual (residualRaw), never the post-aimresample-filter
    `residual` field. The actual rendered camera pose is finalized in ApplyCamera BEFORE the
    demo aim resampler ever runs -- that filter only touches the free-aim RESIDUAL afterwards,
    for the viewmodel/crosshair, and is explicitly forbidden from moving the view (see the
    "THE CAMERA PATH IS DELIBERATELY UNTOUCHED" comment in SmoothView.cpp). Using the filtered
    residual here would make the resampler's OWN smoothing look like a camera-tracking change
    on the demo leg even though the camera itself never moved differently -- caught 2026-09-01
    when this showed a live/demo camera-tracking gap (0.905 vs 0.834) that didn't reproduce
    once fixed to use residualRaw. On old-format logs with no residualRaw field, `load()`
    already falls back resRawP/resRawY to the single `residual=` value, so this is safe there
    too (both fields are then identical, matching the pre-resampler log format's actual meaning).
    """
    sum_cam = sum_aim = 0.0
    n = 0
    for a, b in zip(frames, frames[1:]):
        if b - a != 1 or sv[b]['paused'] or sv[b]['dt'] <= 0:
            continue
        daim = wrap(sv[b]['ay'] - sv[a]['ay'])
        speed = abs(daim) / sv[b]['dt']
        if speed <= speedgate:
            continue
        camA = sv[a]['ay'] - sv[a]['resRawY']
        camB = sv[b]['ay'] - sv[b]['resRawY']
        dcam = wrap(camB - camA)
        sum_cam += abs(dcam)
        sum_aim += abs(daim)
        n += 1
    if n < 10 or sum_aim <= 0:
        return None
    return sum_cam / sum_aim


def find_holds(frames, sv, lowspeed, holdframes):
    holds, cur = [], []
    for a, b in zip(frames, frames[1:]):
        if b - a != 1 or sv[b]['paused'] or sv[b]['dt'] <= 0:
            if len(cur) >= holdframes: holds.append(cur)
            cur = []
            continue
        spd = math.hypot(wrap(sv[b]['ap'] - sv[a]['ap']), wrap(sv[b]['ay'] - sv[a]['ay'])) / sv[b]['dt']
        if spd < lowspeed:
            cur.append(b)
        else:
            if len(cur) >= holdframes: holds.append(cur)
            cur = []
    if len(cur) >= holdframes: holds.append(cur)
    return holds


def report(path):
    session, sv, gun, vmaim = load(path)
    frames = sorted(sv)
    if len(frames) < 100:
        return None, session

    out = {}
    out['frames'] = len(frames)
    out['fps'] = 1 / statistics.median([sv[f]['dt'] for f in frames if sv[f]['dt'] > 0])
    out['tight'] = statistics.median(sv[f]['tight'] for f in frames)
    out['vmSmooth'] = statistics.median(gun[f]['smooth'] for f in frames if f in gun) if gun else None

    out['gun_ratio'], out['gun_n'] = ratio_metric(frames, sv, gun, 'ox', False, MOVE_SPEED_GATE)
    if gun:
        gx = [gun[f]['ox'] for f in gun]
        gy = [gun[f]['oy'] for f in gun]
        gz = [gun[f]['oz'] for f in gun]
        gmag = [math.hypot(gun[f]['ox'], gun[f]['oy'], gun[f]['oz']) for f in gun]
        out['gun_peak_x'] = max(abs(v) for v in gx)
        out['gun_peak_y'] = max(abs(v) for v in gy)
        out['gun_peak_z'] = max(abs(v) for v in gz)
        out['gun_peak_mag'] = max(gmag)
        out['gun_rms_mag'] = math.sqrt(sum(v * v for v in gmag) / len(gmag))
    if vmaim:
        out['vmaim_ratio'], out['vmaim_n'] = sparse_ratio_metric(
            sv, vmaim, 'appP', False, MOVE_SPEED_GATE)
        applied_mag = [math.hypot(vmaim[f]['appP'], vmaim[f]['appY']) for f in vmaim]
        out['vmaim_peak'] = max(applied_mag) if applied_mag else None
        out['vmaim_p90'] = pct(applied_mag, 90)

    out['cam_track_ratio'] = camera_tracking_ratio(frames, sv, MOVE_SPEED_GATE)

    resmag = [math.hypot(sv[f]['resP'], sv[f]['resY']) for f in frames]
    out['res_rms'] = math.sqrt(sum(x * x for x in resmag) / len(resmag)) if resmag else None
    out['res_peak'] = max(resmag) if resmag else None
    out['res_p90'] = pct(resmag, 90)
    out['cone_clamped_pct'] = 100.0 * sum(sv[f]['coneClamp'] for f in frames) / len(frames)

    holds = find_holds(frames, sv, LOW_SPEED_GATE, HOLD_FRAMES)
    out['holds'] = len(holds)
    if holds:
        ends = [math.hypot(sv[h[-1]]['resP'], sv[h[-1]]['resY']) for h in holds]
        out['hold_end_median'] = statistics.median(ends)
        out['hold_end_max'] = max(ends)

    return out, session


def fmt(v, spec='%.4f'):
    return (spec % v) if isinstance(v, (int, float)) else '-'


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    cols = []
    for arg in args:
        label, _, path = arg.partition('=')
        r, session = report(path)
        if not r:
            print(f"{label}: no usable capture (need >=100 camera.smoothview.trace lines)")
            continue
        cols.append((label, r, session))
    if not cols:
        sys.exit('no usable captures')

    # Settings-mismatch check first -- this is the trap that has invalidated comparisons before.
    if len(cols) > 1:
        tights = {round(r['tight'], 1) for _, r, _ in cols}
        vmsmooths = {round(r['vmSmooth'], 1) for _, r, _ in cols if r['vmSmooth'] is not None}
        if len(tights) > 1:
            print(f"!! aimTightness MISMATCH across legs: {tights} -- comparison is not valid.")
        if len(vmsmooths) > 1:
            print(f"!! faVmSmoothing MISMATCH across legs: {vmsmooths} -- comparison is not valid.")
        for label, r, session in cols:
            if session:
                fa = session['smoothview']
                print(f"  {label}: leg={session['leg']} tick={session['tick']} "
                      f"faMax={fa.get('faMax')} faPitchClamp={fa.get('faPitchClamp')} "
                      f"faVmAim={fa.get('faVmAim')} "
                      f"aimresample={session['aimresample'].get('on')}"
                      f"/{session['aimresample'].get('windowMs')}ms"
                      f"/{session['aimresample'].get('lagMs')}ms")
        print()

    rows = [
        ('--- SETTINGS ---', None, None),
        ('aimTightness', 'tight', '%.0f'),
        ('faVmSmoothing', 'vmSmooth', '%.0f'),
        ('fps', 'fps', '%.0f'),
        ('frames (n moving)', 'gun_n', '%d'),
        ('--- SENSITIVITY (the "gun moves too much" question) ---', None, None),
        ('gun offset per deg aim', 'gun_ratio', '%.4f'),
        ('gun offset peak X (units)', 'gun_peak_x', '%.4f'),
        ('gun offset peak Y (units)', 'gun_peak_y', '%.4f'),
        ('gun offset peak Z (units)', 'gun_peak_z', '%.4f'),
        ('gun offset peak |XYZ| (units)', 'gun_peak_mag', '%.4f'),
        ('gun offset RMS |XYZ| (units)', 'gun_rms_mag', '%.4f'),
        ('applied ROTATION per deg aim', 'vmaim_ratio', '%.4f'),
        ('applied rotation peak (deg)', 'vmaim_peak', '%.3f'),
        ('applied rotation p90 (deg)', 'vmaim_p90', '%.3f'),
        ('--- CAMERA TRACKING ---', None, None),
        ('camera reproduces this frac of aim', 'cam_track_ratio', '%.3f'),
        ('--- RESIDUAL (crosshair/gun offset from screen centre) ---', None, None),
        ('residual RMS (deg)', 'res_rms', '%.3f'),
        ('residual p90 (deg)', 'res_p90', '%.3f'),
        ('residual peak (deg)', 'res_peak', '%.3f'),
        ('% frames cone-clamped', 'cone_clamped_pct', '%.2f'),
        ('--- DOES IT RECENTRE WHEN YOU STOP AIMING? ---', None, None),
        ('low-motion holds found', 'holds', '%d'),
        ('residual at end of hold, median (deg)', 'hold_end_median', '%.3f'),
        ('residual at end of hold, worst (deg)', 'hold_end_max', '%.3f'),
    ]
    hdr = f"{'metric':<40}" + ''.join(f"{c[0]:>14}" for c in cols)
    print(hdr)
    print('-' * len(hdr))
    for label, key, spec in rows:
        if key is None:
            print(label)
            continue
        cells = ''.join(f"{fmt(r.get(key), spec):>14}" for _, r, _ in cols)
        print(f"{label:<40}{cells}")


if __name__ == '__main__':
    main()
