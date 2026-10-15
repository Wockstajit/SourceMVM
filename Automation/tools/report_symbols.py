"""Resolve crash-report stack frames to function, file and line.

A report frame reads "AfxHookSource2.dll+0x4e24e0". Automation/tools/archive-symbols.ps1 keeps the
DLL and PDB of every packaged build in a Microsoft-layout symbol store, keyed by the DLL's link
stamp and image size, which reports carry as "Hook ID" (or ", id <key>" on the build line). Given
that key, Windows' own DbgHelp maps each offset to "Environment::Render + 0x3a  (EnvironmentRender.cpp:1012)".

Builds older than the key fall back to the "linked <UTC>" build line, which names the link stamp but
not the image size; one archived build with that stamp is still unambiguous.

Only frames in archived modules resolve. CS2's modules have no public symbols, so those frames stay
module+offset. Engine-free parsing lives in the pure functions below (tested by
Automation/tests/report-symbols-tests.py); DbgHelp is touched only in Resolver.
"""

import calendar
import ctypes
import os
import re
import time
from ctypes import wintypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOOK_MODULE = "AfxHookSource2.dll"

FRAME_RE = re.compile(r"^\s*#(\d+)\s+([^\s+]+)\+0x([0-9a-fA-F]+)\s*$")
HOOK_ID_RE = re.compile(r"^Hook ID\s*:\s*([^/\s]+)/([0-9A-Fa-f]{9,})\s*$", re.M)
BUILD_ID_RE = re.compile(r"hook built\s*:.*?\bid ([0-9A-Fa-f]{9,})")
LINKED_RE = re.compile(r"hook built\s*:\s*linked (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) UTC")
CREATED_RE = re.compile(r"^Created:\s*(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\s*$", re.M)
REPORT_NAME_TIME_RE = re.compile(r"SourceMVM-[a-z]+-(\d{4}-\d\d-\d\d)_(\d\d)-(\d\d)-(\d\d)")
LATER_SESSION_REASON = ("the crash header has no Hook ID and a later session wrote the report, "
                        "so its build line does not name the crashed build")


def symbol_store():
    """The archive archive-symbols.ps1 writes: SOURCEMVM_SYMBOL_STORE, else Automation/symbols."""
    return Path(os.environ.get("SOURCEMVM_SYMBOL_STORE") or REPO / "Automation" / "symbols")


def parse_frames(text):
    """[(index, module, offset)] from a report's "#NN module+0xOFFSET" stack lines."""
    frames = []
    for line in text.splitlines():
        m = FRAME_RE.match(line)
        if m:
            frames.append((int(m.group(1)), m.group(2), int(m.group(3), 16)))
    return frames


def written_by_later_session(text, report_name):
    """True when the report's file name (the crash time) and its Created line (when it was written)
    disagree: a later session found the crash on disk and wrote the report around it."""
    created = CREATED_RE.search(text)
    crashed = REPORT_NAME_TIME_RE.search(report_name or "")
    if not created or not crashed:
        return False
    parse = lambda s: calendar.timegm(time.strptime(s, "%Y-%m-%d %H:%M:%S"))
    crash_time = parse(f"{crashed.group(1)} {crashed.group(2)}:{crashed.group(3)}:{crashed.group(4)}")
    return abs(parse(created.group(1)) - crash_time) > 120


def hook_build(text, report_name=None):
    """(module, key or None, link stamp or None) for the hook that wrote the report.

    The crash header's Hook ID names the crashed session itself. The build line names the session
    that wrote the report, so it is only a fallback when that session is the crashed one.
    """
    m = HOOK_ID_RE.search(text)
    if m:
        return m.group(1), m.group(2).upper()[:8] + m.group(2)[8:].lower(), int(m.group(2)[:8], 16)
    if written_by_later_session(text, report_name):
        return HOOK_MODULE, None, None
    m = BUILD_ID_RE.search(text)
    if m:
        return HOOK_MODULE, m.group(1).upper()[:8] + m.group(1)[8:].lower(), int(m.group(1)[:8], 16)
    m = LINKED_RE.search(text)
    if m:
        stamp = calendar.timegm(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))
        return HOOK_MODULE, None, stamp
    return HOOK_MODULE, None, None


