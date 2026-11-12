"""Wiring drawings and the main-PCB schematic, all from the one net list (wiring/netlist.py).

    uv run build_wiring.py

Writes (committed, so they show on GitHub):
  drawings/bench-wiring.svg    stage 1: the parts on hand on one 830-point breadboard
  drawings/devkit-wiring.svg   stage 2: the printed dev kit's breadboards, every wire and hole
  pcb/main/                    stage 3: KiCad 10 project for the main PCB (open lochness-main.kicad_pro)
and, not committed, out/wiring/lochness-main.pdf, a PDF of the schematic.

First runs the electrical rule check (wiring/erc.py) on all three stages and stops if it
finds anything. Then checks what it wrote: KiCad's own net list of the schematic must equal the product's
net list, and KiCad's ERC must report no errors. Needs KiCad 10 (kicad-cli).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiring import erc
from wiring import kicad as K
from wiring import schematic
from wiring.bench import BENCH, bench_svg, plan_connectivity as bench_connectivity
from wiring.breadboard import devkit_svg
from wiring.netlist import DEVKIT
from wiring.product import PRODUCT

HERE = Path(__file__).resolve().parent
DRAWINGS = HERE / "drawings"
PCB = HERE / "pcb" / "main"
OUT = HERE / "out" / "wiring"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args()
    problems = BENCH.check() + DEVKIT.check() + PRODUCT.check()
    if bench_connectivity() != BENCH.connectivity():
        problems.append("bench: the wire plan doesn't join the bench net list")
    for stage in (BENCH, DEVKIT, PRODUCT):  # electrical rules: shorts, fights, over/under-voltage
        problems += [f"{stage.key}: {f}" for f in erc.check_stage(stage)]
    if problems:
        print("\n".join(problems))
        return 1
    print("electrical check: bench, devkit and product clean")
    for stage in (BENCH, DEVKIT):
        for req in erc.firmware_requirements(stage):
            print(f"  {stage.key} firmware must: {req}")

    DRAWINGS.mkdir(exist_ok=True)
    (DRAWINGS / "bench-wiring.svg").write_text(bench_svg())
    print(f"wrote {(DRAWINGS / 'bench-wiring.svg').relative_to(HERE)}")
    (DRAWINGS / "devkit-wiring.svg").write_text(devkit_svg())
    print(f"wrote {(DRAWINGS / 'devkit-wiring.svg').relative_to(HERE)}")

    sch = schematic.build(PCB)
    print(f"wrote {sch.relative_to(HERE)}")
    OUT.mkdir(parents=True, exist_ok=True)
    schematic.export_pdf(sch, OUT / "lochness-main.pdf")
    print(f"wrote {(OUT / 'lochness-main.pdf').relative_to(HERE)}")

    got, want = K.export_netlist(sch), {n: set(p) for n, p in PRODUCT.nets().items()}
    if got != want:
        for n in sorted(set(got) | set(want)):
            if got.get(n) != want.get(n):
                print(f"net {n}: schematic {sorted(got.get(n, []))} vs net list {sorted(want.get(n, []))}")
        return 1
    code, report = K.erc(sch, OUT / "erc.rpt")
    if code:
        print(report)
        return 1
    print(f"schematic: {len(want)} nets match wiring/product.py; ERC: no errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
