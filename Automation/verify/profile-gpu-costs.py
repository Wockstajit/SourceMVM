"""What the hook's lighting costs the GPU at the current paused CS2 pose.

1. The per-pass GPU profile (`mirv_filmmaker gpuprofile`): every pass the hook records itself,
   most expensive first, beside the GPU frame, plus sampled lit-draw costs.
2. Work inside native CS2 draws (the patched material programs, the lamp shadow kernel, the light
   loop) has no pass of its own, so each piece is switched off in turn and the frame is timed
   against the defaults just before and after it.

Stands still: never moves the camera. Restores every setting it changes, from `mirv_relight status`
where it reports the value. Writes a JSON report under Automation/runs/gpu-profile/.

  py -3 Automation/verify/profile-gpu-costs.py [--seconds 6] [--rounds 2] [--only lamps,sun]
"""
import argparse
import importlib.util
import json
import re
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prm", ROOT / "Automation" / "verify" / "profile-relight-motion.py")
prm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prm)

# label, command that switches the piece off, mirv_relight status key, restore command template, default
CASES = [
    ("patched lit programs (flat shader instead)", "mirv_relight bindingcheck 1", "bindingcheck", "mirv_relight bindingcheck {}", "0"),
    ("lamp shadow lookups", "mirv_relight shadowreach -1", "shadowreach", "mirv_relight shadowreach {}", "0"),
    ("all shadows (map pass and lookups)", "mirv_relight shadows 0", "shadows", "mirv_relight shadows {}", "1"),
    ("sun shadow lookups", "mirv_relight sunshadow 0", "sunshadow", "mirv_relight sunshadow {}", "1"),
    ("local light records", "mirv_relight locallights 0", "locallights", "mirv_relight locallights {}", "1"),
    ("light loop beyond one record", "mirv_relight cost one", "cost", "mirv_relight cost {}", "0"),
    ("probe GI", "mirv_relight gi 0", "gi", "mirv_relight gi {}", "1"),
    ("reflection capture", "mirv_relight reflections 0", "reflections", "mirv_relight reflections {}", "1"),
    ("ambient occlusion", "mirv_relight ao 0", "ao", "mirv_relight ao {}", "1"),
    ("sky occlusion", "mirv_relight skyocclusion 0", "skyocclusion", "mirv_relight skyocclusion {}", "1"),
    ("stained glass", "mirv_relight stainedglass 0", "stainedglass", "mirv_relight stainedglass {}", "1"),
    ("environment composite", "mirv_filmmaker environment off", None, "mirv_filmmaker environment {}", "on"),
]
COST_WORDS = {"0": "full", "1": "word", "2": "one"}


def status_values(text):
    values = {}
    for key, value in re.findall(r"\b([a-z]+)=(-?[\w.]+)", text):
        values.setdefault(key, value)
    return values


def frame_timing(net):
    reply = net.command(["mirv_filmmaker health"], 1.2)
    t = re.search(r"frame timing: samples=(\d+) meanMs=([\d.]+) p50Ms=([\d.]+) p95Ms=([\d.]+)", reply)
    g = re.search(r"GPU frame ([\d.]+) ms \(p95 ([\d.]+)\), timed hook passes ([\d.]+) ms", reply)
    return {"samples": int(t[1]), "meanMs": float(t[2]), "p50Ms": float(t[3]), "p95Ms": float(t[4]),
            "gpuMs": float(g[1]) if g else None, "timedMs": float(g[3]) if g else None} if t else None


def measure(net, seconds, settle):
    time.sleep(settle)
    net.command(["mirv_filmmaker health resetTiming", "mirv_filmmaker gpuprofile reset"], 0.3)
    time.sleep(seconds)
    return frame_timing(net)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=29010)
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--settle", type=float, default=2.5)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--only", default="", help="comma-separated words; a case runs if its label contains one")
    args = parser.parse_args()
    net = prm.Netcon(args.port)
    pose = net.command(["getpos"], 0.4).strip()
    status = status_values(net.command(["mirv_relight status"], 1.0))
    net.command(["mirv_filmmaker gpuprofile on"], 0.3)
    baseline = measure(net, args.seconds + 2, args.settle)
    profile = net.command(["mirv_filmmaker gpuprofile"], 1.2)
    print(profile.strip(), flush=True)
    cases = [c for c in CASES if not args.only or any(w.strip() and w.strip() in c[0] for w in args.only.split(","))]
    results = []
    try:
        for label, off, key, restore, default in cases:
            value = status.get(key, default) if key else default
            if key == "cost": value = COST_WORDS.get(value, value)
            restore_command = restore.format(value)
            rows = []
            for _ in range(args.rounds):
                before = measure(net, args.seconds, 0.5)
                net.command([off], 0.5)
                off_timing = measure(net, args.seconds, args.settle)
                net.command([restore_command], 0.5)
                after = measure(net, args.seconds, args.settle)
                rows.append({"before": before, "off": off_timing, "after": after})
            on_ms = sum((r["before"]["meanMs"] + r["after"]["meanMs"]) / 2 for r in rows) / len(rows)
            off_ms = sum(r["off"]["meanMs"] for r in rows) / len(rows)
            gpu_on = [((r["before"]["gpuMs"] or 0) + (r["after"]["gpuMs"] or 0)) / 2 for r in rows]
            gpu_off = [r["off"]["gpuMs"] or 0 for r in rows]
            result = {"label": label, "off": off, "restore": restore_command, "onMs": on_ms, "offMs": off_ms,
                      "savedMs": on_ms - off_ms, "gpuOnMs": sum(gpu_on) / len(rows), "gpuOffMs": sum(gpu_off) / len(rows),
                      "rounds": rows}
            results.append(result)
            print("%-44s on %6.2f ms  off %6.2f ms  saves %5.2f ms (GPU frame %6.2f -> %6.2f)" % (
                label, on_ms, off_ms, on_ms - off_ms, result["gpuOnMs"], result["gpuOffMs"]), flush=True)
    finally:
        for label, off, key, restore, default in cases:
            value = status.get(key, default) if key else default
            if key == "cost": value = COST_WORDS.get(value, value)
            net.command([restore.format(value)], 0.3)
    end_pose = net.command(["getpos"], 0.4).strip()
    out = ROOT / "Automation" / "runs" / "gpu-profile"
    out.mkdir(parents=True, exist_ok=True)
    report = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "pose": pose, "endPose": end_pose, "baseline": baseline,
              "profile": profile, "cases": results}
    path = out / time.strftime("costs-%Y%m%d-%H%M%S.json")
    path.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("report:", path)
    net.socket.close()


if __name__ == "__main__":
    main()
