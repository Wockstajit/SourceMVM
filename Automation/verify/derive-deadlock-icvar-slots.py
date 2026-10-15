"""Derive Deadlock's ICvar (class CCvar) vtable slot indices by cross-matching against CS2's.

WHY THIS EXISTS
---------------
Deadlock's client hands back an interface for the version string "VEngineCvar007" -- byte-identical
to CS2's -- so the port assumed the interface matched. It does not. Calling ICvar::GetCmd through
CS2's assumed layout faults, which means RegisterConCommand is at the wrong slot too. It does not
fault; it calls some other virtual and registers nothing. The consequence is that NO console command
this DLL declares has ever existed inside Deadlock -- `mirv_deadlock` included. The whole tool has
only ever been reachable over netcon, which dispatches by string and never asks the engine.

A matching interface version string is not a matching vtable.

THE METHOD
----------
The same shape as derive-deadlock-preui-hook.py: anchor on something the compiler could not have
renamed, then cross-match FUNCTION BODIES between the two binaries rather than trusting an index.

  1. Both games ship tier0.dll, and the interface is implemented by `class CCvar` in both -- read
     off MSVC RTTI from the live object (`mirv_deadlock cvarprobe vtable` prints `.?AVCCvar@@`).
  2. Find that class's type descriptor in each binary, then the complete object locator that names
     it, then the vtable that points at that locator. That gives an ordered list of slot RVAs per
     game without guessing where the table starts.
  3. For each CS2 slot, take the function's opening bytes and find the Deadlock slot whose function
     matches best. Identical builds of the same engine differ in offsets, not in shape, so this is
     stable where an index is not.
  4. Report the mapping for the four slots the hook actually calls, and REFUSE any that is ambiguous
     -- a slot guessed wrong here is silently catastrophic in exactly the way this script exists to
     prevent.

CS2's documented layout lives in Deps/release/prop/cs2/sdk_src/public/icvar.h.

USAGE
-----
    py -3 Automation/verify/derive-deadlock-icvar-slots.py
    py -3 Automation/verify/derive-deadlock-icvar-slots.py --cs2 <path\\to\\tier0.dll> --deadlock <path>

Exit code 0 when every wanted slot resolved unambiguously, 1 otherwise.
"""

import argparse
import os
import struct
import subprocess
import sys

# The slots the hook calls, by their CS2 index and the name icvar.h gives them.
WANTED_CS2_SLOTS = {
    41: "GetCvar",
    42: "RegisterConCommand",
    43: "UnregisterConCommand",
    44: "GetCmd",
}

RTTI_NAME = b".?AVCCvar@@"


