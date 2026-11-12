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

Frame: the fixture's (deadbolt/fixture.py). Which pin goes where is the net list's
(wiring/netlist.py, DEVKIT); wire_plan() here only picks the holes, colours and routes.
build_devkit.py writes the result to WIRING.md; build_wiring.py draws it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from build123d import Color, Compound, Part, Plane, Pos, Rot, Solid, Vector

from concepts import parts as P
from concepts.designs import (FLOOR, M3_BOSS, base_joints, battery_bay, chassis_posts,
                              chassis_shell, cover_bosses, floor_cuts)
from concepts.kit import (AA, BOUGHT, PRINTED, WIRE, DriveLayout, Unit, add_stack_screw,
                          back_plate, box, bridge_cuts, bridge_extras, chassis_screws, cover_screw,
                          drive_module, fix, knob_on_shaft, rrect, wall_snap, zcyl)
from deadbolt.fixture import GENERIC_US, DeadboltParams, build as build_fixture
from wiring.netlist import DEVKIT, HALL_HOLE, HALL_PARTS, NET_TITLES, PIN_MAP

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
    """The free hole beside a DevKit pin, by name ('18', '3V3', 'G')."""
    for hdr, pins in (("J1", P.DEVKITM_J1), ("J3", P.DEVKITM_J3)):
        if (not header or header == hdr) and name in pins:
            return board_hole("devkit", f"{hdr}-{pins.index(name) + 1}").xyz()
    raise KeyError(name)


# Pololu #2130: power row in column e, control row in column g, pin 1 (both GNDs) in row 12.
DRV_ROW_GND = 12
DRV_LOC = Pos(8.89, BOARDS["bb_drv"] + COL_OFF["e"] - 1.27, BB_TOP + P.HEADER_H) * Rot(0, 0, 90)


def drv_pin(name: str, col: str = "") -> tuple[float, float, float]:
    """A free hole in a carrier pin's row: power row from column c, control row from i."""
    side = "P" if name in P.POLOLU_2130_PWR else "C"
    return board_hole("drv8833", f"{side}-{name}", col).xyz()


# 3V3 and GND are shared out along two spare rows of the driver's breadboard (columns f-j).
RAIL_3V3, RAIL_GND = 15, 16


def rail(name: str, col: str) -> tuple[float, float, float]:
    return hole("bb_drv", RAIL_3V3 if name == "3V3" else RAIL_GND, col)


HALLS = {r: str(PIN_MAP[b][0]) for b, r in HALL_PARTS.items()}  # adapter -> its OUT's GPIO


# --- the wires: one per entry of wire_plan(), connecting what wiring/netlist.py DEVKIT says ---

@dataclass(frozen=True)
class Hole:
    board: str
    row: int
    col: str

    def xyz(self) -> tuple[float, float, float]:
        return hole(self.board, self.row, self.col)

    def strip(self) -> tuple[str, int, str]:
        """The five holes joined under the breadboard's surface."""
        return self.board, self.row, "a-e" if self.col in "abcde" else "f-j"


@dataclass(frozen=True)
class Term:
    """A wire end off the breadboards: a Hall adapter's hole, a motor cable pin, a battery contact."""
    ref: str
    pin: str


@dataclass(frozen=True)
class Wire:
    net: str
    colour: str
    a: Hole | Term
    b: Hole | Term
    route: str  # 'jumper' (hole to hole), 'cable', 'hall', 'battery'
    lift: float  # mm above the breadboard (jumpers, Hall wires) or the cable's index
    desc: str


def board_hole(ref: str, pin: str, col: str = "") -> Hole:
    """The hole a DevKit or carrier pin sits in, or another hole (col) of its strip. By
    default the free hole beside the header: DevKit J1 in column j, J3 in a; the carrier's
    power row from column c, its control row from i."""
    if ref == "devkit":
        hdr, k = pin.split("-")
        return Hole("bb_mcu", 17 - int(k), col or ("j" if hdr == "J1" else "a"))
    if ref == "drv8833":
        side, name = pin.split("-", 1)
        names = P.POLOLU_2130_PWR if side == "P" else P.POLOLU_2130_CTL
        return Hole("bb_drv", DRV_ROW_GND - names.index(name), col or ("c" if side == "P" else "i"))
    raise KeyError(ref)


