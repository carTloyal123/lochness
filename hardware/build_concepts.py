"""Export each lock-unit concept: STEP assemblies, STLs of the printed parts, a BOM,
renders, and a comparison table.

    uv run build_concepts.py            # all concepts -> out/concepts/<key>/
    uv run build_concepts.py --no-render

Renders per concept: on the door, without the cover (guts), a cut through the bore
axis, and exploded along Z.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

from build123d import Box, Compound, Pos, export_step, export_stl

from concepts.designs import CONCEPTS, _visible, life_years, wh_per_year
from concepts.kit import BOUGHT, CELL, FASTENER, METAL, PCB, PRINTED
from deadbolt.fixture import GENERIC_US, build

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "concepts"
EXPLODE = 2.5  # each part moves up by this times the height of its underside above the door
SHELL_LIFT = 110.0  # cover, knob and battery door lift clear of the rest


def scene(c, mode: str) -> Compound:
    parts = c.parts(GENERIC_US)
    fixture = build(GENERIC_US)
    if mode == "guts":
        parts = _visible(parts, guts_only=True)
        fixture.pop("door")
    if mode == "exploded":
        moved = []
        for name, v in parts.items():
            z0 = max(v.bounding_box().min.Z, 0.0)
            lift = EXPLODE * z0 + (SHELL_LIFT if name not in _visible(parts, True) else 0.0)
            if c.key == "A-puck" and name.startswith(("chassis", "plate", "screw_door", "screw_chassis", "light_pipe")):
                lift -= 70.0  # the cup is the full-height body: drop it so the insides show
            m = Pos(0, 0, lift) * v
            m.label, m.color = v.label, v.color
            moved.append(m)
        return Compound(label=c.key, children=moved)
    children = list(parts.values()) + list(fixture.values())
    if mode == "section":
        keep = Pos(0, 500, 0) * Box(1000, 1000, 1000)
        cut = []
        for v in children:
            half = v & keep
            if half and half.volume > 0:
                half.label, half.color = v.label, v.color
                cut.append(half)
        children = cut
    return Compound(label=c.key, children=children)


VIEWS = {
    "door": ("-1,-0.8,-1.4", "+Y"),
    "guts": ("0,0,-1", "+Y"),
    "section": ("0,1,0", "+Z"),
    "exploded": ("-1,-0.6,-0.5", "+Z"),
}


def render(step: Path, png: Path, direction: str, up: str) -> None:
    subprocess.run(
        ["f3d", str(step), "--output", str(png), "--resolution", "1100,900",
         "--camera-direction", direction, "--up", up, "--anti-aliasing", "--tone-mapping",
         "--edges", "--background-color", "1,1,1", "--grid=false", "--camera-orthographic"],
        check=True, capture_output=True)


SECTIONS = [(PRINTED, "Printed parts (STL in this folder)"), (METAL, "Metal parts"),
            (BOUGHT, "Bought parts"), (PCB, "PCBs"), (CELL, "Cells"), (FASTENER, "Fasteners")]


def bom(c, u) -> str:
    lines = [f"# {c.name} ({c.key}) bill of materials\n", c.summary + "\n"]
    for kind, title in SECTIONS:
        counts = Counter(i.desc for i in u.items.values() if i.kind == kind)
        if counts:
            lines.append(f"\n## {title}\n\n| Qty | Part |\n|---|---|")
            lines += [f"| {n} | {desc} |" for desc, n in sorted(counts.items())]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()
    do_render = not args.no_render and shutil.which("f3d")
    OUT.mkdir(parents=True, exist_ok=True)

    rows = []
    for c in CONCEPTS.values():
        d = OUT / c.key
        d.mkdir(exist_ok=True)
        u = c.unit(GENERIC_US)
        export_step(c.assembly(GENERIC_US), d / f"{c.key}.step")
        for name, item in u.items.items():
            if item.kind == PRINTED:
                export_stl(item.part, d / f"{name}.stl")
        (d / "BOM.md").write_text(bom(c, u))
        for view, (direction, up) in VIEWS.items():
            step = d / f"{c.key}-{view}.step"
            export_step(scene(c, view), step)
            if do_render:
                render(step, d / f"{c.key}-{view}.png", direction, up)
        shell = Compound(children=[i.part for n, i in u.items.items()
                                   if not n.startswith(("screw_door",))])
        size = shell.bounding_box().size
        counts = Counter(i.kind for i in u.items.values())
        rows.append((c, size, counts))

    print(f"Energy model: {wh_per_year():.1f} Wh/year (see concepts/designs.py for assumptions)\n")
    print("| Concept | W x H x D mm | Battery | Est. life | Motor | Printed | Bought/PCB | Screws |")
    print("|---|---|---|---|---|---|---|---|")
    for c, s, n in rows:
        print(f"| {c.name} | {s.X:.0f} x {s.Y:.0f} x {s.Z:.0f} | {c.battery} ({c.battery_wh:.0f} Wh) | "
              f"{life_years(c):.1f} yr | {c.motor} | {n[PRINTED]} | {n[BOUGHT] + n[PCB] + n[METAL]} | {n[FASTENER]} |")
    print(f"\nwrote {OUT.relative_to(HERE)}/", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
