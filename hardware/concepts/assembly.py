"""Can it actually be put together? Checks on the assembly sequence, not just the final pose.

concepts/check.py looks at the finished assembly: nothing interpenetrates, every joint
touches. That passes a model whose parts could never be fitted: a deck that has to pass
through a post to get in, a peg that is exactly the size of its hole. These checks take
the unit's assembly steps (Unit.steps, in order) and ask, for each:

- insertion: can its parts travel in along their path without passing through anything
  already fitted? Run backwards: each part is pulled back out along its path in 0.5 mm
  steps and must hit nothing placed earlier. Snap barbs (Unit.barbs) are left out of
  this sweep, since their arms flex to let things past, and so is the overlap of meshing
  gear teeth. The strain of each snap is checked in concepts/check.py.
- clearance: printed parts are never exactly the size of what they fit round. Each part is
  shifted CLEARANCE_PROBE sideways (square to every move of its path: it may rest against
  things along those) both ways, and must still hit nothing, except the press fits declared
  in Unit.press.
- sequence: every part is fitted by exactly one step.

The geometry is meshed (manifold3d) because a sweep is hundreds of booleans: about 2 ms
each on meshes against 50 ms on the B-rep.
"""

from __future__ import annotations

from dataclasses import dataclass

import manifold3d as m3
import numpy as np
from build123d import Part

from concepts.kit import WIRE, Unit

STEP_MM = 0.5  # sweep resolution: under half the thinnest wall (1.2) plus the thinnest feature
CLEARANCE_PROBE = 0.1  # a fit must have at least this much play each side
OVERLAP_TOL = 0.5  # mm^3: meshing tolerance on curved faces that touch
MESH_TOL = 80.0  # mm^3: meshing teeth slide in between each other; see check.MESH_TOL


@dataclass(frozen=True)
class Step:
    """One assembly step: these parts go in together along `path`.

    path is a list of (direction, distance) moves, in the order they are made; the last
    one ends with the parts seated. distance None means "from clear of everything": only
    the first move may use it.

    A subassembly is built off the door: its steps carry `sub` (a name) and fit their parts
    only against the earlier parts of the same subassembly. A later step then `carry`s the
    whole subassembly into place, as one.
    """
    parts: tuple[str, ...]
    path: tuple[tuple[tuple[float, float, float], float | None], ...]
    how: str
    sub: str = ""
    carry: tuple[str, ...] = ()

    def sideways(self) -> list[np.ndarray]:
        """Unit vectors square to every move of the path, both ways."""
        moves = np.array([d for d, _ in self.path], dtype=float)
        _, sv, vt = np.linalg.svd(moves)
        rank = int(np.sum(sv > 1e-9))
        return [sign * v for v in vt[rank:] for sign in (1, -1)]


DOWN = (0.0, 0.0, -1.0)  # towards the door: how most parts go in


UP = (0.0, 0.0, 1.0)


def step(parts: str | list[str], how: str, *path, sub: str = "", carry: str = "") -> Step:
    """Shorthand: step("deck", "...") goes straight in towards the door. carry names a
    subassembly; its parts are looked up when the unit is checked."""
    names = (parts,) if isinstance(parts, str) else tuple(parts)
    return Step(names, tuple(path) or ((DOWN, None),), how, sub, (carry,) if carry else ())


def _sub_parts(u: Unit, name: str) -> list[str]:
    return [n for s in u.steps if s.sub == name for n in s.parts]


def _moving(u: Unit, s: Step) -> list[str]:
    """The parts a step moves: its own, and those of any subassembly it carries."""
    return list(s.parts) + [n for c in s.carry for n in _sub_parts(u, c)]


def _mesh(part: Part) -> m3.Manifold:
    verts, tris = part.tessellate(0.03, 0.2)
    v = np.array([(p.X, p.Y, p.Z) for p in verts], dtype=np.float32)
    mesh = m3.Mesh(vert_properties=v, tri_verts=np.array(tris, dtype=np.uint32))
    mesh.merge()
    return m3.Manifold(mesh)


class Meshes:
    """Each part meshed once, with its bounding box; `bare` are the parts without their
    snap barbs, for the insertion sweep."""

    def __init__(self, u: Unit, fixture: dict[str, Part]):
        self.m: dict[str, m3.Manifold] = {}
        for name, item in u.items.items():
            if item.kind != WIRE:
                self.m[name] = _mesh(item.part)
        for name, part in fixture.items():
            self.m[f"fixture:{name}"] = _mesh(part)
        self.bare = dict(self.m)
        for name, barbs in u.barbs.items():
            if name in self.m:
                self.bare[name] = self.m[name] - _mesh(barbs)
        self.bb = {k: np.array(v.bounding_box()).reshape(2, 3) for k, v in self.m.items()}

    def overlap(self, a: str, b: str, shift: np.ndarray, bare: bool = False) -> float:
        lo, hi = self.bb[a] + shift
        blo, bhi = self.bb[b]
        if np.any(lo > bhi) or np.any(blo > hi):
            return 0.0
        m = self.bare if bare else self.m
        return (m[a].translate(tuple(shift)) ^ m[b]).volume()


