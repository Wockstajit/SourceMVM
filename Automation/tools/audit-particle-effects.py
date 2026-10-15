"""Audits CS2's particle systems for Scene Objects > Assets > Effects and writes the table the hook
uses: which effects can play on their own when placed at one point in the world, and what value
each of their "number" control points needs.

Why: an effect placed through the particle manager only gets the control points we set. Many CS2
effects are not standalone:
  * child-only parts (renderers with no emitter: spawned by a parent system),
  * effects that build on a parent's particles, a player model or bones (burning_character),
  * beams/ropes between two control points (collapse to nothing at one point),
  * one-shots (explosions, bursts): they fire once at spawn and the list showed an empty preview,
  * screen/viewmodel effects.
Those are hidden from the list. And many read a control point as a NUMBER, not a position
(molotov_fire01's burn time is 10 x CP3.x): the hook used to put every control point at the world
position, so whether such an effect showed depended on where on the map it was placed. Each such
read gets a value here: 0 for an emission duration (= emit forever), the middle of a remap's input
range, else 1.

Usage (Python 3.11+):
  py -3 Automation/tools/audit-particle-effects.py
  py -3 Automation/tools/audit-particle-effects.py --decompiled <folder>   (reuse a decompile)

  --cli    Source2Viewer-CLI.exe. Defaults to $SOURCEMVM_VRF_CLI, else a ValveResourceFormat checkout
           next to this repository (VRF is an external tool this repo does not vendor or build).
  --vpk    CS2's game/csgo/pak01_dir.vpk. Defaults to the Steam-resolved install
           (Automation/lib/steam_paths.py; $SOURCEMVM_CS2_DIR overrides).
  --out    The generated header. Defaults to AfxHookSource2/Filmmaker/Props/PropsEffectProfiles.h.
  --report Optional JSON with every effect's verdict and reasons, for inspection.

Re-run after a CS2 update that changes particles, and rebuild the hook.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Automation" / "lib"))
import steam_paths  # noqa: E402

# Source2Viewer-CLI (ValveResourceFormat): a checkout next to this repository, which is the
# documented layout; set SOURCEMVM_VRF_CLI to point elsewhere.
DEFAULT_CLI = os.environ.get("SOURCEMVM_VRF_CLI") or str(
    Path(__file__).resolve().parents[2].parent / "ValveResourceFormat" / "CLI" / "bin" / "Release" / "Source2Viewer-CLI.exe")
DEFAULT_OUT = REPO / "AfxHookSource2" / "Filmmaker" / "Props" / "PropsEffectProfiles.h"

# Must match Props::IsPlaceableEffect (PropsMath.h): the gameplay pipeline is never listed at all.
SKIP = ["/weapons/", "weapon_fx", "muzzle", "tracer", "impact", "blood", "/ui/", "inventory", "/player/",
        "screen", "viewmodel", "_fp.", "_fps", "/dev/", "hud", "bullet", "shell", "/filmmaker/", "sticker",
        "spray", "c4_", "bumpmine", "taser", "healthshot"]

# ---- KV3 text (as Source2Viewer-CLI decompiles it) -------------------------------------------------
TOK = re.compile(r'''\s*(?:
    (?P<ml>"""(?:.|\n)*?""") |
    (?P<str>[A-Za-z_]+:"(?:[^"\\]|\\.)*"|"(?:[^"\\]|\\.)*") |
    (?P<punct>[{}\[\]=,]) |
    (?P<atom>[^\s{}\[\]=,]+)
)''', re.X)


def kv3(text: str):
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    toks = []
    pos = 0
    while True:
        m = TOK.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        if m.group("ml") is not None:
            toks.append(("s", m.group("ml")[3:-3]))
        elif m.group("str") is not None:
            s = m.group("str")
            toks.append(("s", s[s.index('"') + 1:-1]))
        elif m.group("punct") is not None:
            toks.append(("p", m.group("punct")))
        else:
            toks.append(("a", m.group("atom")))
    i = 0

    def value():
        nonlocal i
        t, v = toks[i]
        i += 1
        if t == "p" and v == "{":
            d = {}
            while toks[i] != ("p", "}"):
                k = toks[i][1]
                i += 1
                if toks[i] == ("p", "="):
                    i += 1
                d[k] = value()
                if toks[i] == ("p", ","):
                    i += 1
            i += 1
            return d
        if t == "p" and v == "[":
            a = []
            while toks[i] != ("p", "]"):
                a.append(value())
                if toks[i] == ("p", ","):
                    i += 1
            i += 1
            return a
        if t == "a":
            if v in ("true", "false"):
                return v == "true"
            if v == "null":
                return None
            try:
                return float(v) if any(c in v for c in ".eE") else int(v)
            except ValueError:
                return v
        return v

    return value()


# ---- what an effect needs ------------------------------------------------------------------------------
NONVISUAL = {"C_OP_RenderSound", "C_OP_RenderScreenShake", "C_OP_RenderScreenVelocityRotate", "C_OP_RenderStatusEffect",
             "C_OP_RenderStatusEffectCitadel", "C_OP_RenderClothForce", "C_OP_RenderVRHapticEvent",
             "C_OP_RenderPostProcessing", "C_OP_RenderClientPhysicsImpulse", "C_OP_RenderFlattenGrass"}
BEAMS = {"C_OP_RenderRopes", "C_OP_RenderCables"}
NEEDS_PARENT = {"C_INIT_CreateFromParentParticles", "C_INIT_InitFromParentKilled", "C_INIT_CreateFromPlaneCache"}
NEEDS_MODEL = {"C_INIT_CreateOnModel", "C_INIT_CreateOnModelAtHeight", "C_OP_LockToBone",
               "C_INIT_InitSkinnedPositionFromCPSnapshot", "C_OP_SetControlPointToHand", "C_OP_SetControlPointToHMD"}
# Operators that WRITE control points: what they set is not an outside requirement.
CP_SETTER = re.compile(r"C_OP_Set.*(ControlPoint|CP)|C_OP_SetPerChildControlPoint|C_OP_SetControlPointsToParticle|"
                       r"C_OP_SetParentControlPointsToChildCP|C_OP_SetCPOrientation|C_OP_ControlpointLight")
# Operators that read a control point's components as numbers through an input range.
CP_REMAP = re.compile(r"Remap.*CP|RemapControlPoint|CPtoScalar|CPtoVector|CPVelocity")


def enabled_ops(d):
    for key in ("m_Emitters", "m_Initializers", "m_Operators", "m_Renderers", "m_ForceGenerators", "m_Constraints",
                "m_PreEmissionOperators"):
        for o in d.get(key) or []:
            if isinstance(o, dict) and not o.get("m_bDisableOperator"):
                yield key, o


def literal(p):
    if isinstance(p, dict) and p.get("m_nType") in ("PF_TYPE_LITERAL", "PF_TYPE_PARTICLE_DETAIL_LEVEL"):
        return p.get("m_flLiteralValue")
    if isinstance(p, (int, float)) and not isinstance(p, bool):
        return p
    return None


def mid(a, b, fallback=1.0):
    try:
        return (float(a) + float(b)) / 2
    except (TypeError, ValueError):
        return fallback


class Needs:
    def __init__(self):
        self.values = collections.defaultdict(dict)  # cp -> {component: wanted value}
        self.positions: set[int] = set()
        self.conflict = False

    def want(self, cp, component, value, strong=False):
        comps = (0, 1, 2) if component is None or component < 0 else (component,)
        for c in comps:
            have = self.values[cp].get(c)
            if have is None or strong:
                self.values[cp][c] = float(value)
            elif abs(have - value) > max(1.0, abs(have) * 0.5) and not strong:
                pass  # the first (or a duration's) wish wins; the others still play, just scaled

    def merge(self, other: "Needs"):
        for cp, comps in other.values.items():
            for c, v in comps.items():
                self.values[cp].setdefault(c, v)
        self.positions |= other.positions
        self.conflict |= other.conflict


def scan_params(node, needs: Needs, key=""):
    """Every place an operator reads a control point's components as a number."""
    if isinstance(node, dict):
        t = node.get("m_nType")
        if isinstance(t, str) and "CONTROL_POINT_COMPONENT" in t:
            cp = int(node.get("m_nControlPoint", 0))
            comp = node.get("m_nVectorComponent", 0)
            mtype = node.get("m_nMapType", "")
            if "EmissionDuration" in key:
                needs.want(cp, comp, 0.0, strong=True)  # 0 = emit forever
            elif "REMAP" in str(mtype):
                needs.want(cp, comp, mid(node.get("m_flInput0"), node.get("m_flInput1")))
            else:
                lit = node.get("m_flLiteralValue")
                needs.want(cp, comp, lit if isinstance(lit, (int, float)) and lit not in (0, 0.0) and "DIRECT" in str(mtype) else 1.0)
            return
        if isinstance(t, str) and t.startswith("PVEC_TYPE_CP_VALUE"):
            needs.want(int(node.get("m_nControlPoint", 0)), None, 1.0)
            return
        for k, v in node.items():
            scan_params(v, needs, k)
    elif isinstance(node, list):
        for v in node:
            scan_params(v, needs, key)


