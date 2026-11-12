"""Export the dev kit: STEP, STLs of the printed parts, BOM, wiring table, renders.

    uv run build_devkit.py            # -> out/devkit/
    uv run build_devkit.py --no-render
"""

from __future__ import annotations

import argparse
import shutil
from collections import Counter

from build123d import Compound, export_step, export_stl

from build_concepts import SECTIONS, render
from concepts.kit import PRINTED, WIRE
from devkit.rig import WIRING, rig

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "out" / "devkit"
VIEWS = {
    "door": ("-1,-0.8,-1.4", "+Y"),
    "front": ("0,0,-1", "+Y"),
    "side": ("-1,0,0", "+Y"),
    "guts": ("0,0,-1", "+Y"),  # no cover or knob
}
SHELL = ("cover", "knob", "screw_knob", "screw_cover")


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
            export_stl(item.part, OUT / f"{name}.stl")
    (OUT / "BOM.md").write_text(bom(u))
    (OUT / "WIRING.md").write_text(wiring())
    guts = Compound(label="devkit", children=[i.part for n, i in u.items.items() if not n.startswith(SHELL)]
                    + [v for k, v in fixture.items() if k != "door"])
    export_step(guts, OUT / "devkit-guts.step")
    if not args.no_render and shutil.which("f3d"):
        for view, (direction, up) in VIEWS.items():
            step = OUT / ("devkit-guts.step" if view == "guts" else "devkit.step")
            render(step, OUT / f"devkit-{view}.png", direction, up)
    n = Counter(i.kind for i in u.items.values())
    print(f"{len(u.items)} parts: {n[PRINTED]} printed, {n['fastener']} fasteners, {n[WIRE]} wires")
    print(f"wrote {OUT.relative_to(HERE)}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
