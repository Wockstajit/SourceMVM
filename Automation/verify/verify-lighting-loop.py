"""Save and replay a live lighting comparison using netcon only.

Captures use mirv_streams with the explicit preview-quality override. Timings are
collected separately, with recording stopped. Private session data stays in the
ignored output directory; no desktop capture, focus changes or input injection.
"""
import argparse
import importlib.util
import json
import re
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("motion", ROOT / "Automation/verify/profile-relight-motion.py")
motion = importlib.util.module_from_spec(spec)
spec.loader.exec_module(motion)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--label", default="baseline")
    parser.add_argument("--port", type=int, default=29010)
    parser.add_argument("--seconds", type=float, default=22)
    parser.add_argument("--capture", type=float, default=0)
    parser.add_argument("--export-capture", action="store_true", help="Baseline identification only; changes rendering quality")
    parser.add_argument("--command", action="append", default=[])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    net = motion.Netcon(args.port)
    transcript = []

    def command(commands, seconds=.3):
        reply = net.command(commands, seconds)
        transcript.append({"commands": commands, "reply": reply})
        return reply

    try:
        if args.command:
            command(args.command, 1)
        state = command(["mirv_filmmaker loop status", "mirv_filmmaker demoprobe",
                         "mirv_filmmaker cosmetics status", "mirv_relight status",
                         "mirv_filmmaker environment status", "mirv_filmmaker window status",
                         "mirv_fov", "getpos", "mirv_relight castercull", "mirv_relight clusterbounds",
                         "mirv_relight reflectionfilter", "mirv_relight reflectionfollow"], 1.5)
        loop = re.search(r"loop (ON|OFF) -- in (\d+), out (\d+)", state)
        if not loop:
            raise RuntimeError("No confirmed playback loop; refusing to replace it")
        report = {"label": args.label, "loop": {"enabled": loop[1] == "ON", "in": int(loop[2]), "out": int(loop[3])},
                  "initialState": state, "captureQuality": None, "profiles": []}
        if args.seconds:
            command(["mirv_filmmaker gpuprofile on", "mirv_filmmaker health resetTiming", "mirv_filmmaker gpuprofile reset"])
            # Keep draining the console: a blocked netcon client can stall CS2.
            until = time.monotonic() + args.seconds
            while time.monotonic() < until:
                net.read(min(5, max(.01, until - time.monotonic())))
                report["profiles"].append(command(["mirv_filmmaker health"], .6))
            lines=report["profiles"][-1].replace("\r", "").splitlines()
            print("\n".join(line.strip() for line in lines if "frame timing:" in line or "gpu profile over" in line or "lamp face refresh" in line), flush=True)
        if args.capture:
            override = not args.export_capture
            if override:
                reply = command(["mirv_relight previewcapture 1"], .5)
                if "previewcapture=1" not in reply:
                    raise RuntimeError("Build has no confirmed preview-quality capture override")
            report["captureQuality"] = "preview" if override else "export (identification only)"
            name = (args.out.resolve() / args.label).as_posix()
            if '"' in name or "\n" in name:
                raise ValueError("Unsafe capture name")
            try:
                command([f'mirv_streams record name "{name}"', "mirv_streams record format bmp",
                         "mirv_streams record screen enabled 1", "mirv_streams record start"], .6)
                net.read(args.capture)
            finally:
                command(["mirv_streams record end", "mirv_streams record screen enabled 0", "mirv_streams record format tga"], .5)
                if override:
                    command(["mirv_relight previewcapture 0"])
        report["finalState"] = command(["mirv_filmmaker loop status", "mirv_filmmaker cosmetics status", "getpos"], .5)
        (args.out / f"{args.label}.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    finally:
        (args.out / f"{args.label}-netcon.json").write_text(json.dumps(transcript, indent=2), encoding="utf-8")
        net.socket.close()


if __name__ == "__main__":
    main()