POS_KEY = re.compile(r"^m_n(ControlPoint|ControlPointNumber|CP|CPin|CPIn|InputCP|StartControlPointNumber|EndControlPointNumber|"
                     r"ControlPointNumberStart|ControlPointNumberEnd|StartCP|EndCP|CP1|CP2|Point|DesiredCP|HeadLocation)$")


class Audit:
    def __init__(self, root: Path):
        self.root = root
        self.cache: dict[str, dict | None] = {}
        self.memo: dict[tuple, dict] = {}

    def load(self, rel: str):
        rel = rel.replace("\\", "/").lower()
        rel = rel[:-2] if rel.endswith("_c") else rel
        if rel not in self.cache:
            path = self.root / rel
            self.cache[rel] = None
            if path.is_file():
                try:
                    self.cache[rel] = kv3(path.read_text(encoding="utf-8", errors="ignore"))
                except Exception as e:  # noqa: BLE001
                    self.cache[rel] = {"__error": str(e)}
        return self.cache[rel]

    def system(self, rel: str, as_child=False):
        key = (rel.lower(), as_child)
        if key in self.memo:
            return self.memo[key]
        r = {"visual": False, "loop": False, "reasons": set(), "beam": False, "needs": Needs()}
        self.memo[key] = r  # cycle guard
        d = self.load(rel)
        if not isinstance(d, dict) or "__error" in d:
            r["reasons"].add("missing" if d is None else "parse-error")
            return r
        if d.get("m_bScreenSpaceEffect") or d.get("m_bViewModelEffect"):
            r["reasons"].add("screen-effect")
        if d.get("m_nMaxParticles", 1000) == 0:
            r["reasons"].add("no-particles")
        own_visual = own_emit = own_loop = False
        set_cps: set[int] = set()
        reads = Needs()
        for kind, o in enabled_ops(d):
            c = o.get("_class", "")
            if CP_SETTER.search(c):
                for k, v in o.items():
                    if isinstance(v, int) and not isinstance(v, bool) and 0 < v < 64 and re.search(r"CP|ControlPoint", k):
                        set_cps.add(v)
                continue
            if kind == "m_Renderers":
                if c in NONVISUAL:
                    continue
                own_visual = True
                r["beam"] |= c in BEAMS
            if kind == "m_Emitters":
                own_emit = True
                if c == "C_OP_ContinuousEmitter":
                    dur = o.get("m_flEmissionDuration")
                    if literal(o.get("m_flEmitRate")) == 0:
                        r["reasons"].add("emits-nothing")
                    lit = literal(dur)
                    if dur is None or lit in (0, 0.0) or (lit is None and isinstance(dur, dict)):
                        own_loop = True
                elif c != "C_OP_InstantaneousEmitter":
                    own_loop = True
            if c in NEEDS_PARENT and not as_child:
                r["reasons"].add("needs-parent")
            if c in NEEDS_MODEL:
                r["reasons"].add("needs-model")
            if CP_REMAP.search(c):
                cp = o.get("m_nCPInput", o.get("m_nInputCP", o.get("m_nControlPoint")))
                if isinstance(cp, int):
                    comp = o.get("m_nField", -1) if "Vector" not in c else -1
                    lo, hi = o.get("m_flInputMin"), o.get("m_flInputMax")
                    reads.want(cp, comp if isinstance(comp, int) and 0 <= comp <= 2 and "Scalar" in c else None, mid(lo, hi))
                continue
            scan_params(o, reads)
            for k, v in o.items():
                if POS_KEY.match(k) and isinstance(v, int) and not isinstance(v, bool) and 0 < v < 64:
                    reads.positions.add(v)
        reads.values = collections.defaultdict(dict, {cp: comps for cp, comps in reads.values.items() if cp not in set_cps})
        reads.positions -= set_cps
        r["needs"] = reads
        child_visual = child_loop = False
        for ch in d.get("m_Children") or []:
            if not isinstance(ch, dict) or ch.get("m_bDisableChild") or ch.get("m_bEndCap") or not ch.get("m_ChildRef"):
                continue
            cr = self.system(ch["m_ChildRef"], as_child=True)
            if cr["visual"] and not (cr["reasons"] - {"needs-parent"}):
                child_visual = True
                child_loop |= cr["loop"]
                r["needs"].merge(cr["needs"])
                r["beam"] |= cr["beam"]
        r["visual"] = (own_visual and own_emit) or child_visual
        r["loop"] = (own_visual and own_emit and own_loop) or child_loop
        if not r["visual"]:
            r["reasons"].add("child-only" if own_visual and not own_emit else "nothing-visible")
        return r

    def verdict(self, rel: str):
        r = self.system(rel)
        why = sorted(r["reasons"])
        needs: Needs = r["needs"]
        if r["beam"] and needs.positions:
            why.append("beam-between-points")
        both = set(needs.values) & needs.positions
        if both:
            why.append("cp-is-position-and-number")
        if not why and not r["loop"]:
            why.append("one-shot")
        values = {cp: [comps.get(c, 0.0) for c in range(3)] for cp, comps in sorted(needs.values.items()) if 0 < cp < 64}
        # CP0 is the effect's own position: a read of CP0 as a number cannot be served.
        if 0 in needs.values and not why:
            why.append("reads-own-position-as-number")
        return (not why), why, values


