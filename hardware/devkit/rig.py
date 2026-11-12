"""Dev kit: the tall slab (concept B), opened up for development.

Same door mounting, drive, motor and battery bay as the slab, and the same chips as its
main PCB, each on a breakout (concepts/parts.py), so what is learned on it carries over:

- the main PCB is replaced by a removable deck carrying two mini breadboards: the
  ESP32-C6-DevKitM-1 (the product's ESP32-C6-MINI-1 module) on one, the Pololu #2130
  DRV8833 carrier on the other
- the three DRV5032 Hall switches (bolt locked, bolt unlocked, door) are on SparkFun SOT-23
  adapters, worn chip-down in a printed carrier at the product PCB's height, so they sit
  over the bolt flag's magnet exactly as on the product
- each breadboard is held by two printed clamp bars, one over each end, screwed into
  heat-set inserts in the deck (plus its adhesive back)
- the cover has a big front window (the wiring stays in view) and a slot for the
  DevKit's USB cables; it is 17 mm taller than the slab's to fit the second breadboard
- no battery door: lift the cover and the deck (four screws) to change cells
- the TPS629206 regulator is the one product chip not in the kit: the DevKit makes its
  own 3.3 V from USB, so battery life is measured on the first product PCB

Frame: the fixture's (deadbolt/fixture.py). WIRING is the proposed pin map;
build_devkit.py writes it to WIRING.md.
"""

from __future__ import annotations

import math

from build123d import Color, Compound, Part, Plane, Pos, Rot, Solid, Vector

from concepts import parts as P
from concepts.designs import (FLOOR, M3_BOSS, base_joints, battery_bay, chassis_posts,
                              chassis_shell, cover_bosses, floor_cuts)
from concepts.kit import (AA, BOUGHT, PRINTED, WIRE, DriveLayout, Unit, add_stack_screw,
                          back_plate, box, bridge_cuts, bridge_extras, chassis_screws, cover_screw,
                          drive_module, fix, knob_on_shaft, rrect, wall_snap, zcyl)
from deadbolt.fixture import GENERIC_US, DeadboltParams, build as build_fixture

W, Y_BOT, Y_TOP, R, DEPTH = 68.0, -35.0, 120.0, 8.0, 51.5  # the slab: 102.6 tall, as deep
WALL_TOP = 20.0
DECK_Z = (21.0, 25.5)  # over the cells (their tops are at 19.5); 4.5 mm holds an M2 insert
DECK_Y = (26.0, 116.0)

# Mini breadboard (170 points: 17 rows x 10 columns, no rails). Check yours: they vary.
MINI_BB = dict(l=47.0, w=35.0, h=8.5)
BB_TOP = DECK_Z[1] + MINI_BB["h"]
BOARDS = {"bb_drv": 60.5, "bb_mcu": 97.5}  # centre y of each breadboard; both centred on x = 0
COL_OFF = {c: -(3.81 + (4 - i) * 2.54) for i, c in enumerate("abcde")} | \
          {c: 3.81 + i * 2.54 for i, c in enumerate("fghij")}


def hole(board: str, row: int, col: str) -> tuple[float, float, float]:
    """Rows 1..17 run along +X; columns a..j run along +Y across the centre gap. Columns
    a-e of a row are one strip, f-j another."""
    return ((row - 9) * 2.54, BOARDS[board] + COL_OFF[col], BB_TOP)


# ESP32-C6-DevKitM-1: J1 in column i, J3 in column b, pin k in row 17 - k, so the USB end
# (pin 15) is at -X by the cable slot and the antenna overhangs the far clamp bar. Columns
# a and j are free for wires.
DEVKIT_LOC = Pos(-28.905, BOARDS["bb_mcu"] + 12.7, BB_TOP + P.HEADER_H) * Rot(0, 0, -90)


def devkit_pin(name: str, header: str = "") -> tuple[float, float, float]:
    for hdr, pins, col in (("J1", P.DEVKITM_J1, "j"), ("J3", P.DEVKITM_J3, "a")):
        if (not header or header == hdr) and name in pins:
            return hole("bb_mcu", 16 - pins.index(name), col)
    raise KeyError(name)


# Pololu #2130: power row in column e, control row in column g, pin 1 (both GNDs) in row 12.
DRV_ROW_GND = 12
DRV_LOC = Pos(8.89, BOARDS["bb_drv"] + COL_OFF["e"] - 1.27, BB_TOP + P.HEADER_H) * Rot(0, 0, 90)