def sequence(u: Unit) -> list[str]:
    seen: dict[str, int] = {}
    out = []
    for k, s in enumerate(u.steps):
        for n in s.parts:
            if n not in u.items:
                out.append(f"step {k + 1} fits '{n}', which is not a part")
            elif n in seen:
                out.append(f"'{n}' is fitted twice (steps {seen[n] + 1} and {k + 1})")
            seen[n] = k
    missing = [n for n, i in u.items.items() if i.kind != WIRE and n not in seen]
    out += [f"'{n}' is never fitted" for n in missing]
    subs = {s.sub for s in u.steps if s.sub}
    carried = [c for s in u.steps for c in s.carry]
    out += [f"subassembly '{c}' is never put in place" for c in sorted(subs - set(carried))]
    out += [f"step carries '{c}', which is no subassembly" for c in carried if c not in subs]
    return out


def _allowed(u: Unit, a: str, b: str) -> float:
    return MESH_TOL if frozenset({a, b}) in u.meshes else OVERLAP_TOL


def _withdrawal(s: Step, extent: float) -> list[np.ndarray]:
    """Offsets that pull a step's parts back out along its path, STEP_MM apart."""
    out, at = [], np.zeros(3)
    for d, dist in reversed(s.path):
        back = -np.array(d, dtype=float)
        n = int(np.ceil((dist if dist is not None else extent) / STEP_MM))
        for _ in range(n):
            at = at + back * STEP_MM
            out.append(at.copy())
    return out


def insertion(u: Unit, fixture: dict[str, Part], meshes: Meshes | None = None) -> list[str]:
    """Each step's parts, pulled back out along their path, hit nothing fitted before them."""
    M = meshes or Meshes(u, fixture)
    placed = [f"fixture:{k}" for k in fixture]
    subs: dict[str, list[str]] = {}
    out = []
    for k, s in enumerate(u.steps):
        moving = [n for n in _moving(u, s) if n in M.m]
        against = subs.get(s.sub, []) if s.sub else placed
        if moving and against:
            lo = np.min([M.bb[n][0] for n in moving], axis=0)
            hi = np.max([M.bb[n][1] for n in moving], axis=0)
            plo = np.min([M.bb[n][0] for n in against], axis=0)
            phi = np.max([M.bb[n][1] for n in against], axis=0)
            extent = float(np.max(np.maximum(phi - lo, hi - plo))) + 1.0  # far enough to be clear
            offsets = _withdrawal(s, extent)
            for n in moving:
                for p in against:
                    tol = _allowed(u, n, p.removeprefix("fixture:"))
                    for off in offsets:
                        v = M.overlap(n, p, off, bare=True)
                        if v > tol:
                            moved = float(np.linalg.norm(off))
                            out.append(f"step {k + 1} ({s.how}): {n} hits {p.removeprefix('fixture:')} "
                                       f"{moved:.1f} mm before it is home ({v:.1f} mm^3)")
                            break
        if s.sub:
            subs.setdefault(s.sub, []).extend(moving)
        else:
            placed += moving
    return out


def clearance(u: Unit, fixture: dict[str, Part], meshes: Meshes | None = None) -> list[str]:
    """Every part has play sideways (across its last move) against everything it fits to."""
    M = meshes or Meshes(u, fixture)
    out, seen = [], set()
    for s in u.steps:
        shifts = [v * CLEARANCE_PROBE for v in s.sideways()]
        for n in s.parts:
            if n not in M.m:
                continue
            for p in M.m:
                other = p.removeprefix("fixture:")
                pair = frozenset({n, other})
                if other == n or pair in seen or pair in u.press or other in s.parts:
                    continue
                if pair in u.meshes:
                    continue
                worst = max(M.overlap(n, p, sh) for sh in shifts)
                if worst > OVERLAP_TOL:
                    seen.add(pair)
                    out.append(f"{n} and {other}: no clearance ({worst:.1f} mm^3 when shifted "
                               f"{CLEARANCE_PROBE} mm); a printed fit needs play")
    return out


def check(u: Unit, fixture: dict[str, Part]) -> dict[str, list[str]]:
    M = Meshes(u, fixture)
    return {"sequence": sequence(u), "insertion": insertion(u, fixture, M), "clearance": clearance(u, fixture, M)}
