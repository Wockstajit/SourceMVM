"""A/B timing and picture check of lighting speed switches at fixed camera poses of the loaded map.

For each pose: put the camera there (mirv_input, so the player cannot move it), wait for the view to
settle, then for each switch alternate "off" and "on" for a few rounds and report the mean frame time
of each side with the spread between rounds. With --capture it also takes two window captures per
side with clouds and wind stopped, and reports how far "on" is from "off" beside how far two captures
of the same side are from each other (foliage, particles and the fps counter still move), and how
many pixels differ between the sides in both pairs while each side agrees with itself.

Run in a loaded offline map with Environment and Dynamic lighting on. Leaves every switch at its
default and the camera in mirv_input mode (`mirv_input end` returns control).

  python Automation/verify/profile-lighting-switches.py --pose "2472 2006 262 0 160 0" --switch stickydraws
  python Automation/verify/profile-lighting-switches.py --poses poses.json --capture out-folder
"""
import argparse
import importlib.util
import json
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prm", ROOT / "Automation" / "verify" / "profile-relight-motion.py")
prm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prm)

# name: (command for the previous path, command for the default)
SWITCHES = {
    "stickydraws": ("mirv_relight stickydraws 0", "mirv_relight stickydraws 1"),
    "recordreuse": ("mirv_relight recordreuse 0", "mirv_relight recordreuse 1"),
    "drawruns": ("mirv_relight drawruns 0", "mirv_relight drawruns 1"),
    "sunsummary": ("mirv_relight sunsummary 0", "mirv_relight sunsummary 1"),
    "volumesplit": ("mirv_relight volumesplit 0", "mirv_relight volumesplit 1"),
    "directsource": ("mirv_relight directsource 0", "mirv_relight directsource 1"),
    "fineslices": ("mirv_relight fineslices 0", "mirv_relight fineslices 1"),
    "litbounds": ("mirv_relight litbounds 0", "mirv_relight litbounds 1"),
    "tilerefresh": ("mirv_relight tilerefresh 0", "mirv_relight tilerefresh 1"),
    "movingactors": ("mirv_relight movingactors 0", "mirv_relight movingactors 1"),
    "engineexposure": ("mirv_relight engineexposure 0", "mirv_relight engineexposure 1"),
}
# The light-list and lamp-refresh switches added after the first four, together.
NEWER = ("fineslices", "litbounds", "tilerefresh", "movingactors", "engineexposure")
# Switches whose picture check needs the clouds drawn: only the wind is stopped for their captures.
NEEDS_CLOUDS = {"volumesplit", "directsource", "all"}
# Every switch at once: the previous paths together against the defaults together.
SWITCHES["all"] = ([off for off, _ in SWITCHES.values()], [on for _, on in SWITCHES.values()])
SWITCHES["newer"] = ([SWITCHES[name][0] for name in NEWER], [SWITCHES[name][1] for name in NEWER])


def commands(value):
    return value if isinstance(value, list) else [value]


def timing(net, seconds, settle):
    time.sleep(settle)
    net.command(["mirv_filmmaker health resetTiming"], 0.3)
    time.sleep(seconds)
    reply = net.command(["mirv_filmmaker health"], 1.2)
    t = re.search(r"frame timing: samples=(\d+) meanMs=([\d.]+) p50Ms=([\d.]+) p95Ms=([\d.]+)", reply)
    d = re.search(r"draw detours ([\d.]+) ms over (\d+) draws", reply)
    if not t:
        raise RuntimeError("no frame timing in the health reply")
    return {"samples": int(t[1]), "meanMs": float(t[2]), "p50Ms": float(t[3]), "p95Ms": float(t[4]),
            "detourMs": float(d[1]) if d else None, "draws": int(d[2]) if d else None}


def capture(path):
    script = ROOT / "Automation" / "capture" / "capture-game-window.ps1"
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-Out", str(path)],
                   check=True, capture_output=True)