def board_pins() -> dict[tuple[str, int, str], list[tuple[str, str]]]:
    """Strip -> the board pins in it (the DevKit's on bb_mcu, the carrier's on bb_drv)."""
    out: dict = {}
    for ref, part in DEVKIT.parts.items():
        if ref in ("devkit", "drv8833"):
            for pin in part.pins:
                out.setdefault(board_hole(ref, pin).strip(), []).append((ref, pin))
    return out


RAIL_ROW = {"+3V3": RAIL_3V3, "GND": RAIL_GND}


def rail_hole(net: str, col: str) -> Hole:
    return Hole("bb_drv", RAIL_ROW[net], col)


def _end(pin: tuple[str, str], col: str = "") -> Hole | Term:
    return board_hole(*pin, col) if pin[0] in ("devkit", "drv8833") else Term(*pin)


def wire_plan() -> list[Wire]:
    """Every wire on the dev kit. The pins come from the net list; this picks the holes (none
    takes two wires), the colours and the 3D routes. tests/test_wiring.py checks that what
    these wires join is exactly DEVKIT's nets."""
    nets = DEVKIT.nets()
    net_of = {pin: net for net, pins in nets.items() for pin in pins}
    blocks = DEVKIT.blocks

    def gpio_to(net):  # (MCU end, other end) of a signal net
        a, b = nets[net]
        return _end(a), _end(b), PIN_MAP[net][0], b

    w = []
    for net, colour, lift in (("MOT_IN1", "yellow", 6.0), ("MOT_IN2", "orange", 7.0),
                              ("DRV_nSLEEP", "white", 9.0), ("DRV_nFAULT", "purple", 11.0)):
        a, b, gpio, (_, pin) = gpio_to(net)
        w.append(Wire(net, colour, a, b, "jumper", lift, f"GPIO {gpio} to DRV8833 {pin.split('-')[1]}"))
    w.append(Wire("+3V3", "red", _end(blocks[("MCU", "3V3")]), rail_hole("+3V3", "f"), "jumper", 10.0,
                  "3V3 to the 3V3 row"))
    w.append(Wire("GND", "black", _end(blocks[("MCU", "GND")]), rail_hole("GND", "f"), "jumper", 8.0,
                  "J3 GND to the GND row"))
    w.append(Wire("GND", "black", rail_hole("GND", "g"), _end(blocks[("DRIVER", "GND")], "h"), "jumper", 3.0,
                  "GND row to the DRV8833 GND"))

    # Motor + encoder: the 6-pin cable, its bare ends into the breadboards.
    rail_cols = {"+3V3": "h", "GND": "i"}
    for k, pin in enumerate("654132"):
        net = DEVKIT.net_of(net_of[("motor", pin)])
        if net in RAIL_ROW:
            src = rail_hole(net, rail_cols[net])
        else:
            src = _end(next(p for p in nets[net] if p != ("motor", pin)))
        name = P.MOTOR_PINS[int(pin) - 1].replace("enc ", "encoder ")
        colour = {"M1": "red", "M2": "black", "encoder VCC": "red", "encoder GND": "black",
                  "encoder A": "yellow", "encoder B": "green"}[name]
        w.append(Wire(net, colour, src, Term("motor", pin), "cable", k, f"motor cable pin {pin}, {name}"))

    # Hall switches: OUT to a GPIO; VCC from the 3V3 row; GND from the GND row, which has
    # five holes (f-j), so the door switch takes the carrier's power-row GND instead.
    vcc_col = {"hall_locked": "g", "hall_unlocked": "i", "hall_door": "j"}
    gnd = {"hall_locked": rail_hole("GND", "j"), "hall_unlocked": rail_hole("GND", "h"),
           "hall_door": board_hole("drv8833", "P-GND", "b")}
    for k, ref in enumerate(HALL_PARTS.values()):
        for j, sig in enumerate(("OUT", "VCC", "GND")):
            end = Term(ref, HALL_HOLE[sig])
            if sig == "OUT":
                src = _end(nets[net_of[(ref, end.pin)]][0])
            elif sig == "VCC":
                src = rail_hole("+3V3", vcc_col[ref])
            else:
                src = gnd[ref]
            colour = {"OUT": "blue", "VCC": "red", "GND": "black"}[sig]
            w.append(Wire(net_of[(ref, end.pin)], colour, src, end, "hall", 3.0 + (3 * k + j), f"{ref} {sig}"))

    for sign, block_pin, colour, col in (("+", ("DRIVER", "VM"), "red", "b"), ("-", ("BATTERY", "-"), "black", "a")):
        drv = blocks[block_pin] if sign == "+" else ("drv8833", "P-GND")
        w.append(Wire(net_of[("battery", sign)], colour, Term("battery", sign), _end(drv, col), "battery", 0.0,
                      f"battery {sign} to the DRV8833 {drv[1].split('-')[1]}"))
    return w