def drv_pin(name: str, col: str = "") -> tuple[float, float, float]:
    """A free hole in a carrier pin's row: power row from column c, control row from i."""
    if name in P.POLOLU_2130_PWR:
        return hole("bb_drv", DRV_ROW_GND - P.POLOLU_2130_PWR.index(name), col or "c")
    return hole("bb_drv", DRV_ROW_GND - P.POLOLU_2130_CTL.index(name), col or "i")


# 3V3 and GND are shared out along two spare rows of the driver's breadboard (columns f-j).
RAIL_3V3, RAIL_GND = 15, 16


def rail(name: str, col: str) -> tuple[float, float, float]:
    return hole("bb_drv", RAIL_3V3 if name == "3V3" else RAIL_GND, col)


# Which adapter holes carry the DRV5032's pins depends on how it sits on the SOT-23-6
# pads; these assume pin 1 on pad 1, pin 2 on pad 3, pin 3 on pad 5. Check the silkscreen.
HALL_HOLE = {"VCC": "1", "OUT": "3", "GND": "5"}
HALLS = {"hall_locked": "22", "hall_unlocked": "23", "hall_door": "2"}  # OUT -> GPIO

# Proposed pin map: (signal, from, to, colour). Avoids the strapping pins (8, 9, 15),
# USB (12, 13) and UART0 (16, 17). The encoder is read by the PCNT unit: any GPIO will do.
WIRING = [
    ("Motor A in 1 (PWM)", "GPIO 18", "DRV8833 AIN1", "yellow"),
    ("Motor A in 2 (PWM)", "GPIO 19", "DRV8833 AIN2", "orange"),
    ("Driver enable", "GPIO 20", "DRV8833 nSLEEP", "white"),
    ("Driver fault (open drain: pull-up on)", "GPIO 3", "DRV8833 nFAULT", "purple"),
    ("3V3 rail", "3V3", "3V3 row (DRV breadboard row 15)", "red"),
    ("GND rail", "J3 GND", "GND row (DRV breadboard row 16)", "black"),
    ("Driver ground", "GND row", "DRV8833 GND (control row)", "black"),
    ("Motor M1 (+)", "DRV8833 AOUT1", "motor cable pin 6", "red"),
    ("Motor M2", "DRV8833 AOUT2", "motor cable pin 5", "black"),
    ("Motor encoder A", "GPIO 21", "motor cable pin 3", "yellow"),
    ("Motor encoder B", "GPIO 4", "motor cable pin 2", "green"),
    ("Motor encoder power", "3V3 row", "motor cable pin 4", "red"),
    ("Motor encoder ground", "GND row", "motor cable pin 1", "black"),
    ("Bolt locked (DRV5032 OUT)", "GPIO 22", "hall_locked OUT", "blue"),
    ("Bolt unlocked (DRV5032 OUT)", "GPIO 23", "hall_unlocked OUT", "blue"),
    ("Door (DRV5032 OUT)", "GPIO 2", "hall_door OUT", "blue"),
    ("Hall switch power (x3)", "3V3 row", "each adapter's VCC", "red"),
    ("Hall switch ground (x3)", "GND row (door: DRV8833 power-row GND)", "each adapter's GND", "black"),
    ("Battery + (6 V)", "4xAA + contact", "DRV8833 VIN (power row)", "red"),
    ("Battery -", "4xAA - contact", "DRV8833 GND (power row)", "black"),
]
WIRE_COLORS = {"red": Color(0.85, 0.1, 0.1), "black": Color(0.1, 0.1, 0.1), "yellow": Color(0.95, 0.85, 0.1),
               "orange": Color(0.95, 0.5, 0.1), "white": Color(0.95, 0.95, 0.95), "blue": Color(0.15, 0.35, 0.9),
               "purple": Color(0.55, 0.2, 0.75), "green": Color(0.15, 0.7, 0.25)}
WIRE_D = 1.5


