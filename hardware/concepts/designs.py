"""Lock-unit concepts, as assemblies of real parts on the deadbolt fixture.

Every concept shares the drive module in concepts/kit.py: a coupler over the
tailpiece, an m1 output gear with a lost-motion pocket, a motor pinion (or worm),
a 1:1 encoder gear pair turning a magnet under an AS5600, and a knob on the
coupler's shaft. What differs is the layout around it: chassis, cover, battery
bay, PCB and motor.

Assembly order (all three): screw the steel back plate to the deadbolt's posts;
fit coupler + output gear; drop the chassis over them and screw it to the plate;
press the motor into the bridge, fit the bridge and encoder gears; screw the PCB
stack down; fit the cover and knob.

Frame and units are the fixture's (deadbolt/fixture.py): origin on the bore axis
at the interior door face, +Z into the room, +X towards the latch edge, +Y up, mm.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Callable

from build123d import Compound, Cylinder, Part, Pos, Rot

from concepts.kit import (AA, BOUGHT, CELL, CONTACT_T, CR123A, METAL, N20, PCB, PRINTED,
                          SPRING_D, SPRING_L, DriveLayout, Unit, add_stack_screw, back_plate, box,
                          bridge_cuts, bridge_extras, cell_along_y, chassis_screws, cover_screw,
                          cover_screw_cut, drive_module, knob_on_shaft, pcb_parts, polar, rrect,
                          zcyl)
from deadbolt.fixture import DeadboltParams

FLOOR = (3.0, 5.0)  # chassis floor, sitting on the 3 mm back plate


def _xcyl(d, x0, x1, y, z) -> Part:
    return Pos((x0 + x1) / 2, y, z) * Rot(0, 90, 0) * Cylinder(d / 2, x1 - x0)


def _ycyl(d, y0, y1, x, z) -> Part:
    return Pos(x, (y0 + y1) / 2, z) * Rot(90, 0, 0) * Cylinder(d / 2, y1 - y0)


# --- shared building blocks for the concepts ---
def cells_along_y(u: Unit, spec: dict, xs: list[float], y0: float, z0: float, holder: str) -> float:
    """Cells lying along +Y: a + contact on the holder face at y0, the cell, a spring, a - contact.

    Returns the y of the far holder face the - contacts sit against.
    """
    d, l = spec["d"], spec["l"]
    zc = z0 + d / 2
    for i, x in enumerate(xs):
        y = y0
        u.add(f"contact_pos_{i}", box(x - 4.5, x + 4.5, y, y + CONTACT_T, zc - 5, zc + 5), METAL,
              "Battery contact, + (nickel-plated steel tab)")
        y += CONTACT_T
        u.add(f"cell_{i}", cell_along_y(spec, x, y, z0), CELL, spec["desc"])
        y += l
        u.add(f"spring_{i}", _ycyl(SPRING_D, y, y + SPRING_L, x, zc), METAL,
              "Battery spring contact, - (conical, shown compressed)")
        y += SPRING_L
        u.add(f"contact_neg_{i}", box(x - 4.5, x + 4.5, y, y + CONTACT_T, zc - 5, zc + 5), METAL,
              "Battery contact plate, - (nickel-plated steel)")
        for a, b in ((f"contact_pos_{i}", holder), (f"cell_{i}", f"contact_pos_{i}"),
                     (f"spring_{i}", f"cell_{i}"), (f"spring_{i}", f"contact_neg_{i}"),
                     (f"contact_neg_{i}", holder), (f"cell_{i}", holder)):
            u.joint(a, b)
    return y0 + 2 * CONTACT_T + l + SPRING_L


def spacers_and_stack(u: Unit, posts, bottom_z, board: str, board_top: float, tip_z: float) -> None:
    """Spacer tubes from the bridge to a board; one long M2 per post clamps board + bridge to the chassis."""
    for i, (x, y) in enumerate(posts):
        s = f"spacer_{i}"
        u.add(s, zcyl(5.0, bottom_z, board_top - 1.6, x, y) - zcyl(2.2, bottom_z - 1, board_top, x, y),
              PRINTED, "PCB spacer tube, 5 mm OD")
        u.joint(s, "bridge")
        u.joint(s, board)
        add_stack_screw(u, f"screw_stack_{i}", x, y, board_top, tip_z, "chassis", [board, s, "bridge"])


def light_pipe(u: Unit, x: float, y: float, z0: float, z1: float, through: str) -> None:
    u.add("light_pipe", zcyl(3.0, z0, z1, x, y), PRINTED, "Light pipe, clear PMMA/PC, 3 mm")
    u.joint("light_pipe", "pcb_led")
    u.joint("light_pipe", through)


def chassis_shell(outline, plate_outline, wall_top: float, wall: float = 1.6) -> Part:
    """Floor on the plate, a skirt around the plate down to the door, and a wall."""
    floor = outline(*FLOOR, 0.0)
    skirt = outline(0.0, FLOOR[0], 0.0) - plate_outline(-1.0, FLOOR[0] + 1, -0.3)
    walls = outline(FLOOR[1], wall_top, 0.0) - outline(FLOOR[1] - 1, wall_top + 1, wall)
    return floor + skirt + walls


def chassis_posts(chassis: Part, posts, top_z: float, shoulder: bool = False) -> Part:
    for x, y in posts:
        if shoulder:  # a PCB rests on the d8 shoulder
            chassis += zcyl(8.0, FLOOR[1], 6.0, x, y) + zcyl(6.0, 6.0, top_z, x, y)
        else:
            chassis += zcyl(6.0, FLOOR[1], top_z, x, y)
        chassis -= zcyl(2.0, FLOOR[1] - 0.5, top_z + 1, x, y)
    return chassis


def floor_cuts(chassis: Part, floor_screws) -> Part:
    chassis -= zcyl(16.4, FLOOR[0] - 1, FLOOR[1] + 1)  # coupler hub
    for x, y in floor_screws:
        chassis -= zcyl(3.4, FLOOR[0] - 1, FLOOR[1] + 1, x, y)
    return chassis


def cover_bosses(chassis: Part, screws, y_outer: float) -> Part:
    """Blocks inside the chassis's bottom wall, tapped along +Y for the cover screws."""
    y_in = y_outer + 2 + 1.6
    for x, z in screws:
        chassis += box(x - 3, x + 3, y_in, y_in + 6, z - 3, z + 3)
        chassis -= _ycyl(3.0, y_outer - 1, y_in + 6.5, x, z)
    return chassis