def find_image(store, module, key, stamp):
    """The archived DLL for that build, or None. A bare stamp must match exactly one archive."""
    folder = Path(store) / module
    if key:
        candidate = folder / key / module
        return candidate if candidate.is_file() else None
    if stamp is None or not folder.is_dir():
        return None
    prefix = f"{stamp:08X}"
    hits = [d / module for d in folder.iterdir() if d.name.upper().startswith(prefix) and (d / module).is_file()]
    return hits[0] if len(hits) == 1 else None


def pdb_identity(image_bytes):
    """(pdb file name, store key "<GUID><age>") from a PE32+ image's CodeView record, or None."""
    import struct
    pe = struct.unpack_from("<I", image_bytes, 0x3C)[0]
    if image_bytes[pe:pe + 4] != b"PE\0\0":
        return None
    sections_count = struct.unpack_from("<H", image_bytes, pe + 6)[0]
    optional = pe + 24
    if struct.unpack_from("<H", image_bytes, optional)[0] != 0x20B:
        return None
    debug_rva, debug_size = struct.unpack_from("<II", image_bytes, optional + 112 + 48)
    sections = optional + struct.unpack_from("<H", image_bytes, pe + 20)[0]

    def to_file(rva):
        for i in range(sections_count):
            s = sections + i * 40
            va, raw_size, raw_ptr = struct.unpack_from("<III", image_bytes, s + 12)
            if va <= rva < va + max(raw_size, 1):
                return rva - va + raw_ptr
        return None

    directory = to_file(debug_rva)
    for entry in range(debug_size // 28 if directory is not None else 0):
        e = directory + entry * 28
        if struct.unpack_from("<I", image_bytes, e + 12)[0] != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        cv = struct.unpack_from("<I", image_bytes, e + 24)[0]
        if image_bytes[cv:cv + 4] != b"RSDS":
            continue
        d1, d2, d3 = struct.unpack_from("<IHH", image_bytes, cv + 4)
        tail = image_bytes[cv + 12:cv + 20].hex().upper()
        age = struct.unpack_from("<I", image_bytes, cv + 20)[0]
        name = image_bytes[cv + 24:image_bytes.index(b"\0", cv + 24)].decode("utf-8", "replace")
        return Path(name.replace("\\", "/")).name, f"{d1:08X}{d2:04X}{d3:04X}{tail}{age:X}"
    return None


class _SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG),
                ("Reserved", ctypes.c_uint64 * 2), ("Index", wintypes.ULONG), ("Size", wintypes.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG),
                ("Tag", wintypes.ULONG), ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG),
                ("Name", wintypes.WCHAR * 1024)]


class _LineInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
                ("FileName", wintypes.LPWSTR), ("Address", ctypes.c_uint64)]


class Resolver:
    """DbgHelp over one archived image, loaded at a fixed fake base so offsets map directly."""

    BASE = 0x10000000

    def __init__(self, image, store):
        self.ok = False
        if os.name != "nt" or image is None:
            return
        self.dbg = ctypes.WinDLL("dbghelp.dll")
        self.process = wintypes.HANDLE(0x5EED)  # any unique value: no real process is inspected
        # UNDNAME | LOAD_LINES | FAIL_CRITICAL_ERRORS | NO_PROMPTS (loaded eagerly: deferred never loaded here)
        self.dbg.SymSetOptions(0x2 | 0x10 | 0x200 | 0x80000)
        # The system dbghelp.dll ships without symsrv.dll, so "srv*<store>" finds nothing. The
        # image's own CodeView record names the PDB folder; search exactly there.
        identity = pdb_identity(Path(image).read_bytes())
        if identity is None:
            return
        pdb_folder = Path(store) / identity[0] / identity[1]
        if not (pdb_folder / identity[0]).is_file():
            return
        self.dbg.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
        if not self.dbg.SymInitializeW(self.process, str(pdb_folder), False):
            return
        self.dbg.SymLoadModuleExW.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                              ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        self.dbg.SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                          ctypes.POINTER(_SymbolInfo)]
        self.dbg.SymGetLineFromAddrW64.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(wintypes.DWORD),
                                                   ctypes.POINTER(_LineInfo)]
        self.ok = bool(self.dbg.SymLoadModuleExW(self.process, None, str(image), None, self.BASE, 0, None, 0))

    def close(self):
        if getattr(self, "dbg", None) is not None:
            self.dbg.SymCleanup(self.process)

    def describe(self, offset):
        """"Function + 0x1a  (File.cpp:123)", or None when no symbol covers the offset."""
        if not self.ok:
            return None
        address = self.BASE + offset
        info = _SymbolInfo()
        info.SizeOfStruct = 88  # sizeof(SYMBOL_INFOW): the fixed part, not the name buffer
        info.MaxNameLen = 1024
        displacement = ctypes.c_uint64(0)
        if not self.dbg.SymFromAddrW(self.process, address, ctypes.byref(displacement), ctypes.byref(info)):
            return None
        text = f"{info.Name} + 0x{displacement.value:x}"
        line = _LineInfo()
        line.SizeOfStruct = ctypes.sizeof(_LineInfo)
        column = wintypes.DWORD(0)
        if self.dbg.SymGetLineFromAddrW64(self.process, address, ctypes.byref(column), ctypes.byref(line)):
            text += f"  ({Path(line.FileName).name}:{line.LineNumber})"
        return text


