"""Lock-unit concepts, as assemblies of real parts on the deadbolt fixture.

Every concept shares the drive module in concepts/kit.py: a coupler over the
tailpiece, an m1 output gear with a lost-motion pocket, a motor pinion (or worm),
bolt sensing (a magnet in a flag on the coupler, read by two Hall switches
on the PCB at the locked and unlocked angles), and a knob on the coupler's shaft.
The motor is a Pololu micro metal gearmotor with a 12 CPR encoder on its back end, which
gives the output gear's position (to park it mid-pocket) and stalls. The chips are the
ones in concepts/parts.py, the same as the dev kit's. What differs is the layout around
it: chassis, cover, battery bay, PCB and motor.

Assembly order (all three): melt the heat-set inserts into the chassis posts, cover
bosses and coupler; screw the steel back plate to the deadbolt's posts; fit coupler +
output gear; drop the chassis over them and screw it to the plate (M3 into the plate's
tapped holes); screw the motor to the bridge (or C's bulkhead) by its gearbox face;
fit the bridge and the bolt flag; screw the PCB stack down (M2 into the post inserts);
drop the battery contacts into their rib slots; snap the cover on and screw its
bottom; fit the knob (M3 into the coupler's insert).

Every screw threads into a brass insert, the steel plate or the motor, never bare
plastic; concepts/check.py checks lengths, thread engagement and the wall round each
insert, and the strain in every snap.

Frame and units are the fixture's (deadbolt/fixture.py): origin on the bore axis
at the interior door face, +Z into the room, +X towards the latch edge, +Y up, mm.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Callable

from build123d import Compound, Cylinder, Part, Pos, Rot

from concepts import parts as P
from concepts.kit import (AA, BOUGHT, CELL, CR123A, INSERTS, INSERT_RESERVE, METAL, MOTOR_DESC, N20, PCB,
                          PRINTED, DriveLayout, Snap, Unit, add_stack_screw, back_plate, bolt_hall_blocks, box,
                          bridge_cuts, bridge_extras, chassis_screws, cover_screw, drive_module, fix,
                          knob_on_shaft, pcb_parts, polar, rrect, wall_snap, zcyl)
from deadbolt.fixture import DeadboltParams

FLOOR = (3.0, 5.0)  # chassis floor, sitting on the 3 mm back plate
POST_D = 7.0  # chassis posts: an M2 insert (hole 3.2) with 1.9 mm of wall


def _xcyl(d, x0, x1, y, z) -> Part:
    return Pos((x0 + x1) / 2, y, z) * Rot(0, 90, 0) * Cylinder(d / 2, x1 - x0)


def _ycyl(d, y0, y1, x, z) -> Part:
    return Pos(x, (y0 + y1) / 2, z) * Rot(90, 0, 0) * Cylinder(d / 2, y1 - y0)


# --- battery bay: cells in series between two slotted ribs ---
# Each rib is a 0.8 mm skin facing the cells, a 1.0 mm slot the contact drops into from
# above, and a 1.2 mm back. A window in the skin lets the cell's + nub or the - spring
# through to the contact. Cells alternate direction so link contacts join them in series
# (the usual 4xAA holder layout); only the two end terminals need wires.
SKIN, SLOT, BACK = 0.8, 1.0, 1.2
RIB_T = SKIN + SLOT + BACK
NUB_L, NUB_D, WINDOW_D, SPRING_D = 1.0, 5.5, 7.0, 6.0
CONTACT_T, CONTACT_HW, TAB_UP = 0.5, 4.5, 2.0


def battery_bay(u: Unit, spec: dict, xs: list[float], y_back: float, z0: float, holder: str,
                rib_x: tuple[float, float] | None = None, link: bool = True) -> Part:
    """Add the cells and contacts; return the two ribs to add to the holder.

    The near rib's back is at y_back, cells lie along +Y on z0. rib_x spans one rib across
    every cell; None gives each cell its own pair of ribs (and link=False: wire the cells).
    """
    d, l = spec["d"], spec["l"]
    zc = z0 + d / 2
    rib_top = zc + 0.35 * d
    yf = y_back + RIB_T  # near skin face; the cells' bodies rest against it
    yf2 = yf + l + 1.0  # far skin face
    ends = {"near": (yf - NUB_L - CONTACT_T, yf - NUB_L, yf - SKIN - SLOT, yf - SKIN, yf - SKIN, yf),
            "far": (yf2 + SKIN + 0.2, yf2 + SKIN + 0.2 + CONTACT_T, yf2 + SKIN, yf2 + SKIN + SLOT, yf2, yf2 + SKIN)}

    def plus_near(i: int) -> bool:
        return i % 2 == 0

    for i, x in enumerate(xs):
        if plus_near(i):
            body, nub = (yf, yf + l - NUB_L), (yf - NUB_L, yf)
        else:
            body, nub = (yf2 - l + NUB_L, yf2), (yf2, yf2 + NUB_L)
        u.add(f"cell_{i}", _ycyl(d, *body, x, zc) + _ycyl(NUB_D, *nub, x, zc), CELL, spec["desc"])
        u.joint(f"cell_{i}", holder)

    # contacts: (name, end, cell indices, desc)
    contacts = []
    if link:
        for i in range(len(xs) - 1):
            end = "far" if plus_near(i) else "near"  # where cell i's - end is
            contacts.append((f"contact_link_{i}", end, [i, i + 1],
                             "Battery link contact, + flat / - spring, nickel-plated steel"))
        last = len(xs) - 1
        contacts.append(("contact_pos", "near", [0], "Battery contact, + flat with solder tab"))
        contacts.append(("contact_neg", "far" if plus_near(last) else "near", [last],
                         "Battery contact, - spring with solder tab"))
    else:
        for i in range(len(xs)):
            pe, ne = ("near", "far") if plus_near(i) else ("far", "near")
            contacts.append((f"contact_pos_{i}", pe, [i], "Battery contact, + flat with solder tab"))
            contacts.append((f"contact_neg_{i}", ne, [i], "Battery contact, - spring with solder tab"))

    slots = {"near": [], "far": []}
    for name, end, cells, desc in contacts:
        c0, c1, *_ = ends[end]
        x0, x1 = min(xs[i] for i in cells) - CONTACT_HW, max(xs[i] for i in cells) + CONTACT_HW
        solid = box(x0, x1, c0, c1, zc - 5.3, rib_top + TAB_UP)
        for i in cells:
            minus_here = plus_near(i) != (end == "near")
            if minus_here:  # spring from the contact's face to the cell's - end
                face = c1 if end == "near" else c0
                cell_end = yf2 - 2.0 if plus_near(i) else yf + 2.0
                solid += _ycyl(SPRING_D, min(face, cell_end), max(face, cell_end), xs[i], zc)
            u.joint(f"cell_{i}", name)
        u.add(name, solid, METAL, desc)
        u.joint(name, holder)
        slots[end].append((x0 - 0.3, x1 + 0.3))

    ribs = Part()
    for end in ("near", "far"):
        _, _, s0, s1, k0, k1 = ends[end]
        y0, y1 = (y_back, yf) if end == "near" else (yf2, yf2 + RIB_T)
        spans = [rib_x] if rib_x else [(x - CONTACT_HW - 2.0, x + CONTACT_HW + 2.0) for x in xs]
        rib = Part()
        for a, b in spans:
            rib += box(a, b, y0, y1, z0, rib_top)
        for a, b in slots[end]:
            rib -= box(a, b, s0, s1, zc - 5.3, rib_top + 1)
        for x in xs:
            rib -= _ycyl(WINDOW_D, k0 - 0.01, k1 + 0.01, x, zc)
        ribs += rib
    return ribs


def spacers_and_stack(u: Unit, posts, bottom_z, board: str, board_top: float, post_top: float) -> None:
    """Spacer tubes from the bridge to a board; one M2 per post clamps board, spacer and bridge
    onto an insert in the chassis post."""
    for i, (x, y) in enumerate(posts):
        s = f"spacer_{i}"
        u.add(s, zcyl(5.0, bottom_z, board_top - 1.6, x, y), PRINTED, "PCB spacer tube, 5 mm OD")
        u.joint(s, "bridge")
        u.joint(s, board)
        add_stack_screw(u, f"screw_stack_{i}", x, y, board_top, post_top, "chassis", [board, s, "bridge"])


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
    """Solid posts; each M2 cuts its own insert hole in the top."""
    for x, y in posts:
        if shoulder:  # a PCB rests on the d9 shoulder
            chassis += zcyl(9.0, FLOOR[1], 6.0, x, y) + zcyl(POST_D, 6.0, top_z, x, y)
        else:
            chassis += zcyl(POST_D, FLOOR[1], top_z, x, y)
    return chassis


def floor_cuts(chassis: Part) -> Part:
    return chassis - zcyl(16.4, FLOOR[0] - 1, FLOOR[1] + 1)  # coupler hub


M3_BOSS = 8.0  # square boss round an M3 insert (hole 4.0): 2 mm walls


def cover_bosses(chassis: Part, screws, y_outer: float) -> Part:
    """Blocks behind the chassis's bottom wall for the cover screws' inserts. The insert goes in
    from outside, through the wall, so the boss starts at the wall's inner face."""
    wall_out = y_outer + 2.0
    depth = INSERTS["M3"].l + INSERT_RESERVE + 1.3
    for x, z in screws:
        chassis += box(x - M3_BOSS / 2, x + M3_BOSS / 2, wall_out + 1.6, wall_out + depth,
                       z - M3_BOSS / 2, z + M3_BOSS / 2)
    return chassis


