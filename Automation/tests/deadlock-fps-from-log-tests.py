"""Tests for Automation/tools/deadlock-fps-from-log.py.

  py -3 Automation/tests/deadlock-fps-from-log-tests.py
"""
from __future__ import annotations

import importlib.util
import io
import pathlib
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("fpslog", ROOT / "Automation" / "tools" / "deadlock-fps-from-log.py")
fpslog = importlib.util.module_from_spec(spec)
# dataclasses resolves string annotations through sys.modules, so the module must be registered
# before it executes.
sys.modules["fpslog"] = fpslog
spec.loader.exec_module(fpslog)


def view(t: str, n: int) -> str:
    return (f"[{t}] [seq=1] [deadlock.view] [tick=5 frame=9 thread=1] n={n} writes=0 dt=0.0000 "
            f"base=0 origin=(0 0 0) angles=(0 0 0) fov=90 vp=1920x1080 phase=after")


def perf(t: str, mean: float, peak: float) -> str:
    return f"[{t}] [seq=2] [deadlock.perf] [tick=5 frame=9 thread=1] workspace=camera frames=400 meanMs={mean} peakMs={peak}"


def mark(t: str, name: str, edge: str) -> str:
    return f"[{t}] [seq=3] [console.engine] [tick=5 frame=9 thread=1] FPS_{name}_{edge} |"


class FpsFromLogTests(unittest.TestCase):
    def test_median_fps_is_64_calls_over_elapsed_time(self):
        lines = [
            mark("10:00:00.000", "A", "BEGIN"),
            view("10:00:00.100", 64),
            view("10:00:00.420", 128),   # 64 calls / 0.32 s = 200 fps
            view("10:00:00.740", 192),   # 200 fps
            view("10:00:01.140", 256),   # 64 / 0.40 = 160 fps
            perf("10:00:01.200", 0.2, 0.5),
            perf("10:00:03.200", 0.4, 0.9),
            mark("10:00:03.300", "A", "END"),
        ]
        r = fpslog.parse(lines)["A"]
        self.assertEqual(r["samples"], 3)
        self.assertAlmostEqual(r["medianFps"], 200.0, places=1)
        self.assertLess(r["p5Fps"], r["medianFps"])
        self.assertAlmostEqual(r["pumpMeanMs"], 0.3, places=3)
        self.assertAlmostEqual(r["pumpPeakMs"], 0.9, places=3)

    def test_window_names_may_contain_underscores(self):
        lines = [
            mark("10:00:00.000", "P4A_B", "BEGIN"),
            view("10:00:00.100", 64),
            view("10:00:00.420", 128),
            view("10:00:00.740", 192),
            view("10:00:01.060", 256),
            mark("10:00:01.100", "P4A_B", "END"),
        ]
        result = fpslog.parse(lines)
        self.assertEqual(set(result), {"P4A_B"})
        self.assertAlmostEqual(result["P4A_B"]["medianFps"], 200.0, places=1)

    def test_lines_outside_windows_and_unclosed_windows_are_ignored(self):
        lines = [
            view("10:00:00.000", 64),
            mark("10:00:00.050", "B", "BEGIN"),
            view("10:00:00.100", 128),
            view("10:00:00.420", 192),
            view("10:00:00.740", 256),
            view("10:00:01.060", 320),
            mark("10:00:01.100", "B", "END"),
            view("10:00:01.500", 384),
            mark("10:00:01.600", "C", "BEGIN"),
            view("10:00:01.900", 448),
        ]
        result = fpslog.parse(lines)
        self.assertEqual(set(result), {"B"})
        self.assertEqual(result["B"]["samples"], 3)

    def test_counter_reset_uses_calls_per_sample(self):
        lines = [
            mark("10:00:00.000", "R", "BEGIN"),
            view("10:00:00.100", 6400),
            view("10:00:00.420", 64),      # hook reloaded: counter restarted
            view("10:00:00.740", 128),
            view("10:00:01.060", 192),
            mark("10:00:01.100", "R", "END"),
        ]
        r = fpslog.parse(lines)["R"]
        self.assertEqual(r["samples"], 3)
        self.assertAlmostEqual(r["medianFps"], 200.0, places=1)

    def test_midnight_rollover_keeps_time_monotonic(self):
        lines = [
            mark("23:59:59.500", "M", "BEGIN"),
            view("23:59:59.600", 64),
            view("23:59:59.920", 128),
            view("00:00:00.240", 192),
            view("00:00:00.560", 256),
            mark("00:00:00.600", "M", "END"),
        ]
        r = fpslog.parse(lines)["M"]
        self.assertEqual(r["samples"], 3)
        self.assertAlmostEqual(r["medianFps"], 200.0, places=1)

    def test_cli_fails_when_a_window_has_too_few_samples(self):
        log = ROOT / "build" / "deadlock-fps-from-log-test.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("\n".join([
            mark("10:00:00.000", "S", "BEGIN"),
            view("10:00:00.100", 64),
            view("10:00:00.420", 128),
            mark("10:00:00.500", "S", "END"),
        ]), encoding="utf-8")
        try:
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(fpslog.main([str(log)]), 1)
        finally:
            log.unlink()

    def test_cli_reports_no_windows(self):
        log = ROOT / "build" / "deadlock-fps-from-log-empty.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(view("10:00:00.100", 64), encoding="utf-8")
        try:
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(fpslog.main([str(log)]), 2)
        finally:
            log.unlink()


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