def _wire(u: Unit, pts: list[tuple[float, float, float]], colour: str, desc: str, ends: tuple[str, str]) -> None:
    """A jumper wire as a chain of cylinders through the given points."""
    r = WIRE_D / 2
    segs = []
    for a, b in zip(pts, pts[1:]):
        va, vb = Vector(*a), Vector(*b)
        segs.append(Solid.make_cylinder(r, (vb - va).length, Plane(origin=va, z_dir=vb - va)))
    segs += [Pos(*p) * Solid.make_sphere(r) for p in pts[1:-1]]
    name = f"wire_{sum(1 for n in u.items if n.startswith('wire_')):02d}"
    u.add(name, Compound(children=segs), WIRE, f"Jumper wire, {colour}: {desc}")
    u.items[name].part.color = WIRE_COLORS[colour]
    u.items[name].ends = (pts[0], pts[-1])  # for the tests: which holes it uses
    for e in ends:
        u.joint(name, e)


def _lift(p, dz):
    return (p[0], p[1], p[2] + dz)


CLAMP_LIP, CLAMP_W, CLAMP_SEAT = 1.2, 6.5, 5.5  # lip over the breadboard's end margin
CLAMP_SCREW_DY = 15.5  # either side of the breadboard's centre line, clear of the DevKit


def clamp_screw_xy(name: str) -> list[tuple[float, float]]:
    x = MINI_BB["l"] / 2 + CLAMP_W / 2
    return [(sx * x, BOARDS[name] + dy) for sx in (-1, 1) for dy in (-CLAMP_SCREW_DY, CLAMP_SCREW_DY)]


def _breadboard(u: Unit, name: str, post_screws: set[tuple[float, float]]) -> None:
    """A mini breadboard on its adhesive back, held by a clamp bar over each end. Each bar's two
    M2s go into inserts in the deck, or through it into a chassis post where there is one."""
    yc = BOARDS[name]
    l, w, h = MINI_BB["l"], MINI_BB["w"], MINI_BB["h"]
    u.add(name, box(-l / 2, l / 2, yc - w / 2, yc + w / 2, DECK_Z[1], DECK_Z[1] + h), BOUGHT,
          "Mini breadboard, 170 points (47 x 35 mm), adhesive back")
    u.items[name].part.color = Color(0.95, 0.95, 0.93)
    u.joint(name, "deck")
    for sx in (-1, 1):
        x0, x1 = sorted((sx * l / 2, sx * (l / 2 + CLAMP_W)))
        lx0, lx1 = sorted((sx * l / 2, sx * (l / 2 - CLAMP_LIP)))
        bar = box(x0, x1, yc - w / 2, yc + w / 2, DECK_Z[1], BB_TOP + 1.5)
        bar += box(lx0, lx1, yc - w / 2, yc + w / 2, BB_TOP, BB_TOP + 1.5)
        bar_name = f"{name}_clamp_{'r' if sx > 0 else 'l'}"
        u.add(bar_name, bar, PRINTED, "Breadboard clamp bar, PETG")
        u.joint(bar_name, name)
        u.joint(bar_name, "deck")
        for k, (x, y) in enumerate(xy for xy in clamp_screw_xy(name) if xy[0] * sx > 0):
            seat = Pos(x, y, DECK_Z[1] + CLAMP_SEAT)
            cbore = BB_TOP + 1.5 - (DECK_Z[1] + CLAMP_SEAT)
            if (x, y) in post_screws:  # through the deck into a chassis post
                add = dict(clamp=CLAMP_SEAT + DECK_Z[1] - DECK_Z[0], into="chassis", clamps=[bar_name, "deck"])
            else:
                add = dict(clamp=CLAMP_SEAT, into="deck", clamps=[bar_name])
            fix(u, f"screw_{bar_name}_{k}", "M2 button", seat, add["clamp"], add["into"], "insert",
                add["clamps"], cbore=cbore + 0.5)


def hall_places(L: DriveLayout, p: DeadboltParams) -> dict[str, tuple[float, float, float]]:
    """(x, y, rot) of each Hall adapter: the bolt ones over the magnet's two stops, their
    7.62 mm side radial so they clear the shaft; the door one by the latch edge."""
    out = {}
    for name, locked in (("hall_locked", True), ("hall_unlocked", False)):
        x, y = L.hall_xy(p, locked)
        out[name] = (x, y, math.degrees(math.atan2(y, x)) - 90.0)
    out["hall_door"] = (24.0, 4.0, 90.0)
    return out