def plan_connectivity(plan: list[Wire] | None = None) -> set[frozenset[tuple[str, str]]]:
    """What the wires, the breadboard strips and the boards' own copper join: the nets a
    continuity tester would find on the built dev kit."""
    from wiring.netlist import merge
    groups = [set(pins) | {strip} for strip, pins in board_pins().items()]
    for wr in plan or wire_plan():
        groups.append({e.strip() if isinstance(e, Hole) else (e.ref, e.pin) for e in (wr.a, wr.b)})
    groups += [set(t) for t in DEVKIT.ties]
    nets = {frozenset(p for p in g if len(p) == 2) for g in merge(groups)}
    return {n for n in nets if len(n) > 1}  # a board pin alone in its strip is no net


def end_label(e: Hole | Term) -> str:
    """Where a wire end goes, in words, with its breadboard hole."""
    if isinstance(e, Term):
        if e.ref == "motor":
            return f"motor cable pin {e.pin}"
        if e.ref == "battery":
            return f"4xAA {e.pin} contact"
        return f"{e.ref} hole {e.pin} ({next(s for s, h in HALL_HOLE.items() if h == e.pin)})"
    where = f"{'DRV' if e.board == 'bb_drv' else 'MCU'} breadboard {e.row}{e.col}"
    pins = board_pins().get(e.strip(), [])
    if pins:
        ref, pin = pins[0]
        name = (P.DEVKITM_J1 + P.DEVKITM_J3)[(int(pin[3:]) - 1) + (15 if pin.startswith("J3") else 0)] \
            if ref == "devkit" else pin.split("-", 1)[1]
        name = f"GPIO {name}" if name.isdigit() else name
        return f"{'DevKit' if ref == 'devkit' else 'DRV8833'} {name} ({where})"
    return f"{'3V3' if e.row == RAIL_3V3 else 'GND'} row ({where})"


# (signal, from, to, colour), for WIRING.md
WIRING = [(NET_TITLES[DEVKIT.net_of(wr.net)], end_label(wr.a), end_label(wr.b), wr.colour) for wr in wire_plan()]
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
        hall_pin[name] = {h: adapter.pin(loc, h) for h in HALL_HOLE.values()}
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

    # --- wires: the net list's, routed over the boards ---
    mx, my = u.motor_xy
    conn = P.micro_gearmotor().pin(Pos(mx, my, b1), "connector")
    contact = {"+": "contact_pos", "-": "contact_neg"}
    for wr in wire_plan():
        if wr.route == "jumper":
            a, b = wr.a.xyz(), wr.b.xyz()
            pts, ends = [a, _lift(a, wr.lift), _lift(b, wr.lift), b], (wr.a.board, wr.b.board)
        elif wr.route == "cable":  # from the connector on the encoder, over to the breadboards
            k, src = int(wr.lift), wr.a.xyz()
            end = (conn[0] - 3.125 + k * 1.25, conn[1], conn[2])
            pts = [src, _lift(src, 8.0 + k), (end[0], conn[1] + 4.0, BB_TOP + 8.0 + k),
                   (end[0], conn[1] + 3.0, conn[2]), end]
            ends = (wr.a.board, "motor")
        elif wr.route == "hall":  # all under the cover (inner face at DEPTH - 2)
            src, e = wr.a.xyz(), hall_pin[wr.b.ref][wr.b.pin]
            pts = [src, _lift(src, wr.lift), (e[0], 30.0, BB_TOP + wr.lift), _lift(e, 6.0), e]
            ends = (wr.a.board, wr.b.ref)
        else:  # battery: from the end contact's tab; any spring runs +Y from it
            c = u.items[contact[wr.a.pin]].part.bounding_box()
            a, b = ((c.min.X + c.max.X) / 2, c.min.Y + 0.25, c.max.Z), wr.b.xyz()
            pts = [a, _lift(a, 6.0), (a[0], 39.25, BB_TOP + 4.0), _lift(b, 4.0), b]
            ends = (contact[wr.a.pin], wr.b.board)
        _wire(u, pts, wr.colour, wr.desc, ends)
        u.items[[n for n in u.items if n.startswith("wire_")][-1]].plan = wr

    return u, build_fixture(p, locked)
