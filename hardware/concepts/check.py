"""Assembly checks for a concept: collisions, joints, connectivity, fit to the door,
fasteners (stock lengths, thread engagement, wall round each insert) and snap strain.

    uv run python -m concepts.check            # all concepts, prints every problem
    uv run python -m concepts.check B-slab

The tests in tests/test_concepts.py call the same functions.
"""

from __future__ import annotations

import sys
from itertools import combinations

from build123d import Compound, Part

from concepts.designs import CONCEPTS, Concept
from concepts.kit import MIN_WALL, SCREW_TYPES, SNAP_STRAIN_LIMIT, WIRE, Unit
from deadbolt.fixture import DeadboltParams, GENERIC_US, build

OVERLAP_TOL = 0.05  # mm^3: touching faces are fine, interpenetration is not
MESH_TOL = 80.0  # mm^3: meshing teeth may graze; the worm is drawn as rings, not a helix
EDGE_MARGIN = 3.0  # clear of the door's latch edge
HANDLE_CLEAR_Y = -95.0  # UNMEASURED: lowest the unit may reach before the handle below


def _vol(a: Part, b: Part) -> float:
    bb_a, bb_b = a.bounding_box(), b.bounding_box()
    if (bb_a.min.X > bb_b.max.X or bb_b.min.X > bb_a.max.X or bb_a.min.Y > bb_b.max.Y
            or bb_b.min.Y > bb_a.max.Y or bb_a.min.Z > bb_b.max.Z or bb_b.min.Z > bb_a.max.Z):
        return 0.0
    i = a & b
    return i.volume if i else 0.0


def collisions(u: Unit, fixture: dict[str, Part]) -> list[str]:
    out = []
    parts = {k: v.part for k, v in u.items.items() if v.kind != WIRE}  # wires are flexible
    for (a, pa), (b, pb) in combinations(parts.items(), 2):
        v = _vol(pa, pb)
        tol = MESH_TOL if frozenset({a, b}) in u.meshes else OVERLAP_TOL
        if v > tol:
            out.append(f"{a} x {b}: {v:.2f} mm^3")
    for a, pa in parts.items():
        for f, pf in fixture.items():
            v = _vol(pa, pf)
            if v > OVERLAP_TOL:
                out.append(f"{a} x fixture {f}: {v:.2f} mm^3")
    return out


def joint_gaps(u: Unit, fixture: dict[str, Part]) -> list[str]:
    parts = u.parts()
    door = Compound(children=list(fixture.values()))
    out = []
    for a, b, gap in u.joints:
        pa = parts.get(a)
        pb = door if b == "door" else parts.get(b)
        if pa is None or pb is None:
            out.append(f"{a} - {b}: unknown part")
            continue
        d = pa.distance_to(pb)
        if d > gap + 1e-3:
            out.append(f"{a} - {b}: {d:.2f} mm apart (allowed {gap})")
    return out


def unattached(u: Unit) -> list[str]:
    """Parts with no chain of joints back to the door."""
    adj: dict[str, set[str]] = {}
    for a, b, _ in u.joints:
        adj.setdefault(a, set()).add(b)
        adj.setdefault(b, set()).add(a)
    seen, todo = {"door"}, ["door"]
    while todo:
        for n in adj.get(todo.pop(), ()):
            if n not in seen:
                seen.add(n)
                todo.append(n)
    return sorted(set(u.items) - seen)


def fit(u: Unit, p: DeadboltParams) -> list[str]:
    out = []
    for name, part in u.parts().items():
        bb = part.bounding_box()
        if bb.max.X > p.backset - EDGE_MARGIN:
            out.append(f"{name} reaches x={bb.max.X:.1f}, too close to the door edge")
        if bb.min.Y < HANDLE_CLEAR_Y:
            out.append(f"{name} reaches y={bb.min.Y:.1f}, into the handle")
        if bb.min.Z < -0.01 and not name.startswith("screw_door"):
            out.append(f"{name} goes into the door (z={bb.min.Z:.2f})")
    return out


WALL_TOL = 0.01  # fraction of the wall probe allowed outside the part (faceting)


def fasteners(u: Unit) -> list[str]:
    """Every screw is a stock length, reaches far enough into its thread without bottoming
    out, and has at least MIN_WALL of material round its insert or tapped hole."""
    parts = u.parts()
    out = []
    for f in u.fixings:
        if f.length not in SCREW_TYPES[f.type].lengths:
            out.append(f"{f.name}: {f.type} x {f.length} is not a stock length")
        lo, hi = f.eng_range
        if not lo - 1e-6 <= f.engagement <= hi + 1e-6:
            out.append(f"{f.name}: {f.engagement:.1f} mm of thread, needs {lo:.1f}-{hi:.1f}")
        if f.probe is not None and f.into in parts:
            outside = (f.probe - parts[f.into]).volume
            if outside > WALL_TOL * f.probe.volume:
                out.append(f"{f.name}: wall round its thread in {f.into} is under {MIN_WALL} mm "
                           f"({outside:.2f} mm^3 missing)")
    return out


def snaps(u: Unit) -> list[str]:
    return [f"{s.name}: {s.strain:.1%} strain, over {SNAP_STRAIN_LIMIT:.1%}"
            for s in u.snaps if s.strain > SNAP_STRAIN_LIMIT]


def check(c: Concept, p: DeadboltParams = GENERIC_US, locked: bool = True) -> dict[str, list[str]]:
    u, fixture = c.unit(p, locked), build(p, locked)
    return {"collisions": collisions(u, fixture), "joints": joint_gaps(u, fixture),
            "unattached": unattached(u), "fit": fit(u, p), "fasteners": fasteners(u),
            "snaps": snaps(u)}


def main() -> int:
    keys = sys.argv[1:] or list(CONCEPTS)
    bad = 0
    for k in keys:
        for locked in (True, False):
            for kind, problems in check(CONCEPTS[k], locked=locked).items():
                for msg in problems:
                    bad += 1
                    print(f"{k} {'locked' if locked else 'unlocked'} {kind}: {msg}")
    print("ok" if not bad else f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
