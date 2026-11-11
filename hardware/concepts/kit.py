"""Parts shared by the lock-unit concepts: geometry helpers, bought parts, fasteners,
gears and the drive module that every concept uses.

Units are mm in the fixture frame (deadbolt/fixture.py): origin on the bore axis at
the interior door face, +Z into the room, +X towards the latch edge, +Y up.

Bought parts (motors, cells, magnet, chips, screws) are datasheet-typical envelopes,
not vendor CAD. Printed parts are the ones we design and are exported as STL.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from build123d import (Box, Color, Cone, Cylinder, Part, Polygon, Pos, RectangleRounded, Rot,
                       extrude)

from deadbolt.fixture import DeadboltParams, _post_xs, _tailpiece_angle

# --- kinds: decide colour, BOM section and whether we export an STL ---
PRINTED, METAL, BOUGHT, FASTENER, CELL, PCB = "printed", "metal", "bought", "fastener", "cell", "pcb"
KIND_COLORS = {
    PRINTED: Color(0.92, 0.92, 0.9), METAL: Color(0.6, 0.62, 0.66), BOUGHT: Color(0.85, 0.5, 0.15),
    FASTENER: Color(0.25, 0.25, 0.28), CELL: Color(0.2, 0.7, 0.35), PCB: Color(0.1, 0.45, 0.2),
}
ACCENT = {  # printed parts that should stand out from the shell
    "coupler": Color(0.2, 0.45, 0.85), "ring_gear": Color(0.3, 0.65, 0.95),
    "pinion": Color(0.95, 0.75, 0.2), "worm": Color(0.95, 0.75, 0.2),
    "enc_drive": Color(0.55, 0.35, 0.8), "enc_idler": Color(0.55, 0.35, 0.8),
    "knob": Color(0.25, 0.25, 0.28), "bridge": Color(0.75, 0.8, 0.85),
    "chassis": Color(0.82, 0.84, 0.86), "light_pipe": Color(0.95, 0.95, 1.0),
    "magnet": Color(0.7, 0.1, 0.1), "battery_door": Color(0.85, 0.85, 0.82),
}


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

    def add(self, name: str, part: Part, kind: str, desc: str) -> Part:
        assert name not in self.items, name
        part.label = name
        part.color = ACCENT.get(name) or KIND_COLORS[kind]
        self.items[name] = Item(part, kind, desc)
        return part

    def joint(self, a: str, b: str, max_gap: float = 0.0) -> None:
        self.joints.append((a, b, max_gap))

    def parts(self) -> dict[str, Part]:
        return {k: v.part for k, v in self.items.items()}


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


# --- fasteners: built head-up with the head's top face at z=0, shank along -Z ---
SCREWS = {  # d, head_d, head_h, countersunk
    "M1.6": (1.6, 3.0, 1.0, False), "M2": (2.0, 3.8, 1.3, False), "M3": (3.0, 5.7, 1.65, False),
    "M3 CSK": (3.0, 6.0, 1.7, True), "#8-32 CSK": (4.17, 7.5, 2.3, True),
}


def screw(size: str, length: float) -> Part:
    d, hd, hh, csk = SCREWS[size]
    head = Pos(0, 0, -hh / 2) * (Cone(d / 2, hd / 2, hh) if csk else Cylinder(hd / 2, hh))
    shank = Pos(0, 0, -hh - length / 2) * Cylinder(d / 2, length)
    return head + shank


def csk_hole(size: str, through_z0: float, top_z: float, x: float, y: float, clearance=0.0) -> Part:
    """Cut for a countersunk head sitting flush at top_z, plus its shank through to through_z0."""
    d, hd, hh, _ = SCREWS[size]
    return (Pos(x, y, top_z - hh / 2) * Cone(d / 2 + clearance, hd / 2 + clearance, hh)
            + zcyl(d + clearance, through_z0, top_z, x, y))


# --- bought parts ---
N20 = dict(w=12.0, t=10.0, gearbox=9.0, can=15.0, shaft_d=3.0)  # w along local X
GM16 = dict(d=16.0, l=34.0, shaft_d=4.0)
AA = dict(d=14.5, l=50.5)
CR123A = dict(d=17.0, l=34.5)
MAGNET = dict(d=6.0, h=2.5)  # diametric, for the AS5600
CONTACT_T, SPRING_L, SPRING_D = 0.6, 3.0, 6.0


def n20_standing(x, y, face_z, shaft_len) -> Part:
    """N20 with its gearbox face down at face_z and the shaft going down."""
    gb = box(x - N20["w"] / 2, x + N20["w"] / 2, y - N20["t"] / 2, y + N20["t"] / 2,
             face_z, face_z + N20["gearbox"])
    can = zcyl(N20["w"], face_z + N20["gearbox"], face_z + N20["gearbox"] + N20["can"], x, y) \
        & box(x - 7, x + 7, y - N20["t"] / 2, y + N20["t"] / 2, face_z, face_z + 40)
    return gb + can + zcyl(N20["shaft_d"], face_z - shaft_len, face_z, x, y)


def gm16_standing(x, y, face_z, shaft_len) -> Part:
    return zcyl(GM16["d"], face_z, face_z + GM16["l"], x, y) + \
        zcyl(GM16["shaft_d"], face_z - shaft_len, face_z, x, y)


def cell_along_y(spec, x, y0, z0) -> Part:
    """Cell lying along +Y from y0 (its negative end), resting on z0."""
    d, l = spec["d"], spec["l"]
    return Pos(x, y0 + l / 2, z0 + d / 2) * Rot(90, 0, 0) * Cylinder(d / 2, l)


# --- the drive module shared by all concepts ---
@dataclass
class DriveLayout:
    gear_z0: float  # bottom of the output-gear layer
    gear_teeth: int  # output gear (module 1)
    motor: str  # "n20", "gm16" or "n20-worm"
    motor_angle_deg: float  # direction from the bore axis to the motor (spur) / worm axis offset side
    encoder_angle_deg: float  # direction from the bore axis to the encoder idler
    shaft_top: float  # top of the coupler's knob shaft
    lug_angle_deg: float = 90.0  # coupler lug, relative to the tailpiece's wide face

    @property
    def bridge_z(self) -> tuple[float, float]:
        return (self.gear_z0 + 6.5, self.gear_z0 + 8.5)

    @property
    def enc_z(self) -> tuple[float, float]:
        top = self.bridge_z[1]
        return (top + 0.5, top + 4.5)

    @property
    def encoder_xy(self) -> tuple[float, float]:
        return polar(16.0, math.radians(self.encoder_angle_deg))

    @property
    def chip_z(self) -> tuple[float, float]:  # AS5600 body, 1 mm above the magnet
        return (self.enc_z[1] + 1.0, self.enc_z[1] + 2.5)


M_OUT, PINION_TEETH = 1.0, 10
M_ENC, ENC_TEETH = 0.8, 20
HUB_D, SHAFT_D, BORE_CLEAR = 16.0, 12.0, 0.4
LUG_R, LUG_W_DEG, POCKET_R = 13.0, 30.0, 13.6
POCKET_MARGIN_DEG = 6.0


def drive_module(u: Unit, p: DeadboltParams, locked: bool, L: DriveLayout) -> None:
    """Coupler, output gear, motor + pinion (or worm), encoder gears + magnet, knob shaft.

    The bridge, posts, PCB and knob are concept-specific and added by the concept.
    """
    g0, g1 = L.gear_z0, L.gear_z0 + 6.0
    b0, b1 = L.bridge_z
    e0, e1 = L.enc_z
    tail_ang = _tailpiece_angle(p, locked)

    # Coupler: hub through floor/gear/bridge, lug in the gear pocket, D-shaft for encoder gear and knob.
    hub = zcyl(HUB_D, 3.3, b1)
    lug_mid = tail_ang + L.lug_angle_deg
    lug = sector(HUB_D / 2 - 0.2, LUG_R, lug_mid - LUG_W_DEG / 2, lug_mid + LUG_W_DEG / 2, g0 + 0.3, g1 - 0.3)
    shaft = zcyl(SHAFT_D, b1, L.shaft_top)
    coupler = hub + lug + shaft
    slot = Pos(0, 0, (3.3 + p.tailpiece_protrusion + 1) / 2) * Box(
        p.tailpiece_w + 0.4, p.tailpiece_t + 0.4, p.tailpiece_protrusion + 1 - 3.3 + 0.02)
    coupler -= Rot(0, 0, tail_ang) * slot
    coupler -= zcyl(3.0, L.shaft_top - 8.0, L.shaft_top + 1)  # tapped for the knob screw
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
    if L.motor in ("n20", "gm16"):
        cd = centre_distance(M_OUT, L.gear_teeth, PINION_TEETH)
        mx, my = polar(cd, math.radians(L.motor_angle_deg))
        shaft_len = b1 - (g0 + 0.5)
        if L.motor == "n20":
            motor = n20_standing(mx, my, b1, shaft_len)
            desc = "N20 gear motor, 6 V, ~150:1, 3 mm D-shaft"
        else:
            motor = gm16_standing(mx, my, b1, shaft_len)
            desc = "16 mm planetary gear motor, 6 V, ~100:1, 4 mm D-shaft"
        u.add("motor", motor, BOUGHT, desc)
        sd = N20["shaft_d"] if L.motor == "n20" else GM16["shaft_d"]
        pin = spur_gear(M_OUT, PINION_TEETH, g0, g1, mx, my, phase_deg=pin_phase, bore=sd)
        u.add("pinion", pin, PRINTED, f"Motor pinion, m1 {PINION_TEETH}T (or brass, pressed)")
        u.joint("pinion", "motor")
        u.meshes.add(frozenset({"pinion", "ring_gear"}))
        u.motor_xy = (mx, my)
    # The worm variant is placed by its concept (it lies along X and needs its own mounts).

    # Encoder: a 1:1 pair turns a diametric magnet on the idler, read by an AS5600 above it.
    ex, ey = L.encoder_xy
    d_phase, i_phase = mesh_phases(ENC_TEETH, ENC_TEETH, L.encoder_angle_deg)
    enc_drive = spur_gear(M_ENC, ENC_TEETH, e0, e1, phase_deg=d_phase, bore=SHAFT_D)
    u.add("enc_drive", enc_drive, PRINTED, "Encoder drive gear, m0.8 20T, pressed on the coupler")
    u.joint("enc_drive", "coupler")
    idler = spur_gear(M_ENC, ENC_TEETH, e0, e1, ex, ey, phase_deg=i_phase, bore=3.0)
    idler -= zcyl(MAGNET["d"], e1 - 2.0, e1 + 1, ex, ey)
    u.add("enc_idler", idler, PRINTED, "Encoder idler gear, m0.8 20T, holds the magnet")
    u.meshes.add(frozenset({"enc_drive", "enc_idler"}))
    u.add("enc_pin", zcyl(3.0, b0, e1 - 2.0, ex, ey), METAL, "Dowel pin 3 x 8 mm, steel")
    u.joint("enc_idler", "enc_pin")
    u.add("magnet", zcyl(MAGNET["d"], e1 - 2.0, e1 - 2.0 + MAGNET["h"], ex, ey), BOUGHT,
          "Diametric NdFeB magnet 6 x 2.5 mm (AS5600)")
    u.joint("magnet", "enc_idler")
    u.layout = L


def bridge_cuts(u: Unit, L: DriveLayout, posts: list[tuple[float, float]]) -> Part:
    """Holes every bridge needs: coupler, motor shaft, encoder pin boss seat, post screws."""
    b0, b1 = L.bridge_z
    cut = zcyl(HUB_D + BORE_CLEAR, b0 - 1, b1 + 1) + zcyl(3.0, b0 - 1, b1 + 1, *L.encoder_xy)
    if u.motor_xy:
        mx, my = u.motor_xy
        cut += zcyl(6.0, b0 - 1, b1 + 1, mx, my)  # motor shaft + pinion hub clearance
    for x, y in posts:
        cut += zcyl(2.2, b0 - 1, b1 + 1, x, y)
    return cut


def bridge_extras(u: Unit, L: DriveLayout) -> Part:
    """Bosses on the bridge: encoder pin seat, motor sleeve."""
    b0, b1 = L.bridge_z
    e0, _ = L.enc_z
    ex, ey = L.encoder_xy
    extra = zcyl(6.0, b1, e0, ex, ey) - zcyl(3.0, b0 - 1, e0 + 1, ex, ey)
    if L.motor == "n20":
        mx, my = u.motor_xy
        w, t = N20["w"], N20["t"]
        sleeve = box(mx - w / 2 - 1.6, mx + w / 2 + 1.6, my - t / 2 - 1.6, my + t / 2 + 1.6, b1, b1 + 7)
        sleeve -= box(mx - w / 2, mx + w / 2, my - t / 2, my + t / 2, b1 - 1, b1 + 8)
        extra += sleeve
    elif L.motor == "gm16":
        mx, my = u.motor_xy
        extra += zcyl(GM16["d"] + 3.2, b1, b1 + 6, mx, my) - zcyl(GM16["d"], b1 - 1, b1 + 7, mx, my)
    return extra


def add_stack_screw(u: Unit, name: str, x: float, y: float, head_z: float, tip_z: float,
                    into: str, through: list[str]) -> None:
    """M2 screw from head_z down to tip_z, clamping `through` parts into `into`."""
    u.add(name, Pos(x, y, head_z + SCREWS["M2"][2]) * screw("M2", head_z - tip_z), FASTENER,
          f"M2 x {math.ceil(head_z - tip_z)} socket button screw")
    u.joint(name, into)
    for t in through:
        u.joint(name, t, 0.3)


def knob_on_shaft(u: Unit, name: str, desc: str, outer: Part, bottom_z: float, shaft_top: float) -> None:
    """A knob (or cap) keyed on the coupler shaft, held by an axial M3 screw."""
    top = outer.bounding_box().max.Z
    knob = outer - zcyl(SHAFT_D, bottom_z - 1, shaft_top) - zcyl(6.2, top - 2.0, top + 1) \
        - zcyl(3.4, shaft_top - 1, top)
    u.add(name, knob, PRINTED, desc)
    u.joint(name, "coupler")
    s = Pos(0, 0, top - 2.0 + SCREWS["M3"][2]) * screw("M3", (top - 2.0) - (shaft_top - 6.0))
    u.add("screw_knob", s, FASTENER, "M3 x 8 socket button screw (knob)")
    u.joint("screw_knob", "coupler")
    u.joint("screw_knob", name)


def back_plate(u: Unit, p: DeadboltParams, outline: Part, chassis_screws: list[tuple[float, float]]) -> None:
    """3 mm steel plate on the door: countersunk for the deadbolt's screws, tapped M3 for the chassis."""
    SCREWS["door"] = (p.post_hole_d, 7.5, 2.3, True)  # thread per brand; shank = the post's hole
    plate = outline - zcyl(10.0, -1, 4)
    for x in _post_xs(p):
        plate -= csk_hole("door", -1, 3.0, x, 0)
        s = Pos(x, 0, 3.0) * screw("door", 22.0 - 2.3)
        u.add(f"screw_door_{'r' if x > 0 else 'l'}", s, FASTENER,
              "Deadbolt through-screw, flat head (thread per brand, e.g. #8-32 x 1\")")
        u.joint(f"screw_door_{'r' if x > 0 else 'l'}", "plate")
        u.joint(f"screw_door_{'r' if x > 0 else 'l'}", "door")
    for x, y in chassis_screws:
        plate -= zcyl(3.0, -1, 4, x, y)
    u.add("plate", plate, METAL, "Back plate, 3 mm steel/aluminium, laser cut, 2x M3 tapped")
    u.joint("plate", "door")


