"""Export the deadbolt fixture: STEP, STL, interface JSON and PNG renders.

    uv run build.py                      # generic preset, locked and unlocked
    uv run build.py --preset generic-thin-door --no-render

Output goes to hardware/out/<preset>/ (git-ignored).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from build123d import Box, Compound, Pos, export_step, export_stl

from deadbolt.fixture import PART_COLORS, PRESETS, DeadboltParams, assembly, build, interface

HERE = Path(__file__).resolve().parent

# name -> f3d camera direction (looking along this vector), up vector
VIEWS = {
    "iso": ("-1,-1,-1.2", "+Y"),
    "interior": ("0,0,-1", "+Y"),  # what the unit's back plate faces
    "top": ("0,-1,0", "-Z"),  # looking down the door edge: bore, bolt, door thickness
}


SECTION_VIEW = {"section": ("0,1,0", "+Z")}  # cut face at y=0, latch edge right, room up


def section(p: DeadboltParams, locked: bool) -> Compound:
    """The fixture cut on the y=0 plane (keeping y >= 0), to show the bores and latch."""
    keep = Pos(0, 500, 0) * Box(1000, 1000, 1000)
    halves = []
    for name, part in build(p, locked).items():
        half = part & keep
        half.label, half.color = name, PART_COLORS[name]
        halves.append(half)
    return Compound(label="deadbolt_fixture_section", children=halves)


def render(step: Path, out_dir: Path, views: dict = VIEWS) -> list[Path]:
    f3d = shutil.which("f3d")
    if not f3d:
        print("f3d not found; skipping renders", file=sys.stderr)
        return []
    pngs = []
    for name, (direction, up) in views.items():
        png = out_dir / f"{step.stem}-{name}.png"
        subprocess.run(
            [f3d, str(step), "--output", str(png), "--resolution", "1200,900",
             "--camera-direction", direction, "--up", up, "--anti-aliasing",
             "--tone-mapping", "--edges", "--background-color", "1,1,1", "--grid=false",
             "--camera-orthographic"],
            check=True, capture_output=True,
        )
        pngs.append(png)
    return pngs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="generic", choices=sorted(PRESETS))
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()

    p = PRESETS[args.preset]
    out = HERE / "out" / args.preset
    out.mkdir(parents=True, exist_ok=True)

    (out / "interface.json").write_text(json.dumps(interface(p), indent=2) + "\n")
    for locked in (True, False):
        state = "locked" if locked else "unlocked"
        step = out / f"fixture-{state}.step"
        export_step(assembly(p, locked), step)
        export_step(assembly(p, locked, include_door=False), out / f"hardware-{state}.step")
        if locked:
            for name, part in build(p, locked).items():
                export_stl(part, out / f"{name}.stl")
        cut = out / f"section-{state}.step"
        export_step(section(p, locked), cut)
        if not args.no_render:
            render(step, out)
            render(cut, out, SECTION_VIEW)

    print(f"wrote {out.relative_to(HERE)}/")
    if p.unmeasured():
        print(f"WARNING: {len(p.unmeasured())} UNMEASURED placeholder values in preset "
              f"'{args.preset}': {', '.join(p.unmeasured())}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