def dump_exception(data):
    """(exception code, thread id, address, module name, module base, thread count) from a minidump's
    exception and module streams, or None when it has no exception (an abort or a test)."""
    import struct
    if len(data) < 32 or data[:4] != b"MDMP":
        return None
    _, _, count, directory = struct.unpack_from("<4sIII", data, 0)
    streams = {}
    for i in range(count):
        kind, size, rva = struct.unpack_from("<III", data, directory + 12 * i)
        streams[kind] = rva
    if 6 not in streams or 4 not in streams:  # ExceptionStream, ModuleListStream
        return None
    thread, _, code, _, _, address = struct.unpack_from("<IIIIQQ", data, streams[6])
    threads = struct.unpack_from("<I", data, streams[3])[0] if 3 in streams else 0
    modules = streams[4]
    for i in range(struct.unpack_from("<I", data, modules)[0]):
        base, size, _, _, name_rva = struct.unpack_from("<QIIII", data, modules + 4 + 108 * i)  # MINIDUMP_MODULE
        if base <= address < base + size:
            length = struct.unpack_from("<I", data, name_rva)[0]
            name = data[name_rva + 4:name_rva + 4 + length].decode("utf-16-le").replace("\\", "/").split("/")[-1]
            return code, thread, address, name, base, threads
    return code, thread, address, None, None, threads


def symbolize_dump(data, report_text, store=None, report_name=None):
    """One line naming where the minidump's exception happened, resolved to function and line when
    the faulting module is the archived hook build the report names."""
    found = dump_exception(data)
    if found is None:
        return "Minidump: no exception record (an abort or a test crash)"
    code, thread, address, module, base, threads = found
    line = f"Minidump: exception 0x{code:08X} on thread {thread} ({threads} threads captured)"
    if module is None:
        return line + f" at 0x{address:x}, outside every loaded module"
    line += f" at {module}+0x{address - base:x}"
    hook, key, stamp = hook_build(report_text, report_name)
    if module.lower() != hook.lower():
        return line
    image = find_image(Path(store or symbol_store()), hook, key, stamp)
    if image is None:
        return line + "  (no archived symbols for this build)"
    resolver = Resolver(image, Path(store or symbol_store()))
    try:
        named = resolver.describe(address - base) if resolver.ok else None
    finally:
        resolver.close()
    return line + (f"  {named}" if named else "")


def symbolize(text, store=None, report_name=None):
    """(resolved stack text, first own frame description or None, why it could not resolve or None).
    report_name is the report's file name, which carries the crash time."""
    store = Path(store or symbol_store())
    frames = parse_frames(text)
    if not frames:
        return "", None, "the report has no stack"
    module, key, stamp = hook_build(text, report_name)
    if key is None and stamp is None and written_by_later_session(text, report_name):
        return "", None, LATER_SESSION_REASON
    image = find_image(store, module, key, stamp)
    if image is None:
        build = key or (f"link stamp {stamp:08X}" if stamp is not None else "an unknown build")
        return "", None, f"no archived symbols for {module} {build} in {store}"
    resolver = Resolver(image, store)
    try:
        if not resolver.ok:
            return "", None, f"DbgHelp could not load {image}"
        lines, first_own = [], None
        for index, frame_module, offset in frames:
            named = resolver.describe(offset) if frame_module.lower() == module.lower() else None
            lines.append(f"  #{index:02d} {frame_module}+0x{offset:x}" + (f"  {named}" if named else ""))
            if named and first_own is None:
                first_own = f"#{index:02d} {named}"
        return "\n".join(lines) + "\n", first_own, None
    finally:
        resolver.close()
