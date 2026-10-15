"""Wall-only impact closures with slightly larger local smoke sprites."""

from __future__ import annotations

import re
from pathlib import Path

from postprocess_common import block_span, collect_closure, iter_renderer_blocks

WALL_SURFACES = frozenset({"brick", "concrete", "plaster", "sheetrock", "tile", "rock"})
WALL_PUFF_SCALE = 1.25
RESOURCE_RE = re.compile(r'resource:"([^"]+)"')
SMOKE_RE = re.compile(r"smoke|dust|puff|steam|vapor", re.IGNORECASE)
OTHER_EFFECT_RE = re.compile(
    r"trail|tracer|burst|spark|flash|flame|glow|flare|shrapnel|debris|fleck", re.IGNORECASE
)
RADIUS_RE = re.compile(
    r"(\bm_flRadius(?:Min|Max)\s*=\s*)([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
)


def enlarge_local_puffs(text: str, stem: str) -> str:
    """Scale smoke-only world-space sprites, leaving ribbons and mixed systems alone."""
    # This suffix describes the closure, not the renderer: its surviving puffs
    # must receive the same size increase as those in the normal closure.
    stem = stem.removesuffix("_nosmoketrails")
    if OTHER_EFFECT_RE.search(stem) or re.search(r"m_bScreenSpaceEffect\s*=\s*(?:true|1)\b", text):
        return text
    renderers = [text[start:end] for start, end in iter_renderer_blocks(text)]
    if not renderers:
        return text
    for renderer in renderers:
        if not re.search(r'_class\s*=\s*"C_OP_RenderSprites"', renderer):
            return text
        resources = " ".join(RESOURCE_RE.findall(renderer))
        if OTHER_EFFECT_RE.search(resources) or not SMOKE_RE.search(stem + " " + resources):
            return text
    # Radius interpolation remains unchanged: scaling both would compound the increase.
    for match in reversed(list(re.finditer(r'_class\s*=\s*"C_INIT_RandomRadius"', text))):
        start, end = block_span(text, match.start())
        block = RADIUS_RE.sub(
            lambda radius: radius[1] + format(float(radius[2]) * WALL_PUFF_SCALE, ".8g"),
            text[start:end],
        )
        text = text[:start] + block + text[end:]
    return text


class WallSmokeVariants:
    """Fork donor closures without changing children shared with carpet, snow, etc."""

    def __init__(self, root: Path, dry_run: bool = False):
        self.root = root
        self.dry_run = dry_run
        self.targets: dict[str, str] = {}

    def target_for(self, target: str, surface: str) -> str:
        if surface.removesuffix("_cheap") not in WALL_SURFACES:
            return target
        if target in self.targets:
            return self.targets[target]
        directory = target.rsplit("/", 1)[0] + "/"
        sources = {}
        for path in sorted(collect_closure(self.root, [target])):
            resource = path.relative_to(self.root).as_posix()
            if resource.startswith(directory) and "/wall_smoke/" not in resource:
                sources[resource] = path.read_text(encoding="utf-8")
        enlarged = {
            resource: enlarge_local_puffs(text, Path(resource).stem)
            for resource, text in sources.items()
        }
        if not any(enlarged[resource] != text for resource, text in sources.items()):
            self.targets[target] = target
            return target
        names = {
            resource: directory + "wall_smoke/" + resource[len(directory):]
            for resource in sources
        }
        for resource, text in enlarged.items():
            text = RESOURCE_RE.sub(
                lambda ref: 'resource:"' + names.get(ref[1], ref[1]) + '"', text
            )
            path = self.root / names[resource]
            if not self.dry_run and (not path.is_file() or path.read_text(encoding="utf-8") != text):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
        self.targets[target] = names[target]
        return names[target]
