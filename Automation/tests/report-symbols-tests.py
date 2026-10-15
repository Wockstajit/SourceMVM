#!/usr/bin/env python3
"""Behavior checks for Automation/tools/report_symbols.py: which build a crash report names and
which archived image resolves it. DbgHelp itself is exercised only when a real archive exists."""

from pathlib import Path
import calendar
import importlib.util
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("report_symbols", ROOT / "Automation" / "tools" / "report_symbols.py")
rs = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(rs)

REPORT = """Summary   : ACCESS_VIOLATION at d3d11.dll+0x168d0c
At        : d3d11.dll+0x168d0c
Hook ID   : AfxHookSource2.dll/6ACF372Ff00000
Stack (module+offset, innermost first):
  #00 d3d11.dll+0x168d0c
  #01 AfxHookSource2.dll+0x4e24e0
  #02 0x0000000000001234 (no module)
First SOURCE:MVM frame: #01 AfxHookSource2.dll+0x4e24e0
SOURCE:MVM hook built : linked 2026-10-13 23:57:43 UTC, runtime 0.1.9-hotfix2
"""

# Frames: module-relative lines only; the summary line, the no-module frame and the
# first-own-frame line are not stack entries.
assert rs.parse_frames(REPORT) == [(0, "d3d11.dll", 0x168D0C), (1, "AfxHookSource2.dll", 0x4E24E0)]

# The crash header's Hook ID wins over the build line: the build line can describe the next session.
assert rs.hook_build(REPORT) == ("AfxHookSource2.dll", "6ACF372Ff00000", 0x6ACF372F)

# A bug report or rebuilt crash report carries the id on its build line.
assert rs.hook_build("SOURCE:MVM hook built : linked 2026-10-14 05:00:00 UTC, id 6acf372fF00000, runtime 0.2.0") == \
    ("AfxHookSource2.dll", "6ACF372Ff00000", 0x6ACF372F)

# Reports from before the id existed still name the link stamp ("linked ... UTC" is the PE stamp).
module, key, stamp = rs.hook_build("SOURCE:MVM hook built : linked 2026-10-13 23:57:43 UTC, runtime 0.1.9-hotfix2")
assert (module, key) == ("AfxHookSource2.dll", None)
assert stamp == calendar.timegm(time.strptime("2026-10-13 23:57:43", "%Y-%m-%d %H:%M:%S"))

# The oldest builds (__DATE__ text) name nothing resolvable.
assert rs.hook_build("SOURCE:MVM hook built : Oct 12 2026 18:39:16") == ("AfxHookSource2.dll", None, None)

# An old crash without Hook ID, found on disk and written up by a NEWER session: the build line
# names the writer, so it must not pick the symbols (that resolved an exit abort to unrelated code).
LATER = """SOURCE:MVM crash report (sent automatically)
Created: 2026-10-14 10:12:28
Summary   : abort() (SIGABRT) at AfxHookSource2.dll+0x4e24e0
  #00 AfxHookSource2.dll+0x4e24e0
SOURCE:MVM hook built : linked 2026-10-14 09:10:37 UTC, id 6ACF470Df0b000, runtime 0.1.9-hotfix4
"""
assert rs.written_by_later_session(LATER, "SourceMVM-crash-2026-10-13_22-31-04.txt")
assert rs.hook_build(LATER, "SourceMVM-crash-2026-10-13_22-31-04.txt") == ("AfxHookSource2.dll", None, None)
assert rs.symbolize(LATER, tempfile.gettempdir(), "SourceMVM-crash-2026-10-13_22-31-04.txt")[2] == rs.LATER_SESSION_REASON
# Written by the crashed session itself (name and Created agree): the build line is that build.
assert not rs.written_by_later_session(LATER, "SourceMVM-crash-2026-10-14_10-12-28.txt")
assert rs.hook_build(LATER, "SourceMVM-crash-2026-10-14_10-12-28.txt")[1] == "6ACF470Df0b000"
# A Hook ID always names the crashed session, whoever wrote the report.
assert rs.hook_build("Hook ID   : AfxHookSource2.dll/6ACF372Ff00000\n" + LATER,
                     "SourceMVM-crash-2026-10-13_22-31-04.txt")[1] == "6ACF372Ff00000"

with tempfile.TemporaryDirectory() as tmp:
    store = Path(tmp)
    exact = store / "AfxHookSource2.dll" / "6ACF372Ff00000"
    exact.mkdir(parents=True)
    (exact / "AfxHookSource2.dll").write_bytes(b"MZ")
    # A full key must match exactly; a different image size is a different build.
    assert rs.find_image(store, "AfxHookSource2.dll", "6ACF372Ff00000", 0x6ACF372F) == exact / "AfxHookSource2.dll"
    assert rs.find_image(store, "AfxHookSource2.dll", "6ACF372Fe00000", 0x6ACF372F) is None
    # A bare stamp resolves when exactly one archived build has it...
    assert rs.find_image(store, "AfxHookSource2.dll", None, 0x6ACF372F) == exact / "AfxHookSource2.dll"
    # ...and refuses to guess between two.
    other = store / "AfxHookSource2.dll" / "6ACF372Fe00000"
    other.mkdir()
    (other / "AfxHookSource2.dll").write_bytes(b"MZ")
    assert rs.find_image(store, "AfxHookSource2.dll", None, 0x6ACF372F) is None
    # No archive for the build: a reason, not a fabricated stack.
    stack, first, reason = rs.symbolize(REPORT.replace("6ACF372Ff00000", "11111111f00000"), store)
    assert stack == "" and first is None and "no archived symbols" in reason

# A minidump's exception, located in its module list (a minimal hand-built dump).
import struct
def fake_dump(address, with_exception=True):
    name = "C:\\x\\AfxHookSource2.dll".encode("utf-16-le")
    header = struct.pack("<4sIII", b"MDMP", 0, 3 if with_exception else 2, 32)
    header += b"\0" * (32 - len(header))
    directory_size = 12 * (3 if with_exception else 2)
    threads_rva = 32 + directory_size
    modules_rva = threads_rva + 4
    name_rva = modules_rva + 4 + 108
    exception_rva = name_rva + 4 + len(name)
    body = struct.pack("<I", 7)  # thread list: 7 threads (entries not needed here)
    body += struct.pack("<I", 1) + struct.pack("<QIIII", 0x7FF600000000, 0x600000, 0, 0, name_rva) + b"\0" * (108 - 24)
    body += struct.pack("<I", len(name)) + name
    body += struct.pack("<IIIIQQ", 42, 0, 0xC0000005, 0, 0, address)
    entries = struct.pack("<III", 3, 4, threads_rva) + struct.pack("<III", 4, 112, modules_rva)
    if with_exception:
        entries += struct.pack("<III", 6, 168, exception_rva)
    return header + entries + body

assert rs.dump_exception(fake_dump(0x7FF6004E9557)) == (0xC0000005, 42, 0x7FF6004E9557, "AfxHookSource2.dll", 0x7FF600000000, 7)
assert rs.dump_exception(fake_dump(0x10))[3] is None          # outside every module
assert rs.dump_exception(fake_dump(0x10, False)) is None       # no exception stream
assert rs.dump_exception(b"not a dump") is None
line = rs.symbolize_dump(fake_dump(0x7FF6004E9557), REPORT.replace("6ACF372Ff00000", "11111111f00000"), tempfile.gettempdir())
assert "AfxHookSource2.dll+0x4e9557" in line and "no archived symbols" in line, line

print("report-symbols-tests: all passed")
