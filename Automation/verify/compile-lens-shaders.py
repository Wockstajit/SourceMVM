#!/usr/bin/env python3
"""Compile every lens shader entry with fxc.exe, the same way the hook does at device start.

The lens module compiles its HLSL at runtime (AfxHookSource2/Filmmaker/Lens/LensFxShaders.h, joined
and compiled by LensFx.cpp ShaderSource() / ShaderRequests()), so a typo or a cbuffer mistake only
shows up in game. This script reproduces that offline in a few seconds:

  1. reads the raw-string constants kShaderCommon, kShaderDof, kShaderFlare, kShaderComposite from
     LensFxShaders.h (adjacent literals inside one constant are joined, like the compiler does);
  2. concatenates them in the order ShaderSource() uses (parsed from LensFx.cpp, so a reorder or a
     new fragment there is followed automatically);
  3. reads ShaderRequests() from LensFx.cpp (entry, profile, DEPTH_MS, COLOR_MS), expanding its
     small `for` loops;
  4. runs fxc.exe on each request with the same flags the hook uses (strictness, optimisation 3)
     and prints one PASS / FAIL line per entry. FAIL lines are followed by the compiler output.

Usage (from anywhere):
  python Automation/verify/compile-lens-shaders.py            # all entries
  python Automation/verify/compile-lens-shaders.py --entry GatherPS --entry CocPS
  python Automation/verify/compile-lens-shaders.py --dump-source out.hlsl   # keep the joined source
  python Automation/verify/compile-lens-shaders.py --fxc "C:/path/to/fxc.exe"

Exit code: 0 when every entry compiled, 1 when any failed, 2 when the sources or fxc could not be
found/parsed. fxc is looked up in the Windows Kits 10 bin folders (newest x64 first) and on PATH.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LENS_DIR = os.path.join(REPO, "AfxHookSource2", "Filmmaker", "Lens")


def find_fxc(explicit):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    kits = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    cands = glob.glob(os.path.join(kits, "Windows Kits", "10", "bin", "*", "x64", "fxc.exe"))

    def ver(p):
        m = re.search(r"bin[\\/](\d+(?:\.\d+)*)[\\/]x64", p)
        return tuple(int(x) for x in m.group(1).split(".")) if m else (0,)

    cands.sort(key=ver, reverse=True)
    if cands:
        return cands[0]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, "fxc.exe")
        if os.path.isfile(p):
            return p
    return None


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def raw_constants(header_text):
    """name -> joined raw-string contents for every `constexpr char kShaderX[] = R"HLSL(...)HLSL" ...;`."""
    out = {}
    for m in re.finditer(r"(kShader\w+)\s*\[\s*\]\s*=\s*", header_text):
        pos = m.end()
        parts = []
        while True:
            lit = re.compile(r'\s*R"(\w*)\(').match(header_text, pos)
            if not lit:
                break
            delim = lit.group(1)
            end = header_text.find(")" + delim + '"', lit.end())
            if end < 0:
                raise ValueError("unterminated raw string in " + m.group(1))
            parts.append(header_text[lit.end():end])
            pos = end + len(delim) + 2
        out[m.group(1)] = "".join(parts)
    return out


def source_order(cpp_text):
    m = re.search(r"ShaderSource\s*\(\s*\)\s*\{(.*?)\n\}", cpp_text, re.S)
    if not m:
        raise ValueError("ShaderSource() not found in LensFx.cpp")
    names = re.findall(r"\b(kShader\w+)\b", m.group(1))
    if not names:
        raise ValueError("ShaderSource() does not reference any kShader constant")
    return names


def match_brace(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces in ShaderRequests()")


def shader_requests(cpp_text):
    m = re.search(r"ShaderRequests\s*\(\s*\)\s*\{", cpp_text)
    if not m:
        raise ValueError("ShaderRequests() not found in LensFx.cpp")
    body_start = m.end() - 1
    body = cpp_text[body_start + 1:match_brace(cpp_text, body_start)]
    reqs = []

    def emit(entry, profile, dms, cms):
        reqs.append((entry, profile, int(dms), int(cms)))

    def eval_arg(a, env):
        a = a.strip()
        if a.startswith('"'):
            return a.strip('"')
        return env.get(a, a)

    def run(text, env):
        i = 0
        while i < len(text):
            fm = re.compile(r"for\s*\(([^)]*(?:\{[^}]*\}[^)]*)?)\)\s*").match(text, i)
            if not fm:
                fm = re.compile(r"for\s*\(").match(text, i)
                if fm:
                    # header with a braced initializer list: find the matching close paren manually
                    depth, j = 0, fm.end() - 1
                    while j < len(text):
                        depth += text[j] == "("
                        depth -= text[j] == ")"
                        if depth == 0:
                            break
                        j += 1
                    header = text[fm.end():j]
                    k = j + 1
                    while k < len(text) and text[k].isspace():
                        k += 1
                    fm = ("HDR", header, k)
                else:
                    fm = None
            else:
                fm = ("HDR", fm.group(1), fm.end())
            if fm:
                _, header, k = fm
                if text[k] == "{":
                    e = match_brace(text, k)
                    stmt, nxt = text[k + 1:e], e + 1
                else:
                    e = text.index(";", k)
                    stmt, nxt = text[k:e + 1], e + 1
                rng = re.match(r"\s*int\s+(\w+)\s*=\s*(\d+)\s*;\s*\1\s*<\s*(\d+)", header)
                lst = re.match(r"\s*const\s+char\s*\*\s*(\w+)\s*:\s*\{(.*)\}\s*$", header, re.S)
                if rng:
                    for v in range(int(rng.group(2)), int(rng.group(3))):
                        run(stmt, dict(env, **{rng.group(1): str(v)}))
                elif lst:
                    for item in re.findall(r'"([^"]*)"', lst.group(2)):
                        run(stmt, dict(env, **{lst.group(1): item}))
                else:
                    raise ValueError("unsupported for-header in ShaderRequests(): " + header)
                i = nxt
                continue
            pm = re.compile(r"push_back\s*\(\s*\{([^}]*)\}\s*\)").match(text, i)
            if pm:
                a = [eval_arg(x, env) for x in pm.group(1).split(",")]
                emit(*a)
                i = pm.end()
                continue
            im = re.compile(r"std::vector<Request>\s+\w+\s*\{(.*?)\}\s*;", re.S).match(text, i)
            if im:
                for row in re.findall(r"\{([^{}]*)\}", im.group(1)):
                    a = [eval_arg(x, env) for x in row.split(",")]
                    emit(*a)
                i = im.end()
                continue
            i += 1

    run(body, {})
    return reqs


def compile_one(fxc, src_path, req, tmp):
    entry, profile, dms, cms = req
    out = os.path.join(tmp, "%s_%d%d.cso" % (entry, dms, cms))
    cmd = [fxc, "/nologo", "/T", profile, "/E", entry, "/O3", "/Ges",
           "/D", "DEPTH_MS=%d" % dms, "/D", "COLOR_MS=%d" % cms, "/Fo", out, src_path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return req, p.returncode == 0, (p.stdout + p.stderr).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fxc", help="path to fxc.exe (default: newest Windows Kits 10 x64, then PATH)")
    ap.add_argument("--entry", action="append", help="only compile this entry point (repeatable)")
    ap.add_argument("--dump-source", metavar="FILE", help="write the joined HLSL here and keep going")
    args = ap.parse_args()

    fxc = find_fxc(args.fxc)
    if not fxc:
        print("ERROR: fxc.exe not found (install the Windows SDK or pass --fxc)")
        return 2
    try:
        consts = raw_constants(read(os.path.join(LENS_DIR, "LensFxShaders.h")))
        cpp = read(os.path.join(LENS_DIR, "LensFx.cpp"))
        order = source_order(cpp)
        missing = [n for n in order if n not in consts]
        if missing:
            raise ValueError("ShaderSource() uses constants not found in LensFxShaders.h: " + ", ".join(missing))
        source = "".join(consts[n] for n in order)
        reqs = shader_requests(cpp)
    except (OSError, ValueError) as e:
        print("ERROR: " + str(e))
        return 2
    if not reqs:
        print("ERROR: no shader requests parsed from LensFx.cpp")
        return 2
    if args.entry:
        reqs = [r for r in reqs if r[0] in args.entry]
        if not reqs:
            print("ERROR: none of the requested entries exist")
            return 2

    print("fxc: %s" % fxc)
    print("source: %s (%d bytes) from %s" % ("+".join(order), len(source), os.path.relpath(LENS_DIR, REPO)))
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        src_path = args.dump_source or os.path.join(tmp, "LensShaders.hlsl")
        with open(src_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(source)
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            results = list(pool.map(lambda r: compile_one(fxc, src_path, r, tmp), reqs))
        for (entry, profile, dms, cms), ok, log in results:
            tag = "%s %s DEPTH_MS=%d COLOR_MS=%d" % (entry, profile, dms, cms)
            if ok:
                print("PASS  " + tag)
            else:
                failed += 1
                print("FAIL  " + tag)
                for line in log.splitlines():
                    print("      " + line)
    print("%d/%d entries compiled" % (len(results) - failed, len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