def chassis_screws(u: Unit, positions: list[tuple[float, float]], floor_top: float) -> None:
    for i, (x, y) in enumerate(positions):
        n = f"screw_chassis_{i}"
        u.add(n, Pos(x, y, floor_top + SCREWS["M3"][2]) * screw("M3", floor_top), FASTENER,
              "M3 x 5 socket button screw (chassis to plate)")
        u.joint(n, "chassis")
        u.joint(n, "plate")


def cover_screw(u: Unit, i: int, x: float, y_outer: float, z: float, length: float = 8.0) -> None:
    """Countersunk M3 entering the cover's bottom face (at y_outer) going +Y into a chassis boss."""
    n = f"screw_cover_{i}"
    s = Pos(x, y_outer, z) * Rot(90, 0, 0) * screw("M3 CSK", length)
    u.add(n, s, FASTENER, f"M3 x {length:.0f} countersunk screw (cover)")
    u.joint(n, "cover")
    u.joint(n, "chassis")


def cover_screw_cut(x: float, y_outer: float, z: float, length: float = 8.0) -> Part:
    d, hd, hh, _ = SCREWS["M3 CSK"]
    return Pos(x, y_outer, z) * Rot(90, 0, 0) * (
        Pos(0, 0, -hh / 2) * Cone(d / 2 + 0.2, hd / 2 + 0.2, hh)
        + Pos(0, 0, -hh - length / 2) * Cylinder(d / 2, length))


def pcb_parts(u: Unit, prefix: str, board_top: float, blocks: dict[str, tuple]) -> None:
    """Component blocks on a board: name -> (x0, x1, y0, y1, height[, underside])."""
    for name, spec in blocks.items():
        x0, x1, y0, y1, h = spec[:5]
        under = len(spec) > 5 and spec[5]
        z0, z1 = (board_top - 1.6 - h, board_top - 1.6) if under else (board_top, board_top + h)
        n = f"{prefix}_{name}"
        u.add(n, box(x0, x1, y0, y1, z0, z1), BOUGHT, COMPONENT_DESC.get(name, name))
        u.joint(n, prefix)


COMPONENT_DESC = {
    "mcu": "ESP32-C6-MINI-1 module (Thread + BLE)", "mcu_chip": "ESP32-C6 QFN (chip-down)", "antenna": "2.4 GHz chip antenna",
    "driver": "DRV8833 H-bridge", "as5600": "AS5600 magnetic encoder (on the board's underside)",
    "hall": "Door-position hall sensor (magnet on the frame)", "led": "Status RGB LED",
    "button": "Pairing / reset button", "conn": "Battery / motor connector",
}