def decompile(cli: Path, vpk: Path, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run([str(cli), "-i", str(vpk), "-f", "particles/", "-e", "vpcf_c", "-d", "-o", str(out) + os.sep],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        sys.exit(f"Source2Viewer-CLI exited {proc.returncode}: {proc.stderr[-2000:]}")


def fmt(v: float) -> str:
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default=DEFAULT_CLI)
    ap.add_argument("--vpk", default=None)
    ap.add_argument("--decompiled", default=None, help="A folder already holding particles/...vpcf")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--report", default=None)
    a = ap.parse_args()

    if a.decompiled:
        root = Path(a.decompiled)
    else:
        vpk = Path(a.vpk) if a.vpk else None
        if not vpk:
            dirs = steam_paths.cs2_install_dirs()
            if not dirs:
                sys.exit("CS2 install not found (set SOURCEMVM_CS2_DIR or pass --vpk)")
            vpk = dirs[0] / "game" / "csgo" / "pak01_dir.vpk"
        if not Path(a.cli).is_file():
            sys.exit(f"Source2Viewer-CLI.exe not found at {a.cli} (pass --cli or set SOURCEMVM_VRF_CLI)")
        root = Path(tempfile.mkdtemp(prefix="cs2-vpcf-"))
        print(f"Decompiling particles from {vpk} ...")
        decompile(Path(a.cli), vpk, root)

    effects = sorted(str(p.relative_to(root)).replace("\\", "/") for p in (root / "particles").rglob("*.vpcf"))
    effects = [e for e in effects if not any(s in e.lower() for s in SKIP)]
    audit = Audit(root)
    rows, report = [], []
    counts = collections.Counter()
    for e in effects:
        ok, why, values = audit.verdict(e)
        counts["playable" if ok else (why[0] if why else "?")] += 1
        report.append({"effect": e, "playable": ok, "why": why, "values": values})
        cps = ";".join(f"{cp}:{','.join(fmt(x) for x in v)}" for cp, v in values.items()) if ok else ""
        rows.append((e, ok, cps))

    out = Path(a.out)
    lines = [
        "#pragma once",
        "// GENERATED by Automation/tools/audit-particle-effects.py from CS2's pak01 particles -- do not edit.",
        "// Re-run that script after a CS2 update that changes particles. Every placeable effect it audited:",
        "// whether it plays on its own at one point (else Scene Objects > Assets hides it), and, for one",
        "// that plays, the value each control point it reads as a NUMBER needs (\"cp:x,y,z;...\").",
        "// Effects not in this table (a workshop map's own) are listed and placed as before.",
        "namespace Filmmaker::Props {",
        "struct EffectProfile {",
        "\tconst char* path;",
        "\tbool playable;",
        "\tconst char* values;",
        "};",
        "inline const EffectProfile kEffectProfiles[] = {",
    ]
    for e, ok, cps in rows:
        lines.append(f'\t{{"{e}", {"true" if ok else "false"}, "{cps}"}},')
    lines += ["};", "} // namespace Filmmaker::Props", ""]
    out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    if a.report:
        Path(a.report).write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"{len(effects)} effects: " + ", ".join(f"{k} {v}" for k, v in counts.most_common()))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
