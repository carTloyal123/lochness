"""Reference door + US single-cylinder deadbolt, as the lock unit sees it.

This is our own model, written from published standard dimensions, so the
enclosure has something to fit around. It shows the door *after* the interior
thumb-turn is removed, because the lochness unit replaces it
(docs/ARCHITECTURE.md §2.2). What is left is:

- the door slab with its cross bore, edge bore and faceplate mortise,
- the latch (housing, drive box, faceplate) and the bolt,
- the exterior cylinder (rose, body, threaded screw posts),
- the tailpiece sticking out of the interior face. The unit's drive couples to it.

Coordinate frame (millimetres):
    origin  cross-bore axis, on the door's interior face
    +Z      out of the door into the room (the side the unit mounts on);
            the door occupies -door_thickness <= z <= 0
    +X      towards the latch edge of the door; the edge is at x = backset
    +Y      up

The exterior cylinder, latch and bolt stay as they are on the door and the
unit never touches them, so their sizes here are illustrative only. The values
in UNMEASURED are the ones the unit does depend on (tailpiece, screw posts).
They are placeholders until measured on real hardware; the build warns about
them.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, fields, replace

from build123d import Box, Color, Compound, Cylinder, Part, Pos, Rot

INCH = 25.4


@dataclass(frozen=True)
class DeadboltParams:
    # --- Door and standard US preparation (published; see hardware/README.md) ---
    door_thickness: float = 1.75 * INCH  # 1-3/8" to 1-3/4" common; exterior doors 1-3/4"
    backset: float = 2.375 * INCH  # 2-3/8" or 2-3/4"
    cross_bore_d: float = 2.125 * INCH  # 2-1/8" through the door face
    edge_bore_d: float = 1.0 * INCH  # 1" from the door edge to the cross bore
    bolt_throw: float = 1.0 * INCH  # 1" residential deadbolt throw
    faceplate_w: float = 1.0 * INCH  # across the door edge (Z)
    faceplate_h: float = 2.25 * INCH  # along the door edge (Y)
    faceplate_round: bool = False  # True: drive-in/round-corner faceplate

    # --- Latch and bolt (illustrative; stays in the door, untouched by the unit) ---
    faceplate_t: float = 3.2
    bolt_d: float = 22.0
    bolt_len: float = 32.0  # retracted, must stay clear of the +X screw post
    housing_d: float = 24.0  # must fit the edge bore
    housing_tail: float = 14.0  # how far the housing runs past the bore axis
    drive_box_x: float = 52.0  # latch frame in the bore; the screws pass through it
    drive_box_y: float = 24.0

    # --- Tailpiece (UNMEASURED; brand-specific, the part the unit couples to) ---
    tailpiece_w: float = 7.5
    tailpiece_t: float = 2.0
    tailpiece_protrusion: float = 25.0  # beyond the interior face, after break-off
    tailpiece_locked_deg: float = 0.0  # 0 = wide face along X
    tailpiece_throw_deg: float = 90.0  # turn from locked to unlocked

    # --- Exterior cylinder (illustrative, except the screw posts, which are UNMEASURED) ---
    rose_d: float = 68.0
    rose_h: float = 22.0
    body_d: float = 50.0
    screw_spacing: float = 38.1  # through-bolt posts, left and right of the tailpiece (X axis), through the latch
    post_d: float = 8.0
    post_hole_d: float = 3.5
    post_top_below_face: float = 1.5  # posts stop this far inside the interior face

    # --- Door section shown (cosmetic) ---
    door_section_w: float = 180.0
    door_section_h: float = 240.0

    # Interface values the unit depends on that are still guesses.
    UNMEASURED = frozenset(
        {
            "tailpiece_w", "tailpiece_t", "tailpiece_protrusion",
            "tailpiece_locked_deg", "tailpiece_throw_deg",
            "screw_spacing", "post_d", "post_hole_d", "post_top_below_face",
        }
    )

    def unmeasured(self) -> list[str]:
        return sorted(f.name for f in fields(self) if f.name in self.UNMEASURED)


# Starting points. Brand presets (Kwikset, Schlage, Yale) get added once their
# UNMEASURED fields are measured off real hardware.
GENERIC_US = DeadboltParams()
GENERIC_US_THIN_DOOR = replace(GENERIC_US, door_thickness=1.375 * INCH)
GENERIC_US_LONG_BACKSET = replace(GENERIC_US, backset=2.75 * INCH)

PRESETS: dict[str, DeadboltParams] = {
    "generic": GENERIC_US,
    "generic-thin-door": GENERIC_US_THIN_DOOR,
    "generic-long-backset": GENERIC_US_LONG_BACKSET,
}


def _x_cylinder(d: float, x0: float, x1: float, z: float = 0.0) -> Part:
    """Cylinder of diameter d along X from x0 to x1, axis at (y=0, z)."""
    return Pos((x0 + x1) / 2, 0, z) * Rot(0, 90, 0) * Cylinder(d / 2, x1 - x0)


def _z_cylinder(d: float, z0: float, z1: float, x: float = 0.0, y: float = 0.0) -> Part:
    return Pos(x, y, (z0 + z1) / 2) * Cylinder(d / 2, z1 - z0)


def _faceplate_blank(p: DeadboltParams, x_center: float) -> Part:
    zc = -p.door_thickness / 2
    if not p.faceplate_round:
        return Pos(x_center, 0, zc) * Box(p.faceplate_t, p.faceplate_h, p.faceplate_w)
    r = p.faceplate_w / 2
    straight = p.faceplate_h - p.faceplate_w
    blank = Pos(x_center, 0, zc) * Box(p.faceplate_t, straight, p.faceplate_w)
    for y in (straight / 2, -straight / 2):
        blank += _x_cylinder(p.faceplate_w, x_center - p.faceplate_t / 2,
                             x_center + p.faceplate_t / 2, zc).moved(Pos(0, y, 0))
    return blank


def _tailpiece_angle(p: DeadboltParams, locked: bool) -> float:
    return p.tailpiece_locked_deg + (0.0 if locked else p.tailpiece_throw_deg)


def _tailpiece_clearance_d(p: DeadboltParams) -> float:
    return math.hypot(p.tailpiece_w, p.tailpiece_t) + 1.0


def _post_xs(p: DeadboltParams) -> tuple[float, float]:
    return (p.screw_spacing / 2, -p.screw_spacing / 2)


def door(p: DeadboltParams) -> Part:
    t, zc = p.door_thickness, -p.door_thickness / 2
    slab = Pos(p.backset - p.door_section_w / 2, 0, zc) * Box(
        p.door_section_w, p.door_section_h, t
    )
    slab -= _z_cylinder(p.cross_bore_d, -t, 0)
    slab -= _x_cylinder(p.edge_bore_d, 0, p.backset, zc)
    slab -= _faceplate_blank(p, p.backset - p.faceplate_t / 2)  # mortise
    return slab


def latch(p: DeadboltParams) -> Part:
    zc = -p.door_thickness / 2
    face_inner = p.backset - p.faceplate_t
    housing = _x_cylinder(p.housing_d, -p.housing_tail, face_inner, zc)
    housing -= _x_cylinder(p.bolt_d + 0.6, p.drive_box_x / 2, face_inner, zc)
    frame = Pos(0, 0, zc) * Box(p.drive_box_x, p.drive_box_y, p.housing_d)
    frame &= _z_cylinder(p.cross_bore_d - 1.0, zc - p.housing_d, zc + p.housing_d)
    housing += frame
    housing -= _z_cylinder(_tailpiece_clearance_d(p), zc - p.housing_d, zc + p.housing_d)
    for x in _post_xs(p):  # the through-bolts pass through the latch frame
        housing -= _z_cylinder(p.post_d + 0.5, zc - p.housing_d, zc + p.housing_d, x=x)
    plate = _faceplate_blank(p, p.backset - p.faceplate_t / 2)
    plate -= _x_cylinder(p.bolt_d + 0.6, face_inner - 1, p.backset + 1, zc)
    return housing + plate


def bolt(p: DeadboltParams, extended: bool) -> Part:
    tip = p.backset + (p.bolt_throw if extended else 0.0)
    return _x_cylinder(p.bolt_d, tip - p.bolt_len, tip, -p.door_thickness / 2)


def exterior_cylinder(p: DeadboltParams) -> Part:
    t = p.door_thickness
    rose = _z_cylinder(p.rose_d, -t - p.rose_h, -t)
    body_top = -t / 2 - p.housing_d / 2 - 1.0
    if body_top > -t:
        rose += _z_cylinder(p.body_d, -t, body_top)
    for x in _post_xs(p):
        post = _z_cylinder(p.post_d, -t, -p.post_top_below_face, x=x)
        post -= _z_cylinder(p.post_hole_d, -t, 0, x=x)
        rose += post
    rose -= _z_cylinder(_tailpiece_clearance_d(p), -t - p.rose_h, 0)
    return rose


def tailpiece(p: DeadboltParams, locked: bool) -> Part:
    z0 = -p.door_thickness - p.rose_h / 2
    z1 = p.tailpiece_protrusion
    bar = Pos(0, 0, (z0 + z1) / 2) * Box(p.tailpiece_w, p.tailpiece_t, z1 - z0)
    return Rot(0, 0, _tailpiece_angle(p, locked)) * bar


PART_COLORS = {
    "door": Color(0.78, 0.62, 0.43),
    "latch": Color(0.72, 0.72, 0.75),
    "bolt": Color(0.45, 0.45, 0.5),
    "exterior_cylinder": Color(0.8, 0.65, 0.25),
    "tailpiece": Color(0.9, 0.2, 0.15),
}


def build(p: DeadboltParams = GENERIC_US, locked: bool = True) -> dict[str, Part]:
    """All fixture parts, by name. locked=True: bolt thrown, tailpiece at the locked angle."""
    parts = {
        "door": door(p),
        "latch": latch(p),
        "bolt": bolt(p, extended=locked),
        "exterior_cylinder": exterior_cylinder(p),
        "tailpiece": tailpiece(p, locked),
    }
    for name, part in parts.items():
        part.label = name
        part.color = PART_COLORS[name]
    return parts


def assembly(p: DeadboltParams = GENERIC_US, locked: bool = True,
             include_door: bool = True) -> Compound:
    parts = build(p, locked)
    if not include_door:
        parts.pop("door")
    return Compound(label="deadbolt_fixture", children=list(parts.values()))


def interface(p: DeadboltParams = GENERIC_US) -> dict:
    """The numbers an enclosure needs, in the fixture frame. Plain data for scripts and JSON."""
    return {
        "frame": "origin on bore axis at interior door face; +Z into room; +X to latch edge; +Y up; mm",
        "door_thickness": p.door_thickness,
        "door_edge_x": p.backset,
        "cross_bore": {"center": [0, 0, 0], "d": p.cross_bore_d},
        "tailpiece": {
            "axis": [0, 0, 1],
            "w": p.tailpiece_w,
            "t": p.tailpiece_t,
            "tip_z": p.tailpiece_protrusion,
            "locked_deg": _tailpiece_angle(p, True),
            "unlocked_deg": _tailpiece_angle(p, False),
        },
        "screw_posts": [
            {"center": [x, 0, -p.post_top_below_face], "d": p.post_d, "hole_d": p.post_hole_d}
            for x in _post_xs(p)
        ],
        "bolt_throw": p.bolt_throw,
        "params": asdict(p),
        "unmeasured": p.unmeasured(),
    }