class PE:
    """Just enough PE to turn RVAs into file offsets and read sections."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE image")
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off:pe_off + 4] != b"PE\0\0":
            raise ValueError(f"{path}: no PE signature")
        coff = pe_off + 4
        n_sections = struct.unpack_from("<H", self.data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", self.data, coff + 16)[0]
        opt = coff + 20
        magic = struct.unpack_from("<H", self.data, opt)[0]
        if magic != 0x20B:
            raise ValueError(f"{path}: not a 64-bit image")
        self.image_base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        sec = opt + opt_size
        self.sections = []
        for i in range(n_sections):
            off = sec + i * 40
            name = self.data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", self.data, off + 8)
            chars = struct.unpack_from("<I", self.data, off + 36)[0]
            self.sections.append({
                "name": name, "vaddr": vaddr, "vsize": vsize,
                "raddr": raddr, "rsize": rsize, "exec": bool(chars & 0x20000000),
            })

    def rva_to_off(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
                delta = rva - s["vaddr"]
                if delta < s["rsize"]:
                    return s["raddr"] + delta
                return None
        return None

    def is_exec_rva(self, rva):
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
                return s["exec"]
        return False

    def read(self, rva, n):
        off = self.rva_to_off(rva)
        if off is None:
            return None
        return self.data[off:off + n]

    def find_all(self, needle):
        out, i = [], self.data.find(needle)
        while i != -1:
            out.append(i)
            i = self.data.find(needle, i + 1)
        return out

    def off_to_rva(self, off):
        for s in self.sections:
            if s["raddr"] <= off < s["raddr"] + s["rsize"]:
                return s["vaddr"] + (off - s["raddr"])
        return None


def find_cvar_vtable(pe):
    """Find class CCvar's vtable RVA and its slot RVAs.

    RTTI on x64 is image-relative throughout, which is what makes this tractable without a
    disassembler: the type descriptor's address appears as a 32-bit RVA inside the complete object
    locator, and the locator's address appears as a full pointer one slot BEFORE the vtable.
    """
    hits = pe.find_all(RTTI_NAME)
    if not hits:
        raise RuntimeError(f"{os.path.basename(pe.path)}: no '{RTTI_NAME.decode()}' type descriptor")

    # The name sits at +16 in a TypeDescriptor, so the descriptor starts 16 bytes earlier.
    candidates = []
    for name_off in hits:
        td_off = name_off - 16
        td_rva = pe.off_to_rva(td_off)
        if td_rva is None:
            continue
        # Complete object locators reference the descriptor by image-relative address.
        for col_off in pe.find_all(struct.pack("<I", td_rva)):
            col_rva = pe.off_to_rva(col_off - 12)   # pTypeDescriptor is at +12
            if col_rva is None:
                continue
            # ...and the vtable is preceded by a full pointer to the locator.
            ptr = struct.pack("<Q", pe.image_base + col_rva)
            for p_off in pe.find_all(ptr):
                vt_rva = pe.off_to_rva(p_off + 8)
                if vt_rva is not None:
                    candidates.append(vt_rva)

    if not candidates:
        raise RuntimeError(f"{os.path.basename(pe.path)}: found CCvar RTTI but no vtable referencing it")

    # A class can appear in several locators (multiple inheritance); take the table with the most
    # consecutive executable entries, which is the primary one.
    best, best_slots = None, []
    for vt in sorted(set(candidates)):
        slots = read_vtable(pe, vt)
        if len(slots) > len(best_slots):
            best, best_slots = vt, slots
    return best, best_slots


def read_vtable(pe, vt_rva, limit=256):
    slots = []
    for i in range(limit):
        raw = pe.read(vt_rva + i * 8, 8)
        if raw is None or len(raw) < 8:
            break
        va = struct.unpack("<Q", raw)[0]
        if va == 0:
            break
        rva = va - pe.image_base
        if rva <= 0 or not pe.is_exec_rva(rva):
            break
        slots.append(rva)
    return slots


def body(pe, rva, n=512):
    """More than a prologue on purpose.

    GetCvar and GetCmd are the same routine over two different tables -- index, bounds-check, return
    a pointer -- so their first sixty-odd bytes are identical in both games and a short comparison
    cannot tell them apart. It is further in, where each loads its own table's base, that they
    diverge."""
    b = pe.read(rva, n)
    return b if b else b""


def score(a, b):
    """How alike are two function openings? Counts the leading identical bytes, then total equal
    bytes, so a prologue match dominates and incidental agreement only breaks ties."""
    if not a or not b:
        return (0, 0)
    lead = 0
    for x, y in zip(a, b):
        if x != y:
            break
        lead += 1
    same = sum(1 for x, y in zip(a, b) if x == y)
    return (lead, same)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cs2")
    ap.add_argument("--deadlock")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    def resolve(kind, explicit, script):
        if explicit:
            return explicit
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        ps = os.path.join(root, "Automation", "lib", script)
        out = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps],
                             capture_output=True, text=True)
        d = out.stdout.strip()
        if not d:
            sys.exit(f"could not resolve the {kind} install; pass --{kind}")
        return os.path.join(d, "game", "bin", "win64", "tier0.dll")

    cs2_path = resolve("cs2", args.cs2, "Resolve-Cs2Dir.ps1")
    dl_path = resolve("deadlock", args.deadlock, "Resolve-DeadlockDir.ps1")

    print(f"CS2      tier0: {cs2_path}")
    print(f"Deadlock tier0: {dl_path}")

    cs2, dl = PE(cs2_path), PE(dl_path)
    cs2_vt, cs2_slots = find_cvar_vtable(cs2)
    dl_vt, dl_slots = find_cvar_vtable(dl)

    print(f"\nCCvar vtable: CS2 tier0+0x{cs2_vt:x} ({len(cs2_slots)} slots), "
          f"Deadlock tier0+0x{dl_vt:x} ({len(dl_slots)} slots)")

    dl_bodies = [body(dl, r) for r in dl_slots]

    failures = 0
    print("\n  CS2 slot  name                    -> Deadlock slot   confidence")
    print("  " + "-" * 70)
    results = {}
    for cs2_index in sorted(WANTED_CS2_SLOTS):
        name = WANTED_CS2_SLOTS[cs2_index]
        if cs2_index >= len(cs2_slots):
            print(f"  {cs2_index:>8}  {name:<22} -> CS2 table is shorter than that")
            failures += 1
            continue
        target = body(cs2, cs2_slots[cs2_index])
        ranked = sorted(((score(target, b), i) for i, b in enumerate(dl_bodies)), reverse=True)
        (lead, same), best = ranked[0]
        (lead2, same2), second = ranked[1] if len(ranked) > 1 else ((0, 0), -1)

        # A match has to be both good and CLEARLY better than the runner-up. An ambiguous answer is
        # worse than no answer: this is the exact failure the pre-UI derivation warns about, where a
        # wrong slot calls an unrelated virtual and nothing says so.
        ok = lead >= 8 and (lead - lead2) >= 4
        results[name] = (best if ok else None, (lead, same), (second, lead2), ranked)
        verdict = "OK" if ok else "tied -- deciding by order below"
        print(f"  {cs2_index:>8}  {name:<22} -> {best:<15} lead={lead} (next {second}: lead={lead2})  {verdict}")

    # ORDER IS EVIDENCE. A compiler emits a class's virtuals in declaration order, and a later build
    # that adds methods inserts them -- it does not shuffle what was already there. So the mapping
    # from CS2 slots to Deadlock slots must be strictly increasing, and any pair that byte-matching
    # left tied can be decided by the neighbours that did resolve, provided the answer is one of the
    # candidates matching cleanly rather than a free choice.
    anchors = {i: results[WANTED_CS2_SLOTS[i]][0]
               for i in sorted(WANTED_CS2_SLOTS) if results[WANTED_CS2_SLOTS[i]][0] is not None}
    if anchors:
        shifts = {i - s for i, s in ((k, v) for k, v in anchors.items())}
        offsets = {v - k for k, v in anchors.items()}
        if len(offsets) == 1:
            shift = offsets.pop()
            print(f"\n  The slots that resolved are all shifted by exactly +{shift}; "
                  f"using that to settle the ties, and only where the tie's own candidates agree.")
            for cs2_index in sorted(WANTED_CS2_SLOTS):
                name = WANTED_CS2_SLOTS[cs2_index]
                chosen, _, _, ranked = results[name]
                if chosen is not None:
                    continue
                expected = cs2_index + shift
                tied = [i for (sc, i) in ranked if sc == ranked[0][0]]
                if expected in tied:
                    results[name] = (expected, ranked[0][0], None, ranked)
                    print(f"    {name:<22} -> {expected}  (order says +{shift}; "
                          f"it is one of the tied best matches {tied})")
                else:
                    print(f"    {name:<22} -> UNRESOLVED: order says {expected}, "
                          f"but the tied best matches are {tied}")

    for cs2_index in sorted(WANTED_CS2_SLOTS):
        if results[WANTED_CS2_SLOTS[cs2_index]][0] is None:
            failures += 1

    # A mapping that is not strictly increasing is not a vtable mapping, whatever the bytes said.
    resolved = [(i, results[WANTED_CS2_SLOTS[i]][0]) for i in sorted(WANTED_CS2_SLOTS)
                if results[WANTED_CS2_SLOTS[i]][0] is not None]
    for (ia, sa), (ib, sb) in zip(resolved, resolved[1:]):
        if sb <= sa:
            print(f"\n  ORDER VIOLATION: CS2 {ia}->{sa} but CS2 {ib}->{sb}. Refusing the whole mapping.")
            failures += 1

    if args.verbose:
        print("\nDeadlock slots:")
        for i, r in enumerate(dl_slots):
            print(f"  {i:>3}  tier0+0x{r:08x}  {dl_bodies[i][:12].hex(' ')}")

    if failures:
        print(f"\n{failures} slot(s) did not resolve cleanly. Do NOT wire a guess: a wrong slot here "
              f"calls an unrelated virtual and fails silently.")
        return 1

    print("\nAll wanted slots resolved. These belong in AfxHookSource2/Deadlock/DeadlockIcvarSlots.h;")
    print("re-run after any Deadlock update -- a shifted slot fails silently.")
    for i in sorted(WANTED_CS2_SLOTS):
        name = WANTED_CS2_SLOTS[i]
        slot, sc, _, _ = results[name]
        print(f"  k{name:<22} = {slot:<3}  (CS2 {i}; {sc[1]} of 512 bytes identical to CS2's)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