def base_joints(u: Unit) -> None:
    u.joint("chassis", "plate")
    u.joint("coupler", "chassis", 0.5)
    u.joint("coupler", "door", 0.3)  # slides over the tailpiece
    u.joint("bridge", "chassis")
    u.joint("ring_gear", "bridge", 0.6)
    u.joint("enc_pin", "bridge")


@dataclass
class Concept:
    key: str
    name: str
    summary: str
    precedent: str
    battery: str
    battery_wh: float
    motor: str
    torque_note: str
    build_fn: Callable[[DeadboltParams, bool], Unit] = field(repr=False)
    drive_efficiency_factor: float = 1.0  # energy per move relative to a spur drive

    def unit(self, p: DeadboltParams, locked: bool = True) -> Unit:
        return self.build_fn(p, locked)

    def parts(self, p: DeadboltParams, locked: bool = True) -> dict[str, Part]:
        return self.unit(p, locked).parts()

    def assembly(self, p: DeadboltParams, locked: bool = True, guts_only: bool = False) -> Compound:
        return Compound(label=self.key, children=list(_visible(self.parts(p, locked), guts_only).values()))


SHELL_PARTS = ("cover", "knob", "cap", "battery_door", "screw_knob")


def _visible(parts: dict[str, Part], guts_only: bool) -> dict[str, Part]:
    if not guts_only:
        return parts
    return {k: v for k, v in parts.items() if k not in SHELL_PARTS and not k.startswith("screw_cover")}


# =====================================================================================
# A: Puck. Round, coaxial. The chassis is the visible cup and the front cap is the
# knob. Cells sit on the bridge under the cap: undo one screw and lift the cap to
# change them. The main PCB is an annulus under the bridge, around the gear, so the
# MCU must be chip-down: an ESP32-C6-MINI module (13.2 mm) is wider than the ring.
# =====================================================================================
A_R_OUT, A_WALL, A_TOP = 37.0, 2.0, 38.0