def battery_door(u: Unit, cx: float, cy: float, w: float, h: float, depth: float) -> Part:
    """Door in the cover's front: two hooks under the rim at the -Y end, a snap latch at the
    +Y end. Hook in, swing down, click. Returns the keeper to add to the cover."""
    y0, y1 = cy - h / 2, cy + h / 2
    arm_l, arm_t, catch = 9.0, 1.2, 1.0
    za = depth - 2 - arm_l
    door = rrect(cx, cy, w - 0.4, h - 0.4, 2.8, depth - 2, depth)
    for sx in (-1, 1):
        door += box(cx + sx * 12 - 5, cx + sx * 12 + 5, y0 - 2, y0 + 0.2, depth - 3, depth - 2)
    door += box(cx - 5, cx + 5, y1 - 0.2 - arm_t, y1 - 0.2, za, depth - 2)
    door += box(cx - 5, cx + 5, y1 - 0.2, y1 + catch, za, za + 1.0)
    u.add("battery_door", door, PRINTED, "Battery door, hooks + snap latch")
    u.joint("battery_door", "cover")
    u.snaps.append(Snap("battery door latch", arm_t, arm_l, catch))
    return box(cx - 6, cx + 6, y1, y1 + 3, za + 1.0, depth - 2)


def base_joints(u: Unit) -> None:
    u.joint("chassis", "plate")
    u.joint("coupler", "chassis", 0.5)
    u.joint("coupler", "door", 0.3)  # slides over the tailpiece
    u.joint("bridge", "chassis")
    u.joint("ring_gear", "bridge", 0.6)
    if "enc_pin" in u.items:
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
# MCU must be chip-down: an ESP32-C6-MINI module (13.2 mm) is wider than the ring. The
# encoder motor stands 32.5 mm off the bridge, which sets the cup's depth.
# =====================================================================================
A_R_OUT, A_WALL, A_TOP = 37.0, 2.0, 48.5


