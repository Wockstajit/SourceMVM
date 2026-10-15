"""Survey shipped CS2 agent facial data with Source2Viewer-CLI.

Usage: py -3 Automation/tools/survey_cs2_agent_faces.py <pak01_dir.vpk> <Source2Viewer-CLI.exe>
Prints JSON; it does not extract or modify game assets.
"""

import json
import re
import subprocess
import sys


def cli(exe, vpk, *args):
    result = subprocess.run(
        [exe, "-i", vpk, *args], capture_output=True, text=True, check=True,
        encoding="utf-8", errors="replace",
    )
    return result.stdout


def array_block(text, field):
    match = re.search(r"(?m)^\s*" + re.escape(field) + r"\s*=\s*\n\s*\[", text)
    if not match:
        return ""
    start = match.end()
    depth = 1
    for pos in range(start, len(text)):
        if text[pos] == "[":
            depth += 1
        elif text[pos] == "]":
            depth -= 1
            if depth == 0:
                return text[start:pos]
    return ""


def names(text, field):
    return re.findall(r'"([^"\r\n]+)"', array_block(text, field))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    vpk, exe = sys.argv[1:]
    listing = cli(exe, vpk, "--vpk_list", "-f", "agents/models/", "-e", "vmdl_c")
    paths = [line.split(" CRC:", 1)[0] for line in listing.splitlines()
             if re.fullmatch(r"agents/models/(?:ctm|tm)_[^/]+/(?:ctm|tm)_[^/]+\.vmdl_c CRC:.*", line)]
    result = []
    for path in paths:
        content = cli(exe, vpk, "-f", path, "-a")
        controllers = re.findall(r'm_szName\s*=\s*"([^"\r\n]+)"', array_block(content, "m_FlexControllers"))
        ranges = re.findall(r'\bmin\s*=\s*([-\d.]+)\s*\n\s*max\s*=\s*([-\d.]+)',
                            array_block(content, "m_FlexControllers"))
        morphs = re.findall(r'm_name\s*=\s*"([^"\r\n]+)"', array_block(content, "m_morphDatas"))
        bones = names(content, "m_boneName")
        graph_refs = re.findall(r'm_hGraph\s*=\s*resource:"([^"\r\n]+)"', content)
        constraints = re.findall(r'_class\s*=\s*"(CBoneConstraintDotToMorph|[^"\r\n]*MorphConstraint[^"\r\n]*)"', content)
        result.append({"model": path, "controllers": controllers, "ranges": ranges,
                       "morphs": morphs, "facialBones": [b for b in bones if re.search(
                           r"eye|jaw|mouth|lip|brow|cheek|tongue|head", b, re.I)],
                       "morphConstraints": constraints, "animGraphs": sorted(set(graph_refs)),
                       "hasMorphAtlas": "thirdperson_body_vmorf.vtex" in content})
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
