"""Export the dev kit: STEP, STLs of the printed parts (each in its print pose), BOM, print
table, assembly steps, wiring table, renders. Two renders are also copied to drawings/ for
the build guide, SETUP.md.

    uv run build_devkit.py            # -> out/devkit/
    uv run build_devkit.py --no-render
"""

from __future__ import annotations

import argparse
import shutil
from collections import Counter

import numpy as np
from build123d import Compound, Pos, Rot, export_step, export_stl

from build_concepts import SECTIONS, render
from concepts.kit import PRINTED, WIRE
from devkit.rig import PRINT, WIRING, rig

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "devkit"
VIEWS = {
    "door": ("-1,-0.8,-1.4", "+Y"),
    "front": ("0,0,-1", "+Y"),
    "side": ("-1,0,0", "+Y"),
    "guts": ("0,0,-1", "+Y"),  # no cover or knob
}
SHELL = ("cover", "knob")


def bom(u) -> str:
    lines = ["# Dev kit bill of materials\n",
             "The tall slab, opened up: same door mounting, drive, motor and battery bay, and the "
             "same chips as the product's main PCB, each on a breakout: the ESP32-C6-DevKitM-1 and "
             "the Pololu DRV8833 carrier on two mini breadboards on a deck, and the three DRV5032 Hall "
             "switches on SOT-23 adapters in a carrier over the bolt flag. See WIRING.md for the pin "
             "map. The product's TPS629206 regulator isn't in the kit: the DevKit makes its own "
             "3.3 V from USB.\n"]
    for kind, title in SECTIONS + [(WIRE, "Wires")]:
        counts = Counter(i.desc for i in u.items.values() if i.kind == kind)
        if counts:
            lines.append(f"\n## {title}\n\n| Qty | Part |\n|---|---|")
            lines += [f"| {n} | {d} |" for d, n in sorted(counts.items())]
    return "\n".join(lines) + "\n"


def on_bed(part, rot):
    """The part in its print pose, resting on z = 0."""
    p = Rot(*rot) * part
    return Pos(0, 0, -p.bounding_box().min.Z) * p


def overhangs(part) -> tuple[float, float]:
    """(flat ceilings, faces steeper than 45 deg) facing down above the bed, mm^2. Flat
    ceilings with walls both sides are bridges a printer spans; the rest need support."""
    vs, ts = part.tessellate(0.05, 0.3)
    v, t = np.array([(q.X, q.Y, q.Z) for q in vs]), np.array(ts)
    a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
    n = np.cross(b - a, c - a)
    area = np.linalg.norm(n, axis=1) / 2
    nz = n[:, 2] / np.maximum(2 * area, 1e-12)
    up = np.minimum(np.minimum(a[:, 2], b[:, 2]), c[:, 2]) > 0.2
    return float(area[(nz < -0.99) & up].sum()), float(area[(nz < -0.72) & (nz >= -0.99) & up].sum())


def print_table(u) -> str:
    lines = ["# Dev kit printed parts\n",
             "PETG (the coupler in PA12 or PETG-CF), 0.4 mm nozzle, 0.2 mm layers, 3 perimeters. The STLs "
             "in this folder are already in the pose below. Every fit has 0.15-0.2 mm of play per side "
             "modelled in, which suits a calibrated printer; if snaps are too tight, check the flow first.\n",
             "Ceilings are downward faces above the bed: bridged spans between walls (window tops, the "
             "chassis floor over the plate pocket) and barb undersides under 1.3 mm. Over 45 deg are "
             "slopes that would need support.\n",
             "| Part | Qty | Pose | Ceilings mm2 | Over 45 deg mm2 | Notes |", "|---|---|---|---|---|---|"]
    for name, (rot, note) in PRINT.items():
        flat, steep = overhangs(on_bed(u.items[name].part, rot))
        pose = "upside down" if rot[0] == 180 else "as modelled"
        lines.append(f"| {name} | 1 | {pose} | {flat:.0f} | {steep:.0f} | {note} |")
    return "\n".join(lines) + "\n"


def assembly(u) -> str:
    lines = ["# Dev kit assembly\n",
             "In this order: `uv run python -m devkit.check` checks that each step can be done (every part "
             "travels in along its path without passing through anything already fitted) and that no "
             "printed fit is line to line. The only screws are the deadbolt's own two through-screws.\n"]
    k, sub = 0, ""
    for st in u.steps:
        if st.sub and st.sub != sub:
            k += 1
            lines.append(f"{k}. **{st.how}**")
        elif st.sub:
            lines.append(f"   - {st.how}")
        else:
            k += 1
            lines.append(f"{k}. {st.how}")
        sub = st.sub
    lines += ["", "## Snaps\n", "| Snap | Bends | Strain | Limit |", "|---|---|---|---|"]
    for sn in u.snaps:
        lines.append(f"| {sn.name} | {sn.layers or '-'} the layers | {sn.strain:.1%} | {sn.limit:.1%} |")
    return "\n".join(lines) + "\n"


def wiring() -> str:
    lines = ["# Dev kit wiring (proposed pin map)\n",
             "ESP32-C6-DevKitM-1 GPIOs. Avoids the strapping pins (8, 9, 15), USB (12, 13) and "
             "UART0 (16, 17). The 4xAA pack powers the motor through the DRV8833 carrier's VIN; the "
             "DevKit runs from USB through the cover's side slot. All grounds are common. Motor "
             "cable pins are the Pololu connector's (1 encoder GND, 2 B, 3 A, 4 VCC, 5 M2, 6 M1). "
             "Which SOT-23 adapter holes carry the DRV5032's VCC, OUT and GND depends on how it is "
             "soldered: check the silkscreen.\n",
             "| Signal | From | To | Wire |", "|---|---|---|---|"]
    lines += [f"| {s} | {a} | {b} | {c} |" for s, a, b, c in WIRING]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.stl"):
        old.unlink()
    u, fixture = rig()
    scene = Compound(label="devkit", children=[i.part for i in u.items.values()] + list(fixture.values()))
    export_step(scene, OUT / "devkit.step")
    for name, item in u.items.items():
        if item.kind == PRINTED:
            export_stl(on_bed(item.part, PRINT[name][0]), OUT / f"{name}.stl")
    (OUT / "BOM.md").write_text(bom(u))
    (OUT / "PRINT.md").write_text(print_table(u))
    (OUT / "ASSEMBLY.md").write_text(assembly(u))
    (OUT / "WIRING.md").write_text(wiring())
    guts = Compound(label="devkit", children=[i.part for n, i in u.items.items() if not n.startswith(SHELL)]
                    + [v for k, v in fixture.items() if k != "door"])
    export_step(guts, OUT / "devkit-guts.step")
    if not args.no_render and shutil.which("f3d"):
        for view, (direction, up) in VIEWS.items():
            step = OUT / ("devkit-guts.step" if view == "guts" else "devkit.step")
            render(step, OUT / f"devkit-{view}.png", direction, up)
        for view in ("guts", "door"):  # the build guide (SETUP.md) shows these two
            shutil.copy(OUT / f"devkit-{view}.png", HERE / "drawings" / f"devkit-{view}.png")
    n = Counter(i.kind for i in u.items.values())
    print(f"{len(u.items)} parts: {n[PRINTED]} printed, {n['fastener']} fasteners, {n[WIRE]} wires")
    print(f"wrote {OUT.relative_to(HERE)}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