def _puck(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="n20", motor_angle_deg=90.0,
                    shaft_top=A_TOP + 0.5 + 6.0, motor_turn_deg=180.0, hall_r=8.5)
    b0, b1 = L.bridge_z
    plate_r = 30.0
    floor_screws = [(25.0, 0.0), (-25.0, 0.0)]
    bridge_posts = [(14.5, 25.1), (-14.5, 25.1)]
    board_posts = [(6.0, -28.0), (-6.0, -28.0)]
    posts = bridge_posts + board_posts
    led_ang = -45.0
    pcb_z = (6.0, 7.6)

    back_plate(u, p, zcyl(2 * plate_r, 0, FLOOR[0]), floor_screws)
    drive_module(u, p, locked, L)

    # Chassis: the cup, with a light pipe through its wall.
    circle = lambda z0, z1, inset: zcyl(2 * (A_R_OUT - inset), z0, z1)
    cup = chassis_shell(circle, lambda z0, z1, g: zcyl(2 * (plate_r - g), z0, z1), A_TOP, A_WALL)
    cup = floor_cuts(chassis_posts(cup, posts, b0, shoulder=True))
    pipe = Rot(0, 0, led_ang) * _xcyl(3.0, 31.0, A_R_OUT, 0, 10.2)
    cup -= pipe
    u.add("chassis", cup, PRINTED, "Chassis cup (the visible body), PETG/ASA")
    chassis_screws(u, floor_screws, FLOOR[1])

    # Annular main PCB on the post shoulders, clamped there by collars under the bridge.
    ring = zcyl(2 * 34.6, *pcb_z) - zcyl(2 * 22.0, 5, 9) - zcyl(14.0, 5, 9, *u.motor_xy)
    for x, y in posts:
        ring -= zcyl(POST_D + 0.4, 5, 9, x, y)
    for x, y in floor_screws:
        ring -= zcyl(6.4, 5, 9, x, y)
    u.add("pcb", ring, PCB, "Main PCB, annular, 1.6 mm FR4 (chip-down ESP32-C6)")
    u.joint("pcb", "chassis")
    blocks = {}
    for name, (r, ang, w, h, ht) in {
        "mcu_chip": (28.0, 200.0, 5.0, 5.0, 0.9), "antenna": (32.0, 160.0, 3.2, 1.6, 1.0),
        "led": (30.0, led_ang, 2.0, 2.0, 1.0), "conn": (28.0, 230.0, 6.0, 4.0, 2.5),
    }.items():
        x, y = polar(r, math.radians(ang))
        blocks[name] = (x - w / 2, x + w / 2, y - h / 2, y + h / 2, ht)
    for name, (r, ang) in {"driver": (28.0, 30.0), "hall_door": (31.0, 0.0), "regulator": (28.0, 300.0),
                           "motor_conn": (28.5, 140.0)}.items():
        x, y = polar(r, math.radians(ang))
        blocks[name] = (x, y, ang + 90.0)
    pcb_parts(u, "pcb", pcb_z[1], blocks)
    u.add("light_pipe", pipe, PRINTED, "Light pipe, clear PMMA/PC, 3 mm, through the cup wall")
    u.joint("light_pipe", "pcb_led", 1.0)
    u.joint("light_pipe", "chassis")

    # Bridge: disc over the gear; collars underneath clamp the PCB; cell ribs on top.
    bridge = zcyl(2 * 34.6, b0, b1) - bridge_cuts(u, L) + bridge_extras(u, L)
    for x, y in posts:
        bridge += zcyl(POST_D + 2.8, pcb_z[1], b0 + 0.01, x, y) - zcyl(POST_D + 0.4, pcb_z[1] - 1, b0, x, y)
    cr = dict(CR123A, desc="CR123A 3 V lithium cell")
    bridge += battery_bay(u, cr, [-18.0, 18.0], -(cr["l"] + 7.0) / 2, b1, "bridge", link=False)
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor mount, PCB clamp, cell holder")
    u.joint("bridge", "pcb")
    base_joints(u)
    u.joint("motor", "bridge")
    for i, (x, y) in enumerate(bridge_posts):
        add_stack_screw(u, f"screw_bridge_{i}", x, y, b1, b0, "chassis", ["bridge"])

    # Bolt-sensor board on two spacers, over the flag: the main PCB is down by the floor, so
    # the puck alone still needs this second (small) board. The same screws hold that side of
    # the bridge down.
    eb_top = L.board_bottom + 1.6
    eb = rrect(0, -11.5, 18.6, 39.0, 2.0, eb_top - 1.6, eb_top) - zcyl(13.0, 0, 60)  # between the cells
    u.add("enc_board", eb, PCB, "Bolt-sensor board, 2x Hall switch, wired to the main PCB")
    pcb_parts(u, "enc_board", eb_top, bolt_hall_blocks(L, p) | {"conn": (-4.0, 4.0, -16.5, -13.5, 2.5)})
    spacers_and_stack(u, board_posts, b1, "enc_board", eb_top, b0)

    cap = zcyl(2 * A_R_OUT, A_TOP + 0.5, A_TOP + 10.5)
    knob_on_shaft(u, "cap", "Front cap = manual knob, PETG/ASA", cap, A_TOP + 0.5, L.shaft_top)
    return u