def _puck(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="n20", motor_angle_deg=90.0,
                    encoder_angle_deg=270.0, shaft_top=A_TOP + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    plate_r = 30.0
    floor_screws = [(25.0, 0.0), (-25.0, 0.0)]
    bridge_posts = [(14.5, 25.1), (-14.5, 25.1)]
    board_posts = [(6.0, -28.0), (-6.0, -28.0)]
    posts = bridge_posts + board_posts
    led_ang = -45.0

    back_plate(u, p, zcyl(2 * plate_r, 0, FLOOR[0]), floor_screws)
    drive_module(u, p, locked, L)

    # Chassis: the cup, with a light pipe through its wall.
    circle = lambda z0, z1, inset: zcyl(2 * (A_R_OUT - inset), z0, z1)
    cup = chassis_shell(circle, lambda z0, z1, g: zcyl(2 * (plate_r - g), z0, z1), A_TOP, A_WALL)
    cup = floor_cuts(chassis_posts(cup, posts, b0, shoulder=True), floor_screws)
    pipe = Rot(0, 0, led_ang) * _xcyl(3.0, 31.0, A_R_OUT, 0, 10.2)
    cup -= pipe
    u.add("chassis", cup, PRINTED, "Chassis cup (the visible body), PETG/ASA")
    chassis_screws(u, floor_screws, FLOOR[1])

    # Annular main PCB under the bridge, resting on the post shoulders.
    ring = zcyl(2 * 34.6, 6.0, 7.6) - zcyl(2 * 22.0, 5, 9) - zcyl(14.0, 5, 9, *u.motor_xy)
    for x, y in posts:
        ring -= zcyl(6.2, 5, 9, x, y)
    for x, y in floor_screws:
        ring -= zcyl(6.4, 5, 9, x, y)
    u.add("pcb", ring, PCB, "Main PCB, annular, 1.6 mm FR4 (chip-down ESP32-C6)")
    u.joint("pcb", "chassis")
    blocks = {}
    for name, (r, ang, w, h, ht) in {
        "mcu_chip": (28.0, 200.0, 5.0, 5.0, 0.9), "antenna": (32.0, 160.0, 3.2, 1.6, 1.0),
        "driver": (27.0, 30.0, 5.0, 6.5, 1.2), "hall": (31.0, 0.0, 3.0, 3.0, 1.0),
        "led": (30.0, led_ang, 2.0, 2.0, 1.0), "conn": (28.0, 230.0, 6.0, 4.0, 2.5),
    }.items():
        x, y = polar(r, math.radians(ang))
        blocks[name] = (x - w / 2, x + w / 2, y - h / 2, y + h / 2, ht)
    pcb_parts(u, "pcb", 7.6, blocks)
    u.add("light_pipe", pipe, PRINTED, "Light pipe, clear PMMA/PC, 3 mm, through the cup wall")
    u.joint("light_pipe", "pcb_led", 1.0)
    u.joint("light_pipe", "chassis")

    # Bridge: disc over the gear, with towers holding the cells' contacts.
    bridge = zcyl(2 * 34.6, b0, b1) - bridge_cuts(u, L, posts) + bridge_extras(u, L)
    cell_xs = [-18.0, 18.0]
    y_pos = -CR123A["l"] / 2 - CONTACT_T
    y_neg = CR123A["l"] / 2 + SPRING_L + CONTACT_T
    for x in cell_xs:
        bridge += box(x - 5, x + 5, y_pos - 1.6, y_pos, b1, b1 + 14)
        bridge += box(x - 5, x + 5, y_neg, y_neg + 1.6, b1, b1 + 14)
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor sleeve, cell holder")
    base_joints(u)
    u.joint("motor", "bridge")
    for i, (x, y) in enumerate(bridge_posts):
        add_stack_screw(u, f"screw_bridge_{i}", x, y, b1, b0 - 5.0, "chassis", ["bridge"])

    # Encoder board on two spacers; the same screws hold that side of the bridge down.
    eb_top = L.chip_z[1] + 1.6
    eb = rrect(0, -20.0, 16.0, 22.0, 2.0, eb_top - 1.6, eb_top)
    for x, y in board_posts:
        eb -= zcyl(2.2, eb_top - 3, eb_top + 1, x, y)
    u.add("enc_board", eb, PCB, "Encoder board, AS5600, wired to the main PCB")
    ex, ey = L.encoder_xy
    pcb_parts(u, "enc_board", eb_top, {"as5600": (ex - 2.5, ex + 2.5, ey - 3.1, ey + 3.1, 1.5, True),
                                       "conn": (-4.0, 4.0, -12.5, -9.5, 2.5)})
    spacers_and_stack(u, board_posts, b1, "enc_board", eb_top, b0 - 5.0)

    cells_along_y(u, dict(CR123A, desc="CR123A 3 V lithium cell"), cell_xs, y_pos, b1, "bridge")

    cap = zcyl(2 * A_R_OUT, A_TOP + 0.5, A_TOP + 10.5)
    knob_on_shaft(u, "cap", "Front cap = manual knob, PETG/ASA", cap, A_TOP + 0.5, L.shaft_top)
    return u


# =====================================================================================
# B: Tall slab. Drive at the bottom, 4x AA above it behind a battery door, main PCB
# over the drive with the AS5600 on its underside. Depth is set by the 16 mm motor.
# =====================================================================================
def _slab(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    W, y_bot, y_top, R, depth = 68.0, -35.0, 102.6, 8.0, 50.5
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="gm16", motor_angle_deg=90.0,
                    encoder_angle_deg=180.0, shaft_top=depth + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    cy, H = (y_bot + y_top) / 2, y_top - y_bot
    outline = lambda z0, z1, inset: rrect(0, cy, W - 2 * (2 + inset), H - 2 * (2 + inset), R - 2 - inset, z0, z1)
    plate_outline = lambda z0, z1, g: rrect(0, 2.5, 55.0 - 2 * g, 67.0 - 2 * g, 3.0 - g, z0, z1)
    floor_screws = [(24.0, -10.0), (-24.0, -10.0)]
    posts = [(25.0, -20.0), (-25.0, -20.0), (25.0, 30.0), (-25.0, 30.0)]
    cover_screws = [(15.0, 9.0), (-15.0, 9.0)]
    rib_lo, rib_hi = 38.4, 94.7

    back_plate(u, p, plate_outline(0, FLOOR[0], 0), floor_screws)
    drive_module(u, p, locked, L)

    chassis = floor_cuts(chassis_posts(chassis_shell(outline, plate_outline, 20.0), posts, b0), floor_screws)
    chassis += box(-30.4, 30.4, rib_lo, rib_lo + 1.6, FLOOR[1], 18.0)
    chassis += box(-30.4, 30.4, rib_hi, rib_hi + 1.6, FLOOR[1], 18.0)
    chassis = cover_bosses(chassis, cover_screws, y_bot)
    u.add("chassis", chassis, PRINTED, "Chassis tray, PETG/ASA")
    chassis_screws(u, floor_screws, FLOOR[1])

    # r4 = the chassis's inner corner radius, inset 0.4
    bridge = rrect(0, 3.25, 60.0, 68.5, 4.0, b0, b1) - bridge_cuts(u, L, posts) + bridge_extras(u, L)
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor sleeve, encoder pin seat")
    base_joints(u)
    u.joint("motor", "bridge")

    top = L.chip_z[1] + 1.6
    pcb = rrect(0, 3.5, 59.6, 69.0, 2.0, top - 1.6, top) - zcyl(13.0, 0, 60) - zcyl(17.0, 0, 60, *u.motor_xy)
    for x, y in posts:
        pcb -= zcyl(2.2, 0, 60, x, y)
    u.add("pcb", pcb, PCB, "Main PCB, 1.6 mm FR4, AS5600 on the underside")
    ex, ey = L.encoder_xy
    pcb_parts(u, "pcb", top, {
        "mcu": (9.0, 22.2, -17.0, -0.4, 2.4), "driver": (9.0, 14.0, 12.0, 18.5, 1.2),
        "as5600": (ex - 2.5, ex + 2.5, ey - 3.1, ey + 3.1, 1.5, True),
        "hall": (26.5, 29.5, 3.0, 6.0, 1.0), "led": (16.0, 18.0, 28.5, 30.5, 1.0),
        "button": (-24.0, -18.0, 12.0, 18.0, 3.5), "conn": (-12.0, -4.0, -29.0, -25.0, 2.5),
    })
    spacers_and_stack(u, posts, b1, "pcb", top, b0 - 5.0)
    light_pipe(u, 17.0, 29.5, top + 1.0, depth, "cover")

    cells_along_y(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                  [-22.5, -7.5, 7.5, 22.5], rib_lo + 1.6, FLOOR[1], "chassis")

    door_c, door_w, door_h = (0.0, 67.0), 58.0, 50.0
    cover = rrect(0, cy, W, H, R, 0, depth) - rrect(0, cy, W - 4, H - 4, R - 2, -1, depth - 2)
    cover -= zcyl(13.0, depth - 3, depth + 1) + zcyl(3.0, depth - 3, depth + 1, 17.0, 29.5)
    cover -= rrect(*door_c, door_w, door_h, 3.0, depth - 3, depth + 1)
    for x, z in cover_screws:
        cover -= cover_screw_cut(x, y_bot, z, 7.0)
    u.add("cover", cover, PRINTED, "Cover, PETG/ASA (or injection-moulded PC/ABS)")
    u.joint("cover", "chassis")
    for i, (x, z) in enumerate(cover_screws):
        cover_screw(u, i, x, y_bot, z, 7.0)

    door = rrect(*door_c, door_w - 0.4, door_h - 0.4, 2.8, depth - 2, depth)
    y0, y1 = door_c[1] - door_h / 2, door_c[1] + door_h / 2
    door += box(-10, 10, y0 - 2, y0 + 0.2, depth - 3, depth - 2) + box(-10, 10, y1 - 0.2, y1 + 2, depth - 3, depth - 2)
    u.add("battery_door", door, PRINTED, "Battery door, hook + snap")
    u.joint("battery_door", "cover")

    knob_on_shaft(u, "knob", "Manual knob, PETG/ASA", zcyl(40.0, depth + 0.5, depth + 12.5),
                  depth + 0.5, L.shaft_top)
    return u


# =====================================================================================
# C: Offset pill. A flat worm drive over the bore (the lost-motion coupling means
# nothing backdrives the motor, so a self-locking worm is allowed), 4x AA towards the
# hinge behind a battery door, main PCB over the drive.
# =====================================================================================
def _pill(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    x0, x1, H, R, depth = -102.0, 36.0, 66.0, 14.0, 31.0
    cx, W = (x0 + x1) / 2, x1 - x0
    L = DriveLayout(gear_z0=9.0, gear_teeth=40, motor="n20-worm", motor_angle_deg=90.0,
                    encoder_angle_deg=0.0, shaft_top=depth + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    outline = lambda z0, z1, inset: rrect(cx, 0, W - 2 * (2 + inset), H - 2 * (2 + inset), R - 2 - inset, z0, z1)
    plate_outline = lambda z0, z1, g: rrect(-3.0, 0, 62.0 - 2 * g, 56.0 - 2 * g, 4.0 - g, z0, z1)
    floor_screws = [(2.0, -26.0), (-30.0, -8.0)]
    posts = [(24.0, -20.0), (24.0, 13.0), (-22.0, -22.0), (-25.0, 5.0)]
    cover_screws = [(10.0, 9.0), (-30.0, 9.0)]

    back_plate(u, p, plate_outline(0, FLOOR[0], 0), floor_screws)
    drive_module(u, p, locked, L)

    # Worm on a long-shaft N20 lying along X; the output gear is the worm wheel.
    worm_y, worm_z, face_x = 24.0, L.gear_z0 + 3.0, -10.0
    worm = _xcyl(5.5, -8.0, 8.0, worm_y, worm_z)
    for k in range(5):
        worm += _xcyl(10.0, -7.1 + k * math.pi, -5.5 + k * math.pi, worm_y, worm_z)
    worm -= _xcyl(3.0, -9.0, 9.0, worm_y, worm_z)
    u.add("worm", worm, PRINTED, "Worm, m1 single start, on the motor shaft (or brass)")
    u.meshes.add(frozenset({"worm", "ring_gear"}))
    gb_x0 = face_x - N20["gearbox"]
    can_x0 = gb_x0 - N20["can"]
    gearbox = box(gb_x0, face_x, worm_y - 5, worm_y + 5, worm_z - 6, worm_z + 6)
    can = _xcyl(12.0, can_x0, gb_x0, worm_y, worm_z) & box(-60, 0, worm_y - 5, worm_y + 5, 0, 40)
    shaft = _xcyl(3.0, face_x, 10.5, worm_y, worm_z)
    u.add("motor", gearbox + can + shaft, BOUGHT, "N20 gear motor, 6 V, ~30:1, long 3 mm shaft")
    u.joint("worm", "motor")

    chassis = chassis_posts(chassis_shell(outline, plate_outline, 20.0), posts, b0)
    chassis = floor_cuts(chassis, floor_screws)
    # Motor cradle under the can (clear of the wheel), and the worm's outboard bearing.
    chassis += box(-35.0, -21.0, 16.0, 29.4, FLOOR[1], worm_z + 6) \
        - box(can_x0 - 1, gb_x0 + 1, worm_y - 5, worm_y + 5, worm_z - 6, 40)
    chassis += box(9.0, 13.0, 19.4, 29.4, FLOOR[1], worm_z + 5) - _xcyl(3.0, 8.0, 14.0, worm_y, worm_z)
    chassis -= zcyl(2.0, worm_z - 3, worm_z + 7, -27.5, 17.5)  # strap screw
    # Battery ribs at both ends of the bay.
    chassis += box(-95.5, -35.5, -29.4, -27.6, FLOOR[1], 18.0) + box(-95.5, -35.5, 27.1, 29.4, FLOOR[1], 18.0)
    chassis &= outline(-1, 40, 0)
    chassis = cover_bosses(chassis, cover_screws, -H / 2)
    u.add("chassis", chassis, PRINTED, "Chassis tray with motor cradle and worm bearing, PETG/ASA")
    u.joint("motor", "chassis")
    chassis_screws(u, floor_screws, FLOOR[1])

    strap = box(-32.0, -23.0, 16.0, 29.4, worm_z + 6, worm_z + 8) - zcyl(2.2, 0, 40, -27.5, 17.5)
    u.add("motor_strap", strap, PRINTED, "Motor strap")
    u.joint("motor_strap", "motor")
    u.joint("motor_strap", "chassis")
    add_stack_screw(u, "screw_strap", -27.5, 17.5, worm_z + 8, worm_z - 2, "chassis", ["motor_strap"])

    # Bridge over the wheel on the side away from the worm (the worm holds the near side down).
    bridge = rrect(0, -5.5, 56.0, 42.0, 3.0, b0, b1) - bridge_cuts(u, L, posts) + bridge_extras(u, L)
    u.add("bridge", bridge, PRINTED, "Bridge: wheel retainer, encoder pin seat")
    base_joints(u)

    top = L.chip_z[1] + 1.6
    pcb = rrect(-2.0, 0, 64.5, 55.0, 9.0, top - 1.6, top) - zcyl(13.0, 0, 60)
    for x, y in posts:
        pcb -= zcyl(2.2, 0, 60, x, y)
    u.add("pcb", pcb, PCB, "Main PCB, 1.6 mm FR4, AS5600 on the underside")
    ex, ey = L.encoder_xy
    pcb_parts(u, "pcb", top, {
        "mcu": (-30.0, -16.8, -20.0, -3.4, 2.4), "driver": (-12.0, -6.5, -22.0, -15.5, 1.2),
        "as5600": (ex - 2.5, ex + 2.5, ey - 3.1, ey + 3.1, 1.5, True),
        "hall": (26.0, 29.0, -2.0, 1.0, 1.0), "led": (-7.5, -5.5, 20.5, 22.5, 1.0),
        "button": (-30.0, -24.0, 10.0, 16.0, 2.0), "conn": (-31.0, -23.0, 19.0, 23.0, 2.0),
    })
    spacers_and_stack(u, posts, b1, "pcb", top, b0 - 5.0)
    light_pipe(u, -6.5, 21.5, top + 1.0, depth, "cover")

    cells_along_y(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                  [-42.5, -57.5, -72.5, -87.5], -27.6, FLOOR[1], "chassis")

    door_c, door_w, door_h = (-66.0, 0.0), 58.0, 52.0
    cover = rrect(cx, 0, W, H, R, 0, depth) - rrect(cx, 0, W - 4, H - 4, R - 2, -1, depth - 2)
    cover -= zcyl(13.0, depth - 3, depth + 1) + zcyl(3.0, depth - 3, depth + 1, -6.5, 21.5)
    cover -= rrect(*door_c, door_w, door_h, 3.0, depth - 3, depth + 1)
    for x, z in cover_screws:
        cover -= cover_screw_cut(x, -H / 2, z, 7.0)
    u.add("cover", cover, PRINTED, "Cover, PETG/ASA (or injection-moulded PC/ABS)")
    u.joint("cover", "chassis")
    for i, (x, z) in enumerate(cover_screws):
        cover_screw(u, i, x, -H / 2, z, 7.0)

    door = rrect(*door_c, door_w - 0.4, door_h - 0.4, 2.8, depth - 2, depth)
    door += box(-70, -62, door_h / 2 - 0.2, door_h / 2 + 2, depth - 3, depth - 2) \
        + box(-70, -62, -door_h / 2 - 2, -door_h / 2 + 0.2, depth - 3, depth - 2)
    u.add("battery_door", door, PRINTED, "Battery door, hook + snap")
    u.joint("battery_door", "cover")

    knob_on_shaft(u, "knob", "Manual knob, PETG/ASA", zcyl(40.0, depth + 0.5, depth + 10.5),
                  depth + 0.5, L.shaft_top)
    return u


CONCEPTS: dict[str, Concept] = {
    c.key: c for c in (
        Concept("A-puck", "Puck",
                "Round and coaxial; the chassis is the visible cup and the front cap is the knob. "
                "Smallest, but the PCB has to be a chip-down annulus.",
                "August Wi-Fi (about 72 mm round, 46 mm deep, 2x CR123A)",
                "2x CR123A lithium", 9.0, "N20 spur gear motor",
                "smallest motor; fine on a well-aligned door, marginal if the bolt rubs the strike",
                _puck),
        Concept("B-slab", "Tall slab",
                "Vertical slab, drive at the bottom, cells above behind a battery door. "
                "Biggest battery and motor; the most visible.",
                "Yale Approach (about 66 x 142 x 41 mm, 4x AA), Aqara U200 (62 x 152 x 61 mm)",
                "4x AA lithium", 18.0, "16 mm planetary gear motor",
                "roughly 3x the N20's torque in the same gear layer; margin for sticky doors",
                _slab),
        Concept("C-pill", "Offset pill",
                "Flat worm drive over the bore, cells towards the hinge behind a battery door. "
                "Shallowest, but wide.",
                "no direct US retrofit precedent; similar split to some EU cylinder retrofits",
                "4x AA lithium", 18.0, "N20 lying flat, driving a worm wheel",
                "worm gives high torque in a flat layer but only ~40-50% efficient, so more energy per move",
                _pill, drive_efficiency_factor=1.6),
    )
}


# --- Energy budget (assumptions; tune when the firmware is measured) ---
SLEEP_UA, SLEEP_V = 60.0, 3.3  # Thread sleepy end device with fast poll, door sensor, regulator
ACTUATIONS_PER_DAY, JOULES_PER_ACTUATION = 10, 3.0  # ~0.5 A at 6 V for ~1 s
CONVERTER_EFF, USABLE_FRACTION = 0.85, 0.7  # cold, pulse sag, brown-out cutoff


def wh_per_year(drive_factor: float = 1.0) -> float:
    sleep_j = SLEEP_UA * 1e-6 * SLEEP_V * 86400
    act_j = ACTUATIONS_PER_DAY * JOULES_PER_ACTUATION * drive_factor
    return (sleep_j + act_j) / CONVERTER_EFF * 365 / 3600


def life_years(c: Concept) -> float:
    return c.battery_wh * USABLE_FRACTION / wh_per_year(c.drive_efficiency_factor)