def rig(p: DeadboltParams = GENERIC_US, locked: bool = True) -> tuple[Unit, dict[str, Part]]:
    """The dev kit and the door + deadbolt it mounts on."""
    u = Unit()
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="n20", motor_angle_deg=90.0,
                    shaft_top=DEPTH + 0.5 + 6.0)
    b0, b1 = L.bridge_z
    cy, H = (Y_BOT + Y_TOP) / 2, Y_TOP - Y_BOT
    outline = lambda z0, z1, inset: rrect(0, cy, W - 2 * (2 + inset), H - 2 * (2 + inset), R - 2 - inset, z0, z1)
    plate_outline = lambda z0, z1, g: rrect(0, 2.5, 55.0 - 2 * g, 67.0 - 2 * g, 3.0 - g, z0, z1)
    floor_screws = [(6.0, -27.0), (-6.0, -27.0)]  # clear of the carrier posts and cover bosses
    bridge_posts = [(25.0, -20.0), (-25.0, -20.0)]
    deck_posts_near = [(25.0, 30.0), (-25.0, 30.0)]  # through spacers on the bridge
    # Tall posts beyond the cells: the upper breadboard's top clamp screws go into them.
    deck_posts_far = [xy for xy in clamp_screw_xy("bb_mcu") if xy[1] > BOARDS["bb_mcu"]]
    carrier_posts = [(-26.0, -7.0), (-26.0, 7.0), (26.0, -9.0)]
    cover_screws = [(15.0, 9.0), (-15.0, 9.0)]

    back_plate(u, p, plate_outline(0, FLOOR[0], 0), floor_screws)
    drive_module(u, p, locked, L)

    # --- chassis: the slab's, with tall posts for the deck's far edge ---
    chassis = chassis_shell(outline, plate_outline, WALL_TOP)
    chassis = chassis_posts(chassis, bridge_posts + deck_posts_near + carrier_posts, b0)
    chassis = floor_cuts(chassis_posts(chassis, deck_posts_far, DECK_Z[0]))
    chassis += battery_bay(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                           [-22.5, -7.5, 7.5, 22.5], 38.4, FLOOR[1], "chassis", rib_x=(-30.4, 30.4))
    chassis = cover_bosses(chassis, cover_screws, Y_BOT)
    pockets = Part()
    for i, x in enumerate((-15.0, 15.0)):
        slits, bump, pocket = wall_snap(u, f"cover snap {i}", Pos(x, Y_TOP - 2.0, 0), 8.0, WALL_TOP)
        chassis = chassis - slits + bump
        pockets += pocket
    u.add("chassis", chassis, PRINTED, "Chassis tray (slab, 17 mm taller), PETG/ASA")
    chassis_screws(u, floor_screws, FLOOR[1])

    bridge = rrect(0, 3.25, 60.0, 68.5, 4.0, b0, b1) - bridge_cuts(u, L) + bridge_extras(u, L)
    for x, _ in cover_screws:
        bridge -= box(x - M3_BOSS / 2 - 0.3, x + M3_BOSS / 2 + 0.3, Y_BOT, Y_BOT + 12.0, b0 - 1, b1 + 1)
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor mount")
    base_joints(u)
    u.joint("motor", "bridge")
    for i, (x, y) in enumerate(bridge_posts):
        add_stack_screw(u, f"screw_bridge_{i}", x, y, b1, b0, "chassis", ["bridge"])

    # --- Hall switches: three SOT-23 adapters, chip-down, hung under a printed carrier. The
    # adapters' chip faces are where the product PCB's underside is, so the switches see the
    # magnet as they will on the product. Their header pins come up through the carrier.
    adapter = P.sot23_adapter_with_hall()
    c0 = L.board_bottom + 1.6 + P.HEADER_H  # the carrier rests on the headers' plastic
    c1 = c0 + 2.0
    carrier = rrect(0, -3.0, 60.0, 26.0, 3.0, c0, c1) - zcyl(13.0, c0 - 1, c1 + 1)
    hall_pin = {}
    for name, (x, y, rot) in hall_places(L, p).items():
        loc = Pos(x, y, L.board_bottom) * Rot(0, 0, rot)
        for sx in (-3.81, 3.81):  # a slot for each 1x3 row of pins
            carrier -= loc * box(sx - 1.0, sx + 1.0, -3.6, 3.6, c0 - L.board_bottom - 1, c1 - L.board_bottom + 1)
        u.add(name, adapter.at(loc), BOUGHT,
              "SparkFun BOB-00717 SOT-23 adapter with a DRV5032FADBZR and 2x 1x3 header, worn chip-down")
        u.joint(name, "hall_carrier")
        hall_pin[name] = {sig: adapter.pin(loc, h) for sig, h in HALL_HOLE.items()}
    u.add("hall_carrier", carrier, PRINTED,
          "Hall carrier: holds the three adapters chip-down (glued in its slots), PETG")
    for i, (x, y) in enumerate(carrier_posts):
        u.add(f"carrier_spacer_{i}", zcyl(5.0, b1, c0, x, y), PRINTED, "Carrier spacer tube, 5 mm OD")
        u.joint(f"carrier_spacer_{i}", "bridge")
        u.joint(f"carrier_spacer_{i}", "hall_carrier")
        add_stack_screw(u, f"screw_carrier_{i}", x, y, c1, b0, "chassis",
                        ["hall_carrier", f"carrier_spacer_{i}", "bridge"])

    # --- deck: replaces the main PCB; removable (four screws) to change the cells ---
    deck = rrect(0, sum(DECK_Y) / 2, 60.0, DECK_Y[1] - DECK_Y[0], 3.0, *DECK_Z)
    deck -= box(-11.0, 11.0, DECK_Y[0] - 1, 36.0, DECK_Z[0] - 1, DECK_Z[1] + 1)  # round the motor
    for sx in (-1, 1):  # slots for the battery leads, over the near contacts' tabs
        deck -= box(min(sx * 14.0, sx * 29.0), max(sx * 14.0, sx * 29.0), 36.5, 42.0, DECK_Z[0] - 1, DECK_Z[1] + 1)
    u.add("deck", deck, PRINTED, "Breadboard deck (in place of the main PCB), PETG")
    u.joint("deck", "chassis")
    for i, (x, y) in enumerate(deck_posts_near):
        u.add(f"spacer_{i}", zcyl(5.0, b1, DECK_Z[0], x, y), PRINTED, "Deck spacer tube, 5 mm OD")
        u.joint(f"spacer_{i}", "bridge")
        u.joint(f"spacer_{i}", "deck")
        add_stack_screw(u, f"screw_stack_{i}", x, y, DECK_Z[1], b0, "chassis", ["deck", f"spacer_{i}", "bridge"])

    # --- the two breadboards, the DevKitM-1 and the DRV8833 carrier ---
    for name in BOARDS:
        _breadboard(u, name, set(deck_posts_far))
    u.add("devkit", P.devkitm_1().at(DEVKIT_LOC), BOUGHT,
          "Espressif ESP32-C6-DevKitM-1 (the product's ESP32-C6-MINI-1 module), on its pin headers")
    u.joint("devkit", "bb_mcu")
    u.add("drv8833", P.pololu_2130().at(DRV_LOC), BOUGHT,
          "Pololu #2130 DRV8833 dual motor driver carrier, on 2x 1x8 pin headers")
    u.joint("drv8833", "bb_drv")
    pcb_top = BB_TOP + P.HEADER_H + P.DEVKITM_PCB_T

    # --- cover: the slab's shape, a big front window, a slot for the USB cables ---
    yc = BOARDS["bb_mcu"]
    cover = rrect(0, cy, W, H, R, 0, DEPTH) - rrect(0, cy, W - 4, H - 4, R - 2, -1, DEPTH - 2)
    cover -= zcyl(13.0, DEPTH - 3, DEPTH + 1)
    cover -= rrect(0, 79.0, 56.0, 76.0, 4.0, DEPTH - 3, DEPTH + 1)
    cover -= box(-W / 2 - 1, -W / 2 + 3, yc - 12.0, yc + 12.0, pcb_top - 1.0, pcb_top + 6.0)
    cover -= pockets
    u.add("cover", cover, PRINTED, "Open cover with a front window and USB slot, PETG (clear works)")
    u.joint("cover", "chassis")
    for i, (x, z) in enumerate(cover_screws):
        cover_screw(u, i, x, Y_BOT, z)
    knob_on_shaft(u, "knob", "Manual knob, PETG/ASA", zcyl(40.0, DEPTH + 0.5, DEPTH + 12.5),
                  DEPTH + 0.5, L.shaft_top)

    # --- wires ---
    def jumper(a, b, colour, desc, ends, h=5.0):
        _wire(u, [a, _lift(a, h), _lift(b, h), b], colour, desc, ends)

    def run(a, b, colour, desc, ends, via):
        _wire(u, [a, *via, b], colour, desc, ends)

    drv_bb = ("bb_mcu", "bb_drv")
    jumper(devkit_pin("18"), drv_pin("AIN1"), "yellow", "GPIO 18 to DRV8833 AIN1", drv_bb, 6.0)
    jumper(devkit_pin("19"), drv_pin("AIN2"), "orange", "GPIO 19 to DRV8833 AIN2", drv_bb, 7.0)
    jumper(devkit_pin("20"), drv_pin("nSLEEP"), "white", "GPIO 20 to DRV8833 nSLEEP", drv_bb, 9.0)
    jumper(devkit_pin("3"), drv_pin("nFAULT"), "purple", "GPIO 3 to DRV8833 nFAULT", drv_bb, 11.0)
    jumper(devkit_pin("3V3"), rail("3V3", "f"), "red", "3V3 to the 3V3 row", drv_bb, 10.0)
    jumper(devkit_pin("G", "J3"), rail("GND", "f"), "black", "J3 GND to the GND row", drv_bb, 8.0)
    jumper(rail("GND", "g"), hole("bb_drv", DRV_ROW_GND, "h"), "black", "GND row to the DRV8833 GND",
           ("bb_drv", "bb_drv"), 3.0)

    # Motor + encoder: one 6-pin cable from the side connector on the encoder.
    mx, my = u.motor_xy
    conn = P.micro_gearmotor().pin(Pos(mx, my, b1), "connector")
    cable = {"pin 6, M1": (drv_pin("AOUT1"), "red", "bb_drv"), "pin 5, M2": (drv_pin("AOUT2"), "black", "bb_drv"),
             "pin 4, encoder VCC": (rail("3V3", "h"), "red", "bb_drv"),
             "pin 1, encoder GND": (rail("GND", "i"), "black", "bb_drv"),
             "pin 3, encoder A": (devkit_pin("21"), "yellow", "bb_mcu"),
             "pin 2, encoder B": (devkit_pin("4"), "green", "bb_mcu")}
    for k, (sig, (src, colour, bb)) in enumerate(cable.items()):
        end = (conn[0] - 3.125 + k * 1.25, conn[1], conn[2])
        run(src, end, colour, f"motor cable {sig}", (bb, "motor"),
            [_lift(src, 8.0 + k), (end[0], conn[1] + 4.0, BB_TOP + 8.0 + k), (end[0], conn[1] + 3.0, conn[2])])

    # Hall switches: OUT to a GPIO; VCC from the 3V3 row; GND from the GND row, which has
    # five holes (f-j), so the door switch takes the driver's power-row GND instead.
    vcc = {"hall_locked": rail("3V3", "g"), "hall_unlocked": rail("3V3", "i"), "hall_door": rail("3V3", "j")}
    gnd = {"hall_locked": rail("GND", "j"), "hall_unlocked": rail("GND", "h"),
           "hall_door": hole("bb_drv", DRV_ROW_GND, "b")}
    for k, (name, gpio) in enumerate(HALLS.items()):
        for j, (sig, src, colour, bb) in enumerate((("OUT", devkit_pin(gpio), "blue", "bb_mcu"),
                                                    ("VCC", vcc[name], "red", "bb_drv"),
                                                    ("GND", gnd[name], "black", "bb_drv"))):
            e = hall_pin[name][sig]
            lift = 3.0 + 1.0 * (3 * k + j)  # all under the cover (inner face at DEPTH - 2)
            run(src, e, colour, f"{name} {sig}", (bb, name),
                [_lift(src, lift), (e[0], 30.0, BB_TOP + lift), _lift(e, 6.0)])

    for name, sign, pin, colour in (("contact_pos", "+", "VIN", "red"), ("contact_neg", "-", "GND", "black")):
        c = u.items[name].part.bounding_box()
        a = ((c.min.X + c.max.X) / 2, c.min.Y + 0.25, c.max.Z)  # the tab; any spring runs +Y from it
        b = drv_pin(pin, "b" if pin == "VIN" else "a")
        run(a, b, colour, f"battery {sign} to the DRV8833 {pin}", (name, "bb_drv"),
            [_lift(a, 6.0), (a[0], 39.25, BB_TOP + 4.0), _lift(b, 4.0)])

    return u, build_fixture(p, locked)