# =====================================================================================
# B: Tall slab. Drive at the bottom, 4x AA above it behind a battery door, main PCB
# over the drive with the bolt Hall switches on its underside. Depth is set by the
# standing encoder motor (32.5 mm off the bridge).
# The cover hooks onto two snaps at the top of the chassis and is screwed at the bottom.
# =====================================================================================
def _slab(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    W, y_bot, y_top, R, depth = 68.0, -35.0, 102.6, 8.0, 51.5
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="n20", motor_angle_deg=90.0,
                    shaft_top=depth + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    cy, H = (y_bot + y_top) / 2, y_top - y_bot
    outline = lambda z0, z1, inset: rrect(0, cy, W - 2 * (2 + inset), H - 2 * (2 + inset), R - 2 - inset, z0, z1)
    plate_outline = lambda z0, z1, g: rrect(0, 2.5, 55.0 - 2 * g, 67.0 - 2 * g, 3.0 - g, z0, z1)
    floor_screws = [(24.0, -10.0), (-24.0, -10.0)]
    posts = [(25.0, -20.0), (-25.0, -20.0), (25.0, 30.0), (-25.0, 30.0)]
    cover_screws = [(15.0, 9.0), (-15.0, 9.0)]
    wall_top = 20.0

    back_plate(u, p, plate_outline(0, FLOOR[0], 0), floor_screws)
    drive_module(u, p, locked, L)

    chassis = floor_cuts(chassis_posts(chassis_shell(outline, plate_outline, wall_top), posts, b0))
    chassis += battery_bay(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                           [-22.5, -7.5, 7.5, 22.5], 38.4, FLOOR[1], "chassis", rib_x=(-30.4, 30.4))
    chassis = cover_bosses(chassis, cover_screws, y_bot)
    pockets = Part()
    for i, x in enumerate((-15.0, 15.0)):
        slits, bump, pocket = wall_snap(u, f"cover snap {i}", Pos(x, y_top - 2.0, 0), 8.0, wall_top)
        chassis = chassis - slits + bump
        pockets += pocket
    u.add("chassis", chassis, PRINTED, "Chassis tray, PETG/ASA")
    chassis_screws(u, floor_screws, FLOOR[1])

    # r4 = the chassis's inner corner radius, inset 0.4
    bridge = rrect(0, 3.25, 60.0, 68.5, 4.0, b0, b1) - bridge_cuts(u, L) + bridge_extras(u, L)
    for x, _ in cover_screws:  # notches round the cover-screw bosses
        bridge -= box(x - M3_BOSS / 2 - 0.3, x + M3_BOSS / 2 + 0.3, y_bot, y_bot + 12.0, b0 - 1, b1 + 1)
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor mount")
    base_joints(u)
    u.joint("motor", "bridge")

    top = L.board_bottom + 1.6
    mx, my = u.motor_xy
    pcb = rrect(0, 3.5, 59.6, 69.0, 2.0, top - 1.6, top) - zcyl(13.0, 0, 60) \
        - rrect(mx, my, N20["w"] + 0.6, N20["t"] + 0.6, 1.0, 0, 60)  # the gearbox passes through
    u.add("pcb", pcb, PCB, "Main PCB, 1.6 mm FR4, bolt Hall switches on the underside")
    pcb_parts(u, "pcb", top, bolt_hall_blocks(L, p) | {
        "mcu": (15.6, -8.7, 0.0), "driver": (11.5, 15.25, 90.0), "hall_door": (28.0, 4.5, 0.0),
        "regulator": (-12.0, 14.0, 0.0), "motor_conn": (-13.0, 26.0, 0.0),
        "led": (16.0, 18.0, 28.5, 30.5, 1.0), "button": (-24.0, -18.0, 12.0, 18.0, 3.5),
        "conn": (-12.0, -4.0, -29.0, -25.0, 2.5),
    })
    spacers_and_stack(u, posts, b1, "pcb", top, b0)
    light_pipe(u, 17.0, 29.5, top + 1.0, depth, "cover")

    door_c, door_w, door_h = (0.0, 67.0), 58.0, 50.0
    keeper = battery_door(u, *door_c, door_w, door_h, depth)
    cover = rrect(0, cy, W, H, R, 0, depth) - rrect(0, cy, W - 4, H - 4, R - 2, -1, depth - 2)
    cover -= zcyl(13.0, depth - 3, depth + 1) + zcyl(3.0, depth - 3, depth + 1, 17.0, 29.5)
    cover -= rrect(*door_c, door_w, door_h, 3.0, depth - 3, depth + 1)
    cover = cover - pockets + keeper
    u.add("cover", cover, PRINTED, "Cover, PETG/ASA (or injection-moulded PC/ABS)")
    u.joint("cover", "chassis")
    for i, (x, z) in enumerate(cover_screws):
        cover_screw(u, i, x, y_bot, z)

    knob_on_shaft(u, "knob", "Manual knob, PETG/ASA", zcyl(40.0, depth + 0.5, depth + 12.5),
                  depth + 0.5, L.shaft_top)
    return u


# =====================================================================================
# C: Offset pill. A flat worm drive over the bore (the lost-motion coupling means
# nothing backdrives the motor, so a self-locking worm is allowed), 4x AA towards the
# hinge behind a battery door, main PCB over the drive. The cover snaps onto both
# ends of the chassis and is screwed at the bottom.
# =====================================================================================
def _pill(p: DeadboltParams, locked: bool) -> Unit:
    u = Unit()
    x0, x1, H, R, depth = -109.0, 36.0, 70.0, 14.0, 32.0
    cx, W = (x0 + x1) / 2, x1 - x0
    L = DriveLayout(gear_z0=9.0, gear_teeth=40, motor="n20-worm", motor_angle_deg=90.0,
                    shaft_top=depth + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    outline = lambda z0, z1, inset: rrect(cx, 0, W - 2 * (2 + inset), H - 2 * (2 + inset), R - 2 - inset, z0, z1)
    plate_outline = lambda z0, z1, g: rrect(-3.0, 0, 62.0 - 2 * g, 56.0 - 2 * g, 4.0 - g, z0, z1)
    floor_screws = [(2.0, -24.0), (-30.0, -8.0)]
    posts = [(24.0, -20.0), (24.0, 13.0), (-22.0, -22.0), (-25.0, 5.0)]
    cover_screws = [(10.0, 9.0), (-30.0, 9.0)]
    wall_top = 20.0

    back_plate(u, p, plate_outline(0, FLOOR[0], 0), floor_screws)
    drive_module(u, p, locked, L)

    # Worm on the motor's 10 mm shaft, the motor lying along X with its connector towards the
    # bore. The output gear is the worm wheel. The gearbox face screws to a bulkhead; a
    # cradle under the can stops the motor turning. The gearbox must stay outside the
    # wheel's tips, so the shaft ends 1 mm past the wheel's centre line: the worm is two
    # turns either side of it, running on past the shaft's end.
    worm_y, worm_z = 24.0, L.gear_z0 + 3.0
    face_x = -9.0
    bulk_x = (face_x, face_x + 2.0)
    worm = _xcyl(5.5, face_x + 2.2, 2.4, worm_y, worm_z)
    for k in (-0.5, 0.5):
        worm += _xcyl(10.0, k * math.pi - 0.8, k * math.pi + 0.8, worm_y, worm_z)
    worm -= _xcyl(3.0, face_x, face_x + N20["shaft_l"], worm_y, worm_z)
    u.add("worm", worm, PRINTED, "Worm, m1 single start, on the motor shaft (or brass)")
    u.meshes.add(frozenset({"worm", "ring_gear"}))
    gb_x0 = face_x - N20["gearbox"]
    can_x0 = gb_x0 - N20["can"]
    motor_loc = Pos(face_x, worm_y, worm_z) * Rot(0, -90, 0) * Rot(0, 0, 180)
    u.add("motor", P.micro_gearmotor().at(motor_loc), BOUGHT, MOTOR_DESC["n20-worm"])
    u.joint("worm", "motor")
    for i, dz in enumerate((-N20["face_holes"] / 2, N20["face_holes"] / 2)):
        fix(u, f"screw_motor_{i}", "M1.6 pan", Pos(bulk_x[1], worm_y, worm_z + dz) * Rot(0, 90, 0),
            bulk_x[1] - bulk_x[0], "motor", "bought", ["chassis"], eng=(1.2, 2.0))

    chassis = chassis_posts(chassis_shell(outline, plate_outline, wall_top), posts, b0)
    chassis = floor_cuts(chassis)
    chassis += box(can_x0 + 1, gb_x0 - 1, 16.0, 31.4, FLOOR[1], worm_z + 6) \
        - box(can_x0, gb_x0, worm_y - 5.2, worm_y + 5.2, worm_z - 6.2, 40)  # cradle
    chassis += box(*bulk_x, 20.0, 31.4, FLOOR[1], worm_z + 6) - _xcyl(4.2, bulk_x[0] - 1, bulk_x[1] + 1, worm_y, worm_z)
    chassis += battery_bay(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                           [-49.5, -64.5, -79.5, -94.5], -29.4, FLOOR[1], "chassis", rib_x=(-102.5, -42.5))
    chassis &= outline(-1, 40, 0)
    chassis = cover_bosses(chassis, cover_screws, -H / 2)
    pockets = Part()
    for i, loc in enumerate((Pos(x0 + 2.0, 0, 0) * Rot(0, 0, 90), Pos(x1 - 2.0, 0, 0) * Rot(0, 0, -90))):
        slits, bump, pocket = wall_snap(u, f"cover snap {i}", loc, 8.0, wall_top)
        chassis = chassis - slits + bump
        pockets += pocket
    u.add("chassis", chassis, PRINTED, "Chassis tray with motor bulkhead, cradle and worm bearing, PETG/ASA")
    u.joint("motor", "chassis")
    chassis_screws(u, floor_screws, FLOOR[1])

    # Bridge over the wheel on the side away from the worm (the worm holds the near side down).
    bridge = rrect(0, -5.5, 56.0, 42.0, 3.0, b0, b1) - bridge_cuts(u, L) + bridge_extras(u, L)
    u.add("bridge", bridge, PRINTED, "Bridge: wheel retainer")
    base_joints(u)

    top = L.board_bottom + 1.6
    pcb = rrect(-2.0, 0, 64.5, 55.0, 9.0, top - 1.6, top) - zcyl(13.0, 0, 60)
    u.add("pcb", pcb, PCB, "Main PCB, 1.6 mm FR4, bolt Hall switches on the underside")
    pcb_parts(u, "pcb", top, bolt_hall_blocks(L, p) | {
        "mcu": (-23.4, -11.7, 0.0), "driver": (-9.25, -18.75, 90.0), "hall_door": (27.5, -0.5, 0.0),
        "regulator": (-14.0, 8.0, 0.0), "motor_conn": (-16.0, 22.0, 0.0),
        "led": (-7.5, -5.5, 20.5, 22.5, 1.0), "button": (-30.0, -24.0, 10.0, 16.0, 2.0),
        "conn": (-31.0, -23.0, 19.0, 23.0, 2.0),
    })
    spacers_and_stack(u, posts, b1, "pcb", top, b0)
    light_pipe(u, -6.5, 21.5, top + 1.0, depth, "cover")

    door_c, door_w, door_h = (-73.0, 0.0), 58.0, 52.0
    keeper = battery_door(u, *door_c, door_w, door_h, depth)
    cover = rrect(cx, 0, W, H, R, 0, depth) - rrect(cx, 0, W - 4, H - 4, R - 2, -1, depth - 2)
    cover -= zcyl(13.0, depth - 3, depth + 1) + zcyl(3.0, depth - 3, depth + 1, -6.5, 21.5)
    cover -= rrect(*door_c, door_w, door_h, 3.0, depth - 3, depth + 1)
    cover = cover - pockets + keeper
    u.add("cover", cover, PRINTED, "Cover, PETG/ASA (or injection-moulded PC/ABS)")
    u.joint("cover", "chassis")
    for i, (x, z) in enumerate(cover_screws):
        cover_screw(u, i, x, -H / 2, z)

    knob_on_shaft(u, "knob", "Manual knob, PETG/ASA", zcyl(40.0, depth + 0.5, depth + 10.5),
                  depth + 0.5, L.shaft_top)
    return u


CONCEPTS: dict[str, Concept] = {
    c.key: c for c in (
        Concept("A-puck", "Puck",
                "Round and coaxial; the chassis is the visible cup and the front cap is the knob. "
                "Smallest, but the PCB has to be a chip-down annulus.",
                "August Wi-Fi (about 72 mm round, 46 mm deep, 2x CR123A)",
                "2x CR123A lithium", 9.0, "Pololu #5193 (150:1, encoder), standing",
                "torque to be checked against the door (README, measurement 10)",
                _puck),
        Concept("B-slab", "Tall slab",
                "Vertical slab, drive at the bottom, cells above behind a battery door. "
                "Biggest battery; the most visible.",
                "Yale Approach (about 66 x 142 x 41 mm, 4x AA), Aqara U200 (62 x 152 x 61 mm)",
                "4x AA lithium", 18.0, "Pololu #5193 (150:1, encoder), standing",
                "torque to be checked against the door (README, measurement 10)",
                _slab),
        Concept("C-pill", "Offset pill",
                "Flat worm drive over the bore, cells towards the hinge behind a battery door. "
                "Shallowest, but wide.",
                "no direct US retrofit precedent; similar split to some EU cylinder retrofits",
                "4x AA lithium", 18.0, "Pololu #5185 (30:1, encoder) lying flat, driving a worm wheel",
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