def picture(shots):
    """How far the sides are apart, beside how far each side is from itself.

    `bothPairs` counts pixels that differ between the sides in both capture pairs while each side
    agrees with itself: random motion rarely does that at one pixel, a real difference does (it
    found a floor one switch darkened by 15 levels where the plain percentages looked like noise).
    Foliage swaying with the capture interval still shows there, so look at where the pixels are;
    `bothPairsControl` is the same test with the roles of the pairs exchanged.
    """
    from PIL import Image
    import numpy as np
    image = {side: np.asarray(Image.open(path).convert("RGB")).astype(np.int16) for side, path in shots.items()}
    apart = lambda a, b: np.abs(image[a] - image[b]).max(axis=2)
    pairs = {"onVsOff": apart("on", "off"), "on2VsOff2": apart("on2", "off2"),
             "offVsOff2": apart("off", "off2"), "onVsOn2": apart("on", "on2")}
    result = {name: {"mean": float(d.mean()), "over2": float((d > 2).mean() * 100), "over8": float((d > 8).mean() * 100)}
              for name, d in pairs.items()}
    both = (pairs["onVsOff"] > 8) & (pairs["on2VsOff2"] > 8) & (pairs["offVsOff2"] <= 2) & (pairs["onVsOn2"] <= 2)
    control = (pairs["offVsOff2"] > 8) & (pairs["onVsOn2"] > 8) & (pairs["onVsOff"] <= 2) & (pairs["on2VsOff2"] <= 2)
    result["bothPairs"] = float(both.mean() * 100)
    result["bothPairsControl"] = float(control.mean() * 100)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=29010)
    parser.add_argument("--pose", action="append", default=[], help='"x y z pitch yaw roll" of the camera eye')
    parser.add_argument("--poses", type=Path, help="JSON list of {name, pose:[x,y,z,pitch,yaw,roll]}")
    parser.add_argument("--switch", action="append", default=[], help="one of: " + ", ".join(SWITCHES) + " (default: all)")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--settle", type=float, default=2)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--capture", type=Path, help="folder for the picture check")
    parser.add_argument("--output", type=Path, help="JSON report")
    args = parser.parse_args()
    poses = [{"name": "pose%d" % i, "pose": [float(v) for v in p.split()]} for i, p in enumerate(args.pose)]
    if args.poses:
        poses += json.loads(args.poses.read_text(encoding="utf-8"))
    if not poses:
        parser.error("give --pose or --poses")
    switches = args.switch or list(SWITCHES)
    net = prm.Netcon(args.port)
    status = net.command(["mirv_relight status", "mirv_filmmaker environment status"], 1.2)
    map_name = (re.search(r"relight: enabled=\d+ .*map='([^']*)'", status) or [None, "?"])[1]
    net.command(["mirv_input camera", "mirv_input cfg offsetMode ownLast", "mirv_input cfg msens 0", "mirv_input cfg ksens 0"], 0.4)
    report = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "map": map_name, "poses": []}
    try:
        for entry in poses:
            pose = entry["pose"] + [0] * (6 - len(entry["pose"]))
            net.pose(pose)
            time.sleep(6)  # region allocation, shadow tiers and the bounce volume settle after a cut
            row = {"name": entry["name"], "pose": pose, "switches": {}}
            print("%s %s" % (map_name, entry["name"]), flush=True)
            for name in switches:
                off, on = SWITCHES[name]
                rounds = []
                for _ in range(args.rounds):
                    net.command(commands(off), 0.4)
                    a = timing(net, args.seconds, args.settle)
                    net.command(commands(on), 0.4)
                    b = timing(net, args.seconds, args.settle)
                    rounds.append({"off": a, "on": b})
                off_ms = [r["off"]["meanMs"] for r in rounds]
                on_ms = [r["on"]["meanMs"] for r in rounds]
                result = {"offMs": sum(off_ms) / len(off_ms), "onMs": sum(on_ms) / len(on_ms),
                          "offRange": [min(off_ms), max(off_ms)], "onRange": [min(on_ms), max(on_ms)],
                          "offDetourMs": rounds[-1]["off"]["detourMs"], "onDetourMs": rounds[-1]["on"]["detourMs"],
                          "offDraws": rounds[-1]["off"]["draws"], "onDraws": rounds[-1]["on"]["draws"], "rounds": rounds}
                if args.capture:
                    args.capture.mkdir(parents=True, exist_ok=True)
                    # Stop what moves on its own, then two captures per side.
                    net.command((["mirv_filmmaker environment climate clouds 0"] if name not in NEEDS_CLOUDS else []) +
                                ["mirv_filmmaker environment climate windSpeed 0"], 0.4)
                    shots = {}
                    for side, command in (("off", off), ("on", on), ("off2", off), ("on2", on)):
                        net.command(commands(command), 0.4)
                        time.sleep(2.5)
                        shots[side] = args.capture / ("%s-%s-%s-%s.png" % (map_name, entry["name"], name, side))
                        capture(shots[side])
                    net.command(["mirv_filmmaker environment climate clouds 1", "mirv_filmmaker environment climate windSpeed 25"], 0.4)
                    result["picture"] = picture(shots)
                row["switches"][name] = result
                line = "  %-12s off %6.2f ms (%.2f-%.2f)  on %6.2f ms (%.2f-%.2f)  saves %5.2f ms  detours %s -> %s ms  draws %s -> %s" % (
                    name, result["offMs"], *result["offRange"], result["onMs"], *result["onRange"], result["offMs"] - result["onMs"],
                    result["offDetourMs"], result["onDetourMs"], result["offDraws"], result["onDraws"])
                if "picture" in result:
                    p = result["picture"]
                    line += "  | pixels over 8 levels: on/off %.3f%% %.3f%%, same side %.3f%% %.3f%%, in both pairs %.4f%% (control %.4f%%)" % (
                        p["onVsOff"]["over8"], p["on2VsOff2"]["over8"], p["offVsOff2"]["over8"], p["onVsOn2"]["over8"],
                        p["bothPairs"], p["bothPairsControl"])
                print(line, flush=True)
            report["poses"].append(row)
    finally:
        net.command(SWITCHES["all"][1], 0.4)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=1), encoding="utf-8")
        print("report:", args.output)
    net.socket.close()


if __name__ == "__main__":
    main()
