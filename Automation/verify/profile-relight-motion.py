"""Measure uncaptured live camera motion without BMP writes or quality changes."""
import argparse
import json
import math
import re
import socket
import threading
import time
from pathlib import Path


class Netcon:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), 3)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.socket.settimeout(0.02)
        self.lock = threading.Lock()
        self.read(0.1)

    def write(self, commands):
        with self.lock:
            self.socket.sendall(("\n".join(commands) + "\n").encode("ascii"))

    def read(self, seconds):
        end = time.perf_counter() + seconds
        result = bytearray()
        while time.perf_counter() < end:
            try:
                data = self.socket.recv(65536)
                if not data:
                    raise RuntimeError("CS2 closed netcon")
                result.extend(data)
            except socket.timeout:
                pass
        return result.decode("utf-8", errors="replace")

    def command(self, commands, seconds=0.15):
        self.write(commands)
        return self.read(seconds)

    def pose(self, pose):
        self.write(["mirv_input position " + " ".join(f"{v:.6f}" for v in pose[:3]),
                    "mirv_input angles " + " ".join(f"{v:.6f}" for v in pose[3:])])

    def health(self, reset=False):
        self.write(["mirv_filmmaker health" + (" resetTiming" if reset else "")])
        reply = ""
        deadline = time.perf_counter() + 3
        while time.perf_counter() < deadline:
            reply += self.read(0.1)
            if "=> healthy:" in reply or "=> UNHEALTHY:" in reply:
                frame_count(reply)
                return reply
        raise RuntimeError("Timed out waiting for complete render health output")


def frame_count(reply):
    match = re.search(r"Before_Present\)\s*:\s*(\d+)", reply)
    if not match:
        raise RuntimeError("Missing render heartbeat in netcon reply")
    return int(match[1])


def measure(client, route, duration):
    client.pose(route["start"])
    client.read(2.0)  # Settle cut/region allocation outside the timed interval.
    first = client.health(reset=True)
    start = time.perf_counter()
    initial = previous = frame_count(first)
    previous_time = start
    stop = threading.Event()
    movement_errors = []

    def move():
        try:
            while not stop.is_set():
                elapsed = time.perf_counter() - start
                # Forward then reverse, including yaw/pitch changes. Fixed wall time.
                amount = min(1.0, elapsed / duration)
                phase = 1.0 - abs(2.0 * amount - 1.0)
                client.pose([a + (b - a) * phase for a, b in zip(route["start"], route["finish"])])
                stop.wait(1 / 60)
        except Exception as error:
            movement_errors.append(error)
            stop.set()

    worker = threading.Thread(target=move)
    worker.start()
    windows = []
    final = ""
    try:
        while time.perf_counter() - start < duration:
            time.sleep(0.35)
            if movement_errors:
                raise RuntimeError("Camera movement stopped") from movement_errors[0]
            final = client.health()
            now = time.perf_counter()
            count = frame_count(final)
            windows.append({"time": now - start, "fps": (count - previous) / (now - previous_time),
                            "frames": count - previous, "seconds": now - previous_time,
                            "health": final})
            previous, previous_time = count, now
    finally:
        stop.set()
        worker.join()
    if previous <= initial:
        raise RuntimeError("Rendering stopped during movement")
    result = {"name": route["name"], "route": route, "seconds": previous_time - start,
              "frames": previous - initial, "fps": (previous - initial) / (previous_time - start),
              "lowestWindowFps": min(w["fps"] for w in windows), "windows": windows, "finalHealth": final}
    match = re.search(r"frame timing: samples=(\d+) meanMs=([\d.]+) p50Ms=([\d.]+) p95Ms=([\d.]+) p99Ms=([\d.]+) maxMs=([\d.]+) over33=(\d+)", final)
    if match:
        keys = ["samples", "meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs", "over33"]
        result["frameTiming"] = {k: float(v) for k, v in zip(keys, match.groups())}
    print(json.dumps({k: v for k, v in result.items() if k not in ("windows", "route", "finalHealth")}), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--routes", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--port", type=int, default=29010)
    parser.add_argument("--debug", action="store_true", help="Keep debug logging enabled for diagnostic timing")
    args = parser.parse_args()
    routes = json.loads(args.routes.read_text(encoding="utf-8-sig"))
    for route in routes:
        for key in ("start", "finish"):
            if len(route[key]) != 6 or not all(math.isfinite(v) for v in route[key]):
                raise ValueError("Routes require finite six-component camera eye poses")
    if not 3 <= args.seconds <= 60:
        raise ValueError("Duration must be between 3 and 60 seconds per route")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    client = Netcon(args.port)
    status = client.command(["mirv_input position", "mirv_input angles", "mvm_debug status",
                             "mirv_relight status", "mirv_filmmaker environment status"], 1.0)
    current = re.findall(r"Current value: ([\d. -]+)", status)
    if len(current) < 2 or "relight: enabled=1" not in status:
        raise RuntimeError("Active relighting and a readable camera pose are required")
    draws = re.search(r"objects=\d+ .*?draws=(\d+)", status)
    player = client.command(["getpos"], 0.8)
    pose = re.search(r"setpos ([\d.-]+) ([\d.-]+) ([\d.-]+);setang", player)
    if not draws or int(draws[1]) < 100 or not pose or sum(abs(float(v)) for v in pose.groups()) < 1:
        client.socket.close()
        raise RuntimeError("Join a loaded offline map before profiling; menus are not lighting benchmarks")
    restore = [float(v) for part in current[:2] for v in part.split()]
    settings = {}
    for key in ("msens", "ksens", "offsetMode"):
        reply = client.command(["mirv_input cfg " + key], 0.4)
        value = re.search(r"Value: ([\w.+-]+)", reply)
        if not value:
            client.socket.close()
            raise RuntimeError("Cannot preserve camera setting: " + key)
        settings[key] = value[1]
    debug_running = "mvm_debug: RUNNING" in status
    results = []
    try:
        client.command(["mirv_input camera", "mirv_input cfg offsetMode ownLast",
                        "mirv_input cfg msens 0", "mirv_input cfg ksens 0",
                        "mvm_debug start" if args.debug else "mvm_debug stop"])
        for route in routes:
            results.append(measure(client, route, args.seconds))
    finally:
        client.pose(restore)
        client.command(["mirv_input cfg " + key + " " + value for key, value in settings.items()] +
                       ["mvm_debug start" if debug_running else "mvm_debug stop"])
        args.output.write_text(json.dumps({"initialStatus": status, "initialCameraSettings": settings, "debug": args.debug,
                                          "capturing": False, "results": results}, indent=2), encoding="utf-8")
        client.socket.close()


if __name__ == "__main__":
    main()
