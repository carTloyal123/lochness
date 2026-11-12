"""Parts shared by the lock-unit concepts: geometry helpers, bought parts, fasteners,
gears and the drive module that every concept uses.

Units are mm in the fixture frame (deadbolt/fixture.py): origin on the bore axis at
the interior door face, +Z into the room, +X towards the latch edge, +Y up.

The motor and the electronics are modelled from their spec sheets in concepts/parts.py;
cells, magnets and screws are datasheet envelopes. Printed parts are the ones we design and
are exported as STL.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from build123d import (Box, Color, Compound, Cone, Cylinder, Location, Part, Plane, Polygon, Pos,
                       RectangleRounded, Rot, extrude)

from concepts import parts as P

from deadbolt.fixture import DeadboltParams, _post_xs, _tailpiece_angle

# --- kinds: decide colour, BOM section and whether we export an STL ---
PRINTED, METAL, BOUGHT, FASTENER, CELL, PCB = "printed", "metal", "bought", "fastener", "cell", "pcb"
WIRE = "wire"  # hook-up wire (dev kit); flexible, so the collision check skips it
KIND_COLORS = {
    PRINTED: Color(0.92, 0.92, 0.9), METAL: Color(0.6, 0.62, 0.66), BOUGHT: Color(0.85, 0.5, 0.15),
    FASTENER: Color(0.25, 0.25, 0.28), CELL: Color(0.2, 0.7, 0.35), PCB: Color(0.1, 0.45, 0.2),
    WIRE: Color(0.9, 0.9, 0.9),
}
ACCENT = {  # printed parts that should stand out from the shell
    "coupler": Color(0.2, 0.45, 0.85), "ring_gear": Color(0.3, 0.65, 0.95),
    "pinion": Color(0.95, 0.75, 0.2), "worm": Color(0.95, 0.75, 0.2),
    "knob": Color(0.25, 0.25, 0.28), "bridge": Color(0.75, 0.8, 0.85),
    "chassis": Color(0.82, 0.84, 0.86), "light_pipe": Color(0.95, 0.95, 1.0),
    "battery_door": Color(0.85, 0.85, 0.82),
    "bolt_flag": Color(0.55, 0.35, 0.8), "bolt_magnet": Color(0.7, 0.1, 0.1),
}
INSERT_COLOR = Color(0.8, 0.65, 0.25)  # brass


@dataclass
class Item:
    part: Part
    kind: str
    desc: str  # BOM line: what to buy or print


@dataclass
class Unit:
    """A concept, assembled: its parts, how they attach, and which ones mesh."""
    items: dict[str, Item] = field(default_factory=dict)
    # (a, b, max_gap): a and b must be within max_gap mm (0 = touching). "door" is the fixture.
    joints: list[tuple[str, str, float]] = field(default_factory=list)
    meshes: set[frozenset[str]] = field(default_factory=set)  # gear pairs allowed to interpenetrate
    motor_xy: tuple[float, float] | None = None
    layout: "DriveLayout | None" = None
    shaft_angle: float = 0.0  # where the coupler shaft's flat faces (it turns with the tailpiece)
    fixings: list["Fixing"] = field(default_factory=list)
    snaps: list["Snap"] = field(default_factory=list)
    _cuts: dict[str, list[Part]] = field(default_factory=dict)

    def add(self, name: str, part: Part, kind: str, desc: str) -> Part:
        assert name not in self.items, name
        for c in self._cuts.pop(name, []):
            part = _cut(part, c)
        part.label = name
        if not _is_model(part):  # a model's children carry their own colours
            part.color = ACCENT.get(name) or (INSERT_COLOR if name.endswith("_insert") else KIND_COLORS[kind])
        self.items[name] = Item(part, kind, desc)
        return part

    def cut(self, name: str, solid: Part) -> None:
        """Cut a hole in a part, whether it has been added yet or not."""
        item = self.items.get(name)
        if item is None:
            self._cuts.setdefault(name, []).append(solid)
            return
        label, color = item.part.label, item.part.color
        item.part = _cut(item.part, solid)
        item.part.label = label
        if not _is_model(item.part):
            item.part.color = color

    def joint(self, a: str, b: str, max_gap: float = 0.0) -> None:
        self.joints.append((a, b, max_gap))

    def parts(self) -> dict[str, Part]:
        return {k: v.part for k, v in self.items.items()}


def _is_model(part: Part) -> bool:
    return isinstance(part, Compound) and bool(part.children) and not isinstance(part, Part)


def _cut(part: Part, solid: Part) -> Part:
    """Cut a solid from a part, or from each coloured piece of a bought-part model."""
    if not _is_model(part):
        return part - solid
    kids = []
    for k in part.children:
        cut = k - solid if k.bounding_box().overlaps(solid.bounding_box()) else k
        cut.color = k.color
        kids.append(cut)
    return Compound(children=kids)


# --- geometry helpers ---
def zcyl(d, z0, z1, x=0.0, y=0.0) -> Part:
    return Pos(x, y, (z0 + z1) / 2) * Cylinder(d / 2, z1 - z0)


def box(x0, x1, y0, y1, z0, z1) -> Part:
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def rrect(cx, cy, w, h, r, z0, z1) -> Part:
    return Pos(0, 0, z0) * extrude(Pos(cx, cy) * RectangleRounded(w, h, min(r, w / 2 - 0.01, h / 2 - 0.01)),
                                   amount=z1 - z0)


def polar(r, a_rad, cx=0.0, cy=0.0):
    return (cx + r * math.cos(a_rad), cy + r * math.sin(a_rad))


def sector(r0, r1, a0_deg, a1_deg, z0, z1, n=48) -> Part:
    """Annular sector between angles a0..a1 (degrees, CCW from +X)."""
    a0, a1 = math.radians(a0_deg), math.radians(a1_deg)
    outer = [polar(r1, a0 + (a1 - a0) * i / n) for i in range(n + 1)]
    inner = [polar(r0, a1 - (a1 - a0) * i / n) for i in range(n + 1)]
    return Pos(0, 0, z0) * extrude(Polygon(*outer, *inner, align=None), amount=z1 - z0)


# --- involute spur gears ---
def _inv(a: float) -> float:
    return math.tan(a) - a


def gear_outline(m: float, teeth: int, phase_deg: float = 0.0, backlash: float = 0.15,
                 pa_deg: float = 20.0, n: int = 6) -> list[tuple[float, float]]:
    """Closed outline of an involute spur gear; a tooth is centred on phase_deg."""
    pa = math.radians(pa_deg)
    rp = m * teeth / 2
    rb, ra, rf = rp * math.cos(pa), rp + m, rp - 1.25 * m
    half = (math.pi * m / 2 - backlash) / 2 / rp  # half tooth thickness at the pitch circle, rad
    r_start = max(rb, rf)
    radii = [r_start + (ra - r_start) * i / (n - 1) for i in range(n)]
    flank = [(r, half + _inv(pa) - _inv(math.acos(min(1.0, rb / r)))) for r in radii]
    pts = []
    for k in range(teeth):
        c = math.radians(phase_deg) + 2 * math.pi * k / teeth
        pts.append(polar(rf, c - flank[0][1]))
        pts += [polar(r, c - th) for r, th in flank]
        pts += [polar(r, c + th) for r, th in reversed(flank)]
        pts.append(polar(rf, c + flank[0][1]))
    return pts


def spur_gear(m, teeth, z0, z1, x=0.0, y=0.0, phase_deg=0.0, bore=0.0) -> Part:
    g = Pos(x, y, z0) * extrude(Polygon(*gear_outline(m, teeth, phase_deg), align=None), amount=z1 - z0)
    if bore:
        g -= zcyl(bore, z0 - 1, z1 + 1, x, y)
    return g


def mesh_phases(teeth_a: int, teeth_b: int, angle_ab_deg: float) -> tuple[float, float]:
    """Tooth phases so gear A (a tooth facing B) meshes with gear B (a gap facing A)."""
    return angle_ab_deg, angle_ab_deg + 180.0 + 180.0 / teeth_b


def centre_distance(m, za, zb) -> float:
    return m * (za + zb) / 2


# --- fasteners ---
# Screws thread into one of: a heat-set brass insert in a printed part, a tapped hole
# in the steel back plate, or a bought part's own threads (the motor face). Never into
# bare plastic. Lengths are stock lengths; fix() picks the shortest that engages enough
# and sinks the insert deeper if a stock length would bottom out.
@dataclass(frozen=True)
class ScrewType:
    d: float  # nominal diameter
    clear: float  # clearance hole
    head: str  # "button", "pan" or "csk"
    head_d: float
    head_h: float
    lengths: tuple[float, ...]  # stock lengths (csk: overall, others: under the head)
    desc: str


SCREW_TYPES = {
    "M1.6 pan": ScrewType(1.6, 1.9, "pan", 3.2, 1.3, (2, 2.5, 3, 4, 5, 6, 8), "pan head (ISO 7045)"),
    "M2 button": ScrewType(2.0, 2.4, "button", 3.5, 1.3, (3, 4, 5, 6, 8, 10, 12, 16, 20),
                           "button head hex socket"),
    "M3 button": ScrewType(3.0, 3.4, "button", 5.7, 1.65, (4, 5, 6, 8, 10, 12, 16, 20, 25, 30),
                           "button head hex socket (ISO 7380)"),
    "M3 csk": ScrewType(3.0, 3.4, "csk", 6.72, 1.86, (6, 8, 10, 12, 16, 20, 25, 30),
                        "countersunk hex socket (ISO 10642)"),
}


@dataclass(frozen=True)
class InsertType:
    """Heat-set brass insert. `hole` is the printed hole it is melted into (supplier's figure)."""
    hole: float
    l: float
    min_eng: float  # least thread the screw must reach into it


# CNC Kitchen / Ruthex-style standard inserts. Check the hole size against your supplier.
INSERTS = {"M2": InsertType(3.2, 3.0, 2.5), "M3": InsertType(4.0, 5.7, 4.0)}
INSERT_RESERVE = 1.0  # blind hole below the insert, for displaced plastic and the screw tip
MIN_WALL = 1.2  # around an insert or tapped hole: three 0.4 mm perimeters


def screw_solid(st: ScrewType, length: float) -> Part:
    """Seat at z=0 (button/pan: under the head; csk: the flush top face), shank along -Z."""
    if st.head == "csk":
        head = Pos(0, 0, -st.head_h / 2) * Cone(st.d / 2, st.head_d / 2, st.head_h)
    else:
        head = Pos(0, 0, st.head_h / 2) * Cylinder(st.head_d / 2, st.head_h)
    return head + Pos(0, 0, -length / 2) * Cylinder(st.d / 2, length)


def _zc(d: float, z0: float, z1: float) -> Part:
    return Pos(0, 0, (z0 + z1) / 2) * Cylinder(d / 2, z1 - z0)


@dataclass
class Fixing:
    """One screw: what it clamps, what it threads into, and how far."""
    name: str
    type: str
    length: float
    clamp: float  # seat to the top of the thread (insert top / tapped face)
    into: str
    thread: str  # "insert", "tapped" or "bought"
    eng_range: tuple[float, float]
    probe: Part | None  # a MIN_WALL ring round the thread; must lie inside `into`

    @property
    def engagement(self) -> float:
        return self.length - self.clamp


def fix(u: Unit, name: str, type_: str, loc: Location, clamp: float, into: str, thread: str,
        clamps: list[str], eng: tuple[float, float] | None = None, cbore: float = 0.0,
        tapped_depth: float = 0.0) -> Fixing:
    """Add a screw (and its insert), and cut its holes in every part it passes through.

    loc puts the seat at the origin with the screw pointing along local -Z. `clamp` is the
    distance from the seat to where the thread starts. `cbore` sinks the head that deep
    into clamps[0].
    """
    st = SCREW_TYPES[type_]
    size = type_.split()[0]
    ins = INSERTS[size] if thread == "insert" else None
    lo, hi = (ins.min_eng, ins.l + INSERT_RESERVE - 0.5) if ins else eng
    length = next((L for L in st.lengths if L - clamp >= lo - 1e-6), None)
    assert length is not None, f"{name}: no stock {type_} is long enough for {clamp:.1f} mm"
    recess = max(0.0, length - clamp - hi)
    assert ins or recess < 1e-6, f"{name}: {type_} x {length} would bottom out"
    t = clamp + recess  # the insert's top, below the seat

    clear = _zc(st.clear, -t, 0.01)
    if st.head == "csk":
        clear += Pos(0, 0, -st.head_h / 2) * Cone(st.clear / 2, st.head_d / 2, st.head_h) \
            + _zc(st.head_d, 0, 0.5)
    elif cbore:
        clear += _zc(st.head_d + 0.6, 0, cbore + 0.01)
    for c in clamps:
        u.cut(c, loc * clear)

    u.add(name, loc * screw_solid(st, length), FASTENER, f"{size} x {length:g} {st.desc}")
    u.joint(name, clamps[0], 0.3 if st.head == "csk" else 0.0)
    for c in clamps[1:]:
        u.joint(name, c, 0.3)
    probe = None
    if ins:
        u.cut(into, loc * (_zc(st.clear, -t - 0.01, 0.01) + _zc(ins.hole, -t - ins.l - INSERT_RESERVE, -t)))
        u.add(f"{name}_insert", loc * (_zc(ins.hole, -t - ins.l, -t) - _zc(st.d, -t - ins.l - 1, -t + 1)),
              FASTENER, f"Heat-set insert {size} x {ins.l:g} (hole {ins.hole:g} mm)")
        u.joint(name, f"{name}_insert")
        u.joint(f"{name}_insert", into)
        probe = loc * (_zc(ins.hole + 2 * MIN_WALL, -t - ins.l, -t) - _zc(ins.hole, -t - ins.l - 1, -t + 1))
    else:
        if thread == "tapped":
            u.cut(into, loc * _zc(st.d, -t - tapped_depth - 0.01, -t + 0.01))
            probe = loc * (_zc(st.d + 2 * MIN_WALL, -t - tapped_depth, -t) - _zc(st.d, -t - tapped_depth - 1, -t + 1))
        else:  # the bought part's own threaded hole, as deep as it is
            u.cut(into, loc * _zc(st.d, -t - hi - 0.01, -t + 0.01))
        u.joint(name, into)
    f = Fixing(name, type_, length, t, into, thread, (lo, hi), probe)
    u.fixings.append(f)
    return f


@dataclass
class Snap:
    """A printed cantilever snap: thickness t, free length l, deflection to engage."""
    name: str
    t: float
    l: float
    deflection: float

    @property
    def strain(self) -> float:  # root strain of a uniform cantilever: 1.5 t y / l^2
        return 1.5 * self.t * self.deflection / self.l ** 2


SNAP_STRAIN_LIMIT = 0.025  # PETG/ASA, for snaps opened a few times


def wall_snap(u: Unit, name: str, loc: Location, z0: float, z1: float, wall_t: float = 1.6,
              width: float = 8.0, bump: float = 0.8, bump_h: float = 2.0) -> tuple[Part, Part, Part]:
    """A snap arm cut into a chassis wall, its bump, and the pocket in the cover over it.

    loc puts the wall's outer face on local y=0 with +y outwards. The arm is the wall
    between two slits, from z0 up to the wall's free top edge z1; the bump's flat underside
    holds the cover down, its ramp lets the cover slide on.
    Returns (slits to cut from the chassis, bump to add to it, pocket to cut from the cover).
    """
    slits = Part()
    for sx in (-1, 1):
        x = sx * (width / 2 + 0.3)
        slits += Pos(x, -wall_t / 2, (z0 + z1 + 1) / 2) * Box(0.6, wall_t + 0.2, z1 + 1 - z0)
    zb = z1 - bump_h - 0.8
    prof = Polygon((0, zb), (bump, zb), (bump, zb + 0.4), (0, zb + bump_h), align=None)
    wedge = Pos(-(width / 2 - 0.5), 0, 0) * extrude(Plane.YZ * prof, amount=width - 1.0)
    pocket = Pos(0, 0.5, zb + (bump_h + 0.6) / 2) * Box(width, 1.0, bump_h + 0.6)
    u.snaps.append(Snap(name, wall_t, z1 - z0, bump))
    return loc * slits, loc * wedge, loc * pocket


# --- bought parts ---
N20 = P.MOTOR  # Pololu micro metal gearmotor with encoder (concepts/parts.py)
AA = dict(d=14.5, l=50.5)
CR123A = dict(d=17.0, l=34.5)
CONTACT_T, SPRING_L, SPRING_D = 0.6, 3.0, 6.0
MOTOR_DESC = {
    "n20": "Pololu #5193 gearmotor, 150:1 HPCB 6 V, 12 CPR encoder, side connector (6-pin JST SH cable)",
    "n20-worm": "Pololu #5185 gearmotor, 30:1 HPCB 6 V, 12 CPR encoder, side connector (6-pin JST SH cable)",
}


def n20_standing(x: float, y: float, face_z: float, turn_deg: float = 0.0) -> Part:
    """The motor with its gearbox face down at face_z and the shaft going down; its encoder
    connector faces turn_deg from +Y (the plug goes in from that side)."""
    return P.micro_gearmotor().at(Pos(x, y, face_z) * Rot(0, 0, turn_deg))


def cell_along_y(spec, x, y0, z0) -> Part:
    """Cell lying along +Y from y0 (its negative end), resting on z0."""
    d, l = spec["d"], spec["l"]
    return Pos(x, y0 + l / 2, z0 + d / 2) * Rot(90, 0, 0) * Cylinder(d / 2, l)


# --- the drive module shared by all concepts ---
@dataclass
class DriveLayout:
    gear_z0: float  # bottom of the output-gear layer
    gear_teeth: int  # output gear (module 1)
    motor: str  # "n20" (standing, spur pinion) or "n20-worm" (lying, placed by the concept)
    motor_angle_deg: float  # direction from the bore axis to the motor (spur) / worm axis offset side
    shaft_top: float  # top of the coupler's knob shaft
    lug_angle_deg: float = 90.0  # coupler lug, relative to the tailpiece's wide face
    motor_turn_deg: float = 0.0  # standing motor: which way its encoder connector faces (0 = +Y)
    # The bolt magnet's radius. 10.5 lets the dev kit's SOT-23 adapters (10.16 x 7.62) clear
    # the shaft, so the slab uses it too; the puck's cells leave room for only 8.5.
    hall_r: float = 10.5

    @property
    def flag_d(self) -> float:
        return 2 * (self.hall_r + 2.0)

    @property
    def bridge_z(self) -> tuple[float, float]:  # 3 mm: room to sink the motor screws' heads
        return (self.gear_z0 + 6.5, self.gear_z0 + 9.5)

    @property
    def flag_z(self) -> tuple[float, float]:  # the bolt flag, magnet flush with its top
        top = self.bridge_z[1]
        return (top + 0.5, top + 4.5)

    @property
    def board_bottom(self) -> float:
        """Underside of the board carrying the bolt Hall switches: their SOT-23 bodies (1.12)
        clear the flag by 1.38, which puts the Hall element 1.85 above the magnet."""
        return self.flag_z[1] + 2.5

    def hall_xy(self, p: DeadboltParams, locked: bool) -> tuple[float, float]:
        """Where the flag's magnet sits with the bolt locked / unlocked: put a Hall switch there."""
        return polar(self.hall_r, math.radians(_tailpiece_angle(p, locked) + FLAG_DEG))


SHAFT_FLAT = 1.2  # depth of the flat on the coupler's knob shaft


def d_shaft(angle_deg: float, z0: float, z1: float) -> Part:
    """The coupler's knob shaft: a D, so the knob and encoder gear are keyed to it."""
    flat = box(SHAFT_D / 2 - SHAFT_FLAT, SHAFT_D, -SHAFT_D, SHAFT_D, z0 - 1, z1 + 1)
    return Rot(0, 0, angle_deg) * (zcyl(SHAFT_D, z0, z1) - flat)


FLAG_DEG = 225.0  # the flag's magnet, relative to the tailpiece's wide face
# The magnet's radius is DriveLayout.hall_r; the flag reaches 2 mm past the magnet.
BOLT_MAGNET = dict(d=3.0, h=2.0)  # axial NdFeB, a face towards the Hall switches

M_OUT, PINION_TEETH = 1.0, 10
HUB_D, SHAFT_D, BORE_CLEAR = 16.0, 12.0, 0.4
LUG_R, LUG_W_DEG, POCKET_R = 13.0, 30.0, 13.6
POCKET_MARGIN_DEG = 6.0


def drive_module(u: Unit, p: DeadboltParams, locked: bool, L: DriveLayout) -> None:
    """Coupler, output gear, motor + pinion (or worm), bolt flag + magnet, knob shaft.

    The bridge, posts, PCB and knob are concept-specific and added by the concept.
    """
    g0, g1 = L.gear_z0, L.gear_z0 + 6.0
    b0, b1 = L.bridge_z
    e0, e1 = L.flag_z
    tail_ang = _tailpiece_angle(p, locked)

    # Coupler: hub through floor/gear/bridge, lug in the gear pocket, D-shaft for the flag and knob.
    hub = zcyl(HUB_D, 3.3, b1)
    lug_mid = tail_ang + L.lug_angle_deg
    lug = sector(HUB_D / 2 - 0.2, LUG_R, lug_mid - LUG_W_DEG / 2, lug_mid + LUG_W_DEG / 2, g0 + 0.3, g1 - 0.3)
    u.shaft_angle = tail_ang
    shaft = d_shaft(tail_ang, b1, L.shaft_top)
    coupler = hub + lug + shaft
    slot = Pos(0, 0, (3.3 + p.tailpiece_protrusion + 1) / 2) * Box(
        p.tailpiece_w + 0.4, p.tailpiece_t + 0.4, p.tailpiece_protrusion + 1 - 3.3 + 0.02)
    coupler -= Rot(0, 0, tail_ang) * slot  # the knob screw's insert is cut by knob_on_shaft
    u.add("coupler", coupler, PRINTED, "Coupler / tailpiece adapter (per brand), PA12 or PETG-CF")

    # Output gear: pocket spans the lug's travel in both bolt states, plus margin each side.
    locked_lug = _tailpiece_angle(p, True) + L.lug_angle_deg
    unlocked_lug = _tailpiece_angle(p, False) + L.lug_angle_deg
    pocket_mid = (locked_lug + unlocked_lug) / 2
    pocket_half = abs(unlocked_lug - locked_lug) / 2 + LUG_W_DEG / 2 + POCKET_MARGIN_DEG
    if L.motor == "n20-worm":
        driver_angle = 90.0  # worm sits along X at +Y
    else:
        driver_angle = L.motor_angle_deg
    gear_phase, pin_phase = mesh_phases(L.gear_teeth, PINION_TEETH, driver_angle)
    ring = spur_gear(M_OUT, L.gear_teeth, g0, g1, phase_deg=gear_phase, bore=HUB_D + BORE_CLEAR)
    ring -= sector(HUB_D / 2 - 0.5, POCKET_R, pocket_mid - pocket_half, pocket_mid + pocket_half, g0 - 1, g1 + 1)
    u.add("ring_gear", ring, PRINTED, f"Output gear, m1 {L.gear_teeth}T, lost-motion pocket {2 * pocket_half:.0f} deg")
    u.joint("ring_gear", "coupler", 0.5)  # rides on the hub with running clearance

    # Motor and its driver gear.
    if L.motor == "n20":
        cd = centre_distance(M_OUT, L.gear_teeth, PINION_TEETH)
        mx, my = polar(cd, math.radians(L.motor_angle_deg))
        u.add("motor", n20_standing(mx, my, b1, L.motor_turn_deg), BOUGHT, MOTOR_DESC["n20"])
        motor_face_screws(u, L, mx, my)
        pin = spur_gear(M_OUT, PINION_TEETH, g0, g1, mx, my, phase_deg=pin_phase, bore=N20["shaft_d"])
        u.add("pinion", pin, PRINTED, f"Motor pinion, m1 {PINION_TEETH}T (or brass, pressed)")
        u.joint("pinion", "motor")
        u.meshes.add(frozenset({"pinion", "ring_gear"}))
        u.motor_xy = (mx, my)
    # The worm variant is placed by its concept (it lies along X and needs its own mounts).

    u.layout = L
    # A flag on the D-shaft carries a magnet past the bolt Hall switches on the board above.
    flag = zcyl(L.flag_d, e0, e1) - d_shaft(tail_ang, e0 - 1, e1 + 1)
    mx_, my_ = polar(L.hall_r, math.radians(tail_ang + FLAG_DEG))
    flag -= zcyl(BOLT_MAGNET["d"], e1 - BOLT_MAGNET["h"], e1 + 1, mx_, my_)
    u.add("bolt_flag", flag, PRINTED, "Bolt flag (magnet carrier) on the coupler's D-shaft")
    u.joint("bolt_flag", "coupler")
    u.add("bolt_magnet", zcyl(BOLT_MAGNET["d"], e1 - BOLT_MAGNET["h"], e1, mx_, my_), BOUGHT,
          "NdFeB magnet 3 x 2 mm, axial (bolt position)")
    u.joint("bolt_magnet", "bolt_flag")


def bridge_cuts(u: Unit, L: DriveLayout) -> Part:
    """Holes every bridge needs: coupler, motor shaft. Screws cut their own."""
    b0, b1 = L.bridge_z
    cut = zcyl(HUB_D + BORE_CLEAR, b0 - 1, b1 + 1)
    if u.motor_xy:
        mx, my = u.motor_xy
        cut += zcyl(6.0, b0 - 1, b1 + 1, mx, my)  # motor shaft + pinion hub clearance
    return cut


# Pololu drawing: 2x M1.6, 9 apart across the 12 mm width. The thread depth is not drawn
# (UNMEASURED, typical 1.2-2.0).
N20_FACE_HOLES = ("M1.6 pan", N20["face_holes"], (1.2, 2.0))


def motor_face_screws(u: Unit, L: DriveLayout, mx: float, my: float) -> None:
    """Two screws up through the bridge into the motor's gearbox face, heads sunk in the bridge."""
    b0, b1 = L.bridge_z
    type_, spacing, eng = N20_FACE_HOLES
    sink = SCREW_TYPES[type_].head_h + 0.1
    for i, s in enumerate((-1, 1)):
        loc = Pos(mx + s * spacing / 2, my, b0 + sink) * Rot(180, 0, 0)
        fix(u, f"screw_motor_{i}", type_, loc, b1 - (b0 + sink), "motor", "bought", ["bridge"],
            eng=eng, cbore=sink)


def bridge_extras(u: Unit, L: DriveLayout) -> Part:
    """A sleeve on the bridge round the standing motor's gearbox, so it cannot turn."""
    b0, b1 = L.bridge_z
    extra = Part()
    if L.motor == "n20":
        mx, my = u.motor_xy
        w, t = N20["w"], N20["t"]
        sleeve = box(mx - w / 2 - 1.6, mx + w / 2 + 1.6, my - t / 2 - 1.6, my + t / 2 + 1.6, b1, b1 + 7)
        sleeve -= box(mx - w / 2, mx + w / 2, my - t / 2, my + t / 2, b1 - 1, b1 + 8)
        extra += sleeve
    return extra


def add_stack_screw(u: Unit, name: str, x: float, y: float, head_z: float, post_top: float,
                    into: str, through: list[str]) -> None:
    """M2 down from head_z, clamping `through` parts onto an insert in the post top at post_top."""
    fix(u, name, "M2 button", Pos(x, y, head_z), head_z - post_top, into, "insert", through)


def knob_on_shaft(u: Unit, name: str, desc: str, outer: Part, bottom_z: float, shaft_top: float) -> None:
    """A knob (or cap) keyed on the coupler shaft, held by an axial M3 screw."""
    top = outer.bounding_box().max.Z
    seat = top - 2.0
    u.add(name, outer - d_shaft(u.shaft_angle, bottom_z - 1, shaft_top), PRINTED, desc)
    u.joint(name, "coupler")
    fix(u, "screw_knob", "M3 button", Pos(0, 0, seat), seat - shaft_top, "coupler", "insert", [name],
        cbore=2.0)


def back_plate(u: Unit, p: DeadboltParams, outline: Part, chassis_screws: list[tuple[float, float]]) -> None:
    """3 mm steel plate on the door: countersunk for the deadbolt's screws, tapped M3 for the chassis."""
    door = ScrewType(p.post_hole_d, p.post_hole_d, "csk", 7.5, 2.3, (), "")  # thread per brand
    plate = outline - zcyl(10.0, -1, 4)
    for x in _post_xs(p):
        plate -= Pos(x, 0, 3.0) * (Pos(0, 0, -door.head_h / 2) * Cone(door.d / 2, door.head_d / 2, door.head_h)
                                   + _zc(door.d, -4, 0.01))
        s = Pos(x, 0, 3.0) * screw_solid(door, 22.0)
        u.add(f"screw_door_{'r' if x > 0 else 'l'}", s, FASTENER,
              "Deadbolt through-screw, flat head (thread per brand, e.g. #8-32 x 1\")")
        u.joint(f"screw_door_{'r' if x > 0 else 'l'}", "plate")
        u.joint(f"screw_door_{'r' if x > 0 else 'l'}", "door")
    u.add("plate", plate, METAL, "Back plate, 3 mm steel, laser cut, 2x M3 tapped")
    u.joint("plate", "door")


PLATE_T = 3.0


def chassis_screws(u: Unit, positions: list[tuple[float, float]], floor_top: float) -> None:
    """M3 down through the chassis floor into the plate's tapped holes. The door is behind the
    plate, so the screw may reach through the plate but no further."""
    for i, (x, y) in enumerate(positions):
        fix(u, f"screw_chassis_{i}", "M3 button", Pos(x, y, floor_top), floor_top - PLATE_T, "plate",
            "tapped", ["chassis"], eng=(2.4, PLATE_T), tapped_depth=PLATE_T)


COVER_WALL = 2.0


def cover_screw(u: Unit, i: int, x: float, y_outer: float, z: float) -> None:
    """Countersunk M3 through the cover's bottom wall (outer face at y_outer), going +Y into an
    insert set in the chassis wall from outside (before the cover goes on)."""
    fix(u, f"screw_cover_{i}", "M3 csk", Pos(x, y_outer, z) * Rot(90, 0, 0), COVER_WALL, "chassis",
        "insert", ["cover"])


CHIPS = {  # name on a board -> (model, BOM line)
    "mcu": (P.esp32_c6_mini_1, "ESP32-C6-MINI-1-N4 module (Thread + BLE), antenna keep-out under its end"),
    "driver": (P.drv8833_pwp, "DRV8833PWPR dual H-bridge, HTSSOP-16"),
    "hall_locked": (P.drv5032_dbz, "DRV5032FADBZR Hall switch, SOT-23: bolt locked (underside)"),
    "hall_unlocked": (P.drv5032_dbz, "DRV5032FADBZR Hall switch, SOT-23: bolt unlocked (underside)"),
    "hall_door": (P.drv5032_dbz, "DRV5032FADBZR Hall switch, SOT-23: door position (magnet on the frame)"),
    "regulator": (P.tps629206_drl, "TPS629206DRLR 3.3 V buck, SOT-583 (3-17 V in, 4 uA)"),
    "motor_conn": (P.jst_sh6_side, "JST SM06B-SRSS-TB, 6-pin SH side entry (motor + encoder cable)"),
}


def bolt_hall_blocks(L: DriveLayout, p: DeadboltParams) -> dict[str, tuple]:
    """The two bolt-position Hall switches, for pcb_parts: under the board, over the flag's
    magnet at the locked and unlocked angles."""
    return {name: (x, y, 0.0, True) for name, locked in (("hall_locked", True), ("hall_unlocked", False))
            for x, y in [L.hall_xy(p, locked)]}


def pcb_parts(u: Unit, prefix: str, board_top: float, blocks: dict[str, tuple]) -> None:
    """Parts on a board. A chip from CHIPS: name -> (x, y, rot_deg[, underside]). Anything
    else is an envelope: name -> (x0, x1, y0, y1, height[, underside])."""
    bottom = board_top - 1.6
    for name, spec in blocks.items():
        n = f"{prefix}_{name}"
        if name in CHIPS:
            x, y, rot = spec[:3]
            under = len(spec) > 3 and spec[3]
            make, desc = CHIPS[name]
            loc = Pos(x, y, bottom) * Rot(180, 0, rot) if under else Pos(x, y, board_top) * Rot(0, 0, rot)
            u.add(n, make().at(loc), BOUGHT, desc)
        else:
            x0, x1, y0, y1, h = spec[:5]
            under = len(spec) > 5 and spec[5]
            z0, z1 = (bottom - h, bottom) if under else (board_top, board_top + h)
            u.add(n, box(x0, x1, y0, y1, z0, z1), BOUGHT, COMPONENT_DESC.get(name, name))
        u.joint(n, prefix)


COMPONENT_DESC = {
    "mcu_chip": "ESP32-C6 QFN40 5 x 5 (chip-down: the same chip as the MINI-1 module)",
    "antenna": "2.4 GHz chip antenna", "led": "Status RGB LED", "button": "Pairing / reset button",
    "conn": "Battery connector",
}
