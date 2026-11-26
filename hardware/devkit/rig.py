"""Dev kit: the tall slab (concept B), opened up for development, and put together with
snaps instead of screws.

Same door mounting, drive, motor and battery bay as the slab, and the same chips as its
main PCB, each on a breakout (concepts/parts.py), so what is learned on it carries over:

- the main PCB is replaced by a removable deck carrying two mini breadboards: the
  ESP32-C6-DevKitM-1 (the product's ESP32-C6-MINI-1 module) on one, the Pololu #2130
  DRV8833 carrier on the other
- the three DRV5032 Hall switches (bolt locked, bolt unlocked, door) are on SparkFun SOT-23
  adapters, worn chip-down in a printed carrier at the product PCB's height, so they sit
  over the bolt flag's magnet exactly as on the product
- the cover has a big front window (the wiring stays in view) and a slot for the
  DevKit's USB cables; it is 17 mm taller than the slab's to fit the second breadboard
- the TPS629206 regulator is the one product chip not in the kit: the DevKit makes its
  own 3.3 V from USB, so battery life is measured on the first product PCB

Fastening. The only screws are the deadbolt's own two through-screws, which hold a printed
back plate (a bar across the deadbolt) to the door. Everything else snaps or interlocks
(concepts/snaps.py):

| Joint | How |
|---|---|
| chassis to back plate | hangs on two keyhole pegs: on 6 mm high, slide down; a tongue in the floor drops into a notch |
| bridge, Hall carrier, deck | sit on pegged posts; spring tabs on their edges click into windows in the chassis walls |
| motor | in a sleeve on the bridge; two arms hook over its encoder board |
| pinion, bolt magnet | pressed on (glue the magnet) |
| breadboards | in a fenced pocket on the deck, an arm hooked over each end (and their adhesive back) |
| cover | four arms in the chassis walls click into windows in its sides; press them to take it off |
| knob | two fingers in its bore click into a groove on the coupler's shaft |

Every printed fit has play (SLIDE_FIT, FIT); concepts/assembly.py checks each step of
ASSEMBLY can actually be done in order, and that nothing fits line to line.

Frame: the fixture's (deadbolt/fixture.py). Which pin goes where is the net list's
(wiring/netlist.py, DEVKIT); wire_plan() here only picks the holes, colours and routes.
build_devkit.py writes the result to WIRING.md; build_wiring.py draws it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from build123d import Axis, Color, Compound, Cone, Part, Plane, Polygon, Pos, Rot, Solid, Vector, revolve

from concepts import parts as P
from concepts.assembly import DOWN, UP, Step, step
from concepts.designs import FLOOR, base_joints, battery_bay, floor_cuts
from concepts.kit import (AA, BOUGHT, FASTENER, PRINTED, SHAFT_D, WIRE, DriveLayout, ScrewType, Snap, Unit, box,
                          bridge_cuts, bridge_extras, d_shaft, drive_module, rrect, screw_solid, sector, zcyl)
from concepts.snaps import SLIDE_FIT, add_barb, edge_tab, hook_arm, keyhole, wall_latch
from deadbolt.fixture import GENERIC_US, DeadboltParams, _post_xs, build as build_fixture
from wiring.netlist import DEVKIT, HALL_HOLE, HALL_PARTS, NET_TITLES, PIN_MAP

W, Y_BOT, Y_TOP, R, DEPTH = 68.0, -35.0, 120.0, 8.0, 51.5  # the cover: the slab's, 17 mm taller
CY, H = (Y_BOT + Y_TOP) / 2, Y_TOP - Y_BOT
COVER_T, COVER_GAP = 2.0, 0.25  # cover wall, and its play over the chassis wall
WALL_T, WALL_TOP = 1.6, 30.0  # chassis wall; tall enough for the deck's and carrier's tab windows
X_OUT = W / 2 - COVER_T - COVER_GAP  # chassis wall, outer face
X_IN = X_OUT - WALL_T  # and inner
FIT = 0.15  # play per side on keyed fits (D-bores on the coupler's shaft, the motor sleeve)
EDGE = X_IN - 0.45  # edge of the bridge, carrier and deck: 0.45 inside the walls
DECK_Z = (21.0, 25.5)  # over the cells (their tops are at 19.5); 4.5 mm stiffens it
DECK_Y = (26.0, CY + H / 2 - COVER_T - COVER_GAP - WALL_T - 0.3)

# Back plate: a printed bar across the deadbolt, on its two through-screws. The chassis
# hangs on its keyhole pegs. A bar, not a plate under the whole chassis: the chassis floor
# prints over the pocket it sits in, and 25 mm is a span a printer bridges cleanly.
PLATE_T = 3.0
PLATE_X, PLATE_Y = (-29.0, 29.0), (-10.0, 8.0)
SLIDE = 6.0  # the chassis goes on this much high, then slides down onto the pegs
KEY_PEGS = [(12.0, 4.5), (-12.0, 4.5)]
# The floor tongue: x span, root and tip (y), the hole the plate's bump drops into, the bump's height.
TONGUE = dict(x=(21.5, 28.3), root=PLATE_Y[0] - SLIDE - SLIDE_FIT, tip=-3.0, hole=(-6.6, -4.2), drop=0.8)

# Mini breadboard (170 points: 17 rows x 10 columns, no rails). Check yours: they vary.
MINI_BB = dict(l=47.0, w=35.0, h=8.5, margin=1.9)  # margin: plain plastic past the end rows
BB_TOP = DECK_Z[1] + MINI_BB["h"]
BOARDS = {"bb_drv": 60.0, "bb_mcu": 97.0}  # centre y of each breadboard; both centred on x = 0
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


SHAFT_R = SHAFT_D / 2
PEG_D = 3.0  # locating pegs on post tops; the part on them has a SLIDE_FIT hole


def outline(z0: float, z1: float, inset: float) -> Part:
    """The chassis's outline (inset from its wall's outer face), z0..z1."""
    k = COVER_T + COVER_GAP + inset
    return rrect(0, CY, W - 2 * k, H - 2 * k, R - k, z0, z1)


def posts(xys, z0: float, top: float, d: float = 7.0, peg: float = 2.0) -> Part:
    """Round posts from z0 to top, each with a locating peg on top."""
    out = Part()
    for x, y in xys:
        out += zcyl(d, z0, top, x, y) + zcyl(PEG_D, top - 0.01, top + peg, x, y)
    return out


def peg_holes(xys, z0: float, z1: float) -> Part:
    out = Part()
    for x, y in xys:
        out += zcyl(PEG_D + 2 * SLIDE_FIT, z0, z1, x, y)
    return out


def back_plate(u: Unit, p: DeadboltParams) -> None:
    """The printed bar on the deadbolt's two through-screws, with the chassis's keyhole pegs
    and the bump its floor tongue drops over."""
    (x0, x1), (y0, y1) = PLATE_X, PLATE_Y
    plate = rrect((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0, 2.0, 0, PLATE_T) - zcyl(10.0, -1, PLATE_T + 1)
    door = ScrewType(p.post_hole_d, p.post_hole_d, "csk", 7.5, 2.3, (), "")  # thread per brand
    slope = (door.head_d - door.d) / 2 / door.head_h
    seat = PLATE_T - 0.25 / slope * 1.0  # a 0.25 mm wider recess: the head seats this far down
    for x in _post_xs(p):
        hole_r = door.d / 2 + 0.25
        plate -= zcyl(2 * hole_r, -1, PLATE_T + 1, x, 0)
        h = door.head_h
        plate -= Pos(x, 0, PLATE_T - h / 2) * Cone(door.d / 2 + 0.25, door.head_d / 2 + 0.25, h)
        name = f"screw_door_{'r' if x > 0 else 'l'}"
        u.add(name, Pos(x, 0, seat) * screw_solid(door, 22.0), FASTENER,
              "Deadbolt through-screw, flat head (thread per brand, e.g. #8-32 x 1\")")
        u.joint(name, "plate", 0.05)
        u.joint(name, "door")
        u.press[frozenset({name, "plate"})] = "countersunk head seated in its recess"
        u.press[frozenset({name, "exterior_cylinder"})] = "threaded into the deadbolt's post"
    for x, y in KEY_PEGS:
        plate += keyhole(x, y, SLIDE, PLATE_T, FLOOR[1])[0]
    t = TONGUE
    bump = box(t["x"][0] + 1.9, t["x"][1] - 1.9, t["hole"][0] + 0.2, t["hole"][1] - 0.2, PLATE_T - 0.01,
               PLATE_T + t["drop"])
    plate += bump
    add_barb(u, "plate", bump)
    u.add("plate", plate, PRINTED, "Back plate: a bar on the deadbolt's two screws, 2 keyhole pegs, PETG")
    u.joint("plate", "door")


def chassis_floor_tongue(u: Unit) -> Part:
    """The tongue in the chassis floor that locks it on the plate: a slot round three sides
    and a hole the plate's bump drops into. It flexes up out of the floor's plane, which
    bends it along its layers. Returns the cut."""
    t = TONGUE
    (xa, xb), root, tip = t["x"], t["root"], t["tip"]
    z0, z1 = FLOOR[0] - 0.1, FLOOR[1] + 0.1
    cut = box(xa - 0.8, xa, root, tip + 0.8, z0, z1) + box(xb, xb + 0.8, root, tip + 0.8, z0, z1) \
        + box(xa - 0.8, xb + 0.8, tip, tip + 0.8, z0, z1) \
        + box(xa + 1.7, xb - 1.7, t["hole"][0], t["hole"][1], z0, z1)
    u.snaps.append(Snap("chassis floor tongue", FLOOR[1] - FLOOR[0], sum(t["hole"]) / 2 - root, t["drop"], "along"))
    return cut


def snap_knob(u: Unit, z0: float, z1: float) -> None:
    """The knob, keyed on the coupler's D-shaft; two fingers in its bore click into a groove
    round the shaft. Pull it off firmly: the groove's upper face is a 45 deg ramp."""
    ang = u.shaft_angle
    r_in = SHAFT_R + FIT
    root = z1 - 2.0  # the fingers hang from the knob's 2 mm top
    knob = zcyl(40.0, z0, z1) - d_shaft(ang, z0 - 1, root, FIT)
    groove = Polygon((SHAFT_R - 0.6, z0 + 0.8), (SHAFT_R + 0.2, z0 + 0.8), (SHAFT_R + 0.2, z0 + 3.4),
                     (SHAFT_R - 0.6, z0 + 2.6), align=None)
    u.cut("coupler", revolve(Plane.XZ * groove, Axis.Z))
    tooth = Polygon((r_in, z0 + 1.0), (SHAFT_R - 0.45, z0 + 1.6), (SHAFT_R - 0.45, z0 + 2.4), (r_in, z0 + 2.4),
                    align=None)
    for i, a in enumerate((ang + 90.0, ang - 90.0)):
        knob -= sector(r_in - 0.01, r_in + 2.2, a - 29, a + 29, z0 - 1, root) \
            - sector(r_in - 0.02, r_in + 1.2, a - 25, a + 25, z0 - 1, root)
        barb = Rot(0, 0, a - 22) * revolve(Plane.XZ * tooth, Axis.Z, 44)
        knob += barb
        add_barb(u, "knob", barb)
        u.snaps.append(Snap(f"knob finger {i}", 1.2, root - (z0 + 2.0), 0.45, "across"))
    u.add("knob", knob, PRINTED, "Manual knob, snaps onto the coupler's shaft, PETG/ASA")
    u.joint("knob", "coupler", 0.2)


def _notch(s: int, y: float, x_from: float, z0: float, z1: float) -> Part:
    """Room beside a cover latch arm (at y on side s) for it to flex in."""
    return box(min(s * x_from, s * (EDGE + 1)), max(s * x_from, s * (EDGE + 1)), y - 5.6, y + 5.6, z0 - 1, z1 + 1)


def _breadboard(u: Unit, name: str) -> tuple[Part, Part]:
    """A mini breadboard in a fenced pocket on the deck: an arm at each end hooks over its
    end margin (the arms' roots are sunk into the deck, so they're long enough to flex), and
    low fences either side stop it sliding. Its adhesive back is optional. Returns what to
    add to and cut from the deck (deck geometry, so the caller adds it before the deck)."""
    yc = BOARDS[name]
    l, w, h = MINI_BB["l"], MINI_BB["w"], MINI_BB["h"]
    u.add(name, box(-l / 2, l / 2, yc - w / 2, yc + w / 2, DECK_Z[1], DECK_Z[1] + h), BOUGHT,
          "Mini breadboard, 170 points (47 x 35 mm), adhesive back")
    u.items[name].part.color = Color(0.95, 0.95, 0.93)
    u.joint(name, "deck")
    add, cut = Part(), Part()
    for sx in (-1, 1):
        for k, dy in enumerate((-8.0, 8.0)):
            loc = Pos(sx * l / 2, yc + dy, 0) * Rot(0, 0, -90 * sx)
            arm, pocket = hook_arm(u, f"{name} clip {'r' if sx > 0 else 'l'}{k}", "deck", loc, DECK_Z[0] + 0.8,
                                   BB_TOP, catch=MINI_BB["margin"] - 1.0, sink_from=DECK_Z[1])
            add += arm
            cut += pocket
    for sy in (-1, 1):
        y0 = yc + sy * (w / 2 + SLIDE_FIT)
        for x in (-8.0, 8.0):
            add += box(x - 2, x + 2, min(y0, y0 + sy * 1.4), max(y0, y0 + sy * 1.4), DECK_Z[1] - 0.01, DECK_Z[1] + 1.5)
    return add, cut


def hall_places(L: DriveLayout, p: DeadboltParams) -> dict[str, tuple[float, float, float]]:
    """(x, y, rot) of each Hall adapter: the bolt ones over the magnet's two stops, their
    7.62 mm side radial so they clear the shaft; the door one by the latch edge."""
    out = {}
    for name, locked in (("hall_locked", True), ("hall_unlocked", False)):
        x, y = L.hall_xy(p, locked)
        out[name] = (x, y, math.degrees(math.atan2(y, x)) - 90.0)
    out["hall_door"] = (24.0, 4.0, 90.0)
    return out


# How each printed part goes on the bed (rotation from the model's pose), and what to know.
# build_devkit.py exports the STLs in this pose and lists the overhangs it leaves.
FLIP = (180.0, 0.0, 0.0)
PRINT = {
    "plate": ((0, 0, 0), "Pegs up. 100% infill round the screw holes, or 4 perimeters."),
    "chassis": ((0, 0, 0), "Skirt down. The floor bridges the 25 mm plate pocket; the wall windows bridge too."),
    "bridge": ((0, 0, 0), "Flat side down; sleeve, standoffs and motor clips up."),
    "hall_carrier": ((0, 0, 0), "Flat."),
    "deck": ((0, 0, 0), "Flat, clips up."),
    "cover": (FLIP, "Front face down, open side up. Clear PETG shows the wiring."),
    "knob": (FLIP, "Top face down, fingers up."),
    "coupler": ((0, 0, 0), "Hub down. Needs support under the lug (2.5 mm up the hub); or print it in PA12 (SLS)."),
    "ring_gear": ((0, 0, 0), "Flat."),
    "pinion": ((0, 0, 0), "Flat. Or buy a brass m1 10T pinion for a 3 mm D shaft."),
    "bolt_flag": ((0, 0, 0), "Flat, magnet pocket up."),
}


def assembly_steps(u: Unit) -> list[Step]:
    """The order the dev kit goes together in. concepts/assembly.py checks every step can be
    done; build_devkit.py writes them out as ASSEMBLY.md."""
    has = lambda pre: sorted(n for n in u.items if n.startswith(pre))
    slide_down = ((0.0, -1.0, 0.0), SLIDE)
    return [
        step("plate", "Hold the back plate over the deadbolt, pegs out, the bump towards the latch edge."),
        step(has("screw_door"), "Screw it to the door with the deadbolt's own two through-screws."),
        step("chassis", f"Hang the chassis on the plate: offer it up {SLIDE:g} mm high, so the pegs' heads go "
             "through the round holes in its floor, then slide it down until it stops and the floor's tongue "
             f"clicks over the plate's bump. (Off: lift the tongue's tip, slide it up {SLIDE:g} mm.)",
             (DOWN, None), slide_down),
        step("coupler", "Slide the coupler over the tailpiece, through the floor."),
        step("ring_gear", "Drop the output gear over the coupler's hub, its pocket round the lug."),
        step("bridge", "Bridge, on the bench:", sub="bridge"),
        step("motor", "push the motor into the bridge's sleeve, connector towards the top of the door, until "
             "both arms click over its encoder board;", sub="bridge"),
        step("pinion", "press the pinion onto the motor's shaft from underneath, flat to flat.", (UP, None),
             sub="bridge"),
        step([], "Lower the bridge onto its four posts, the pinion meshing with the gear, until its four edge "
             "tabs click into the chassis walls. (Out: squeeze each tab's tip in.)", carry="bridge"),
        step("bolt_flag", "Bolt flag:", sub="flag"),
        step("bolt_magnet", "press the magnet into its pocket in the flag (a drop of glue), the face that "
             "triggers the switches up.", sub="flag"),
        step([], "Slide the flag down the coupler's shaft onto the bridge, flat to flat.", carry="flag"),
        step("hall_carrier", "Hall carrier, on the bench:", sub="carrier"),
        step(["hall_locked", "hall_unlocked", "hall_door"], "glue the three Hall adapters chip-down under it, "
             "their header pins up through its slots.", (UP, None), sub="carrier"),
        step([], "Lower the carrier onto its three standoffs until its two edge tabs click into the walls.",
             carry="carrier"),
        step(has("contact_"), "Drop the battery contacts into the slots in the battery ribs (solder the leads "
             "to the two end contacts first)."),
        step(has("cell_"), "Fit the four AA cells, + and - alternating as moulded in the ribs."),
        step("deck", "Lower the deck onto its four posts, the battery leads up through its slots, until its "
             "four edge tabs click into the walls. (Out, to change cells: squeeze the tabs in, a pair at a time.)"),
        step(["bb_drv", "bb_mcu"], "Press each breadboard into its pocket until the arms at its ends click over it."),
        step(["devkit", "drv8833"], "Plug the DevKit and the DRV8833 carrier into their breadboards."),
        step("cover", "Wire it up (SETUP.md). Then push the cover on until its four latches click into the "
             "windows in its sides. (Off: press the four latches in through the windows.)"),
        step("knob", "Push the knob onto the shaft, flat to flat, until it clicks."),
    ]


def rig(p: DeadboltParams = GENERIC_US, locked: bool = True) -> tuple[Unit, dict[str, Part]]:
    """The dev kit and the door + deadbolt it mounts on."""
    u = Unit()
    L = DriveLayout(gear_z0=5.5, gear_teeth=40, motor="n20", motor_angle_deg=90.0,
                    shaft_top=DEPTH + 0.5 + 6.0, fit=FIT, motor_screws=False)
    b0, b1 = L.bridge_z
    bridge_posts = [(21.0, -26.0), (-21.0, -26.0), (23.0, 30.0), (-23.0, 30.0)]
    deck_near = [(23.0, 30.0), (-23.0, 30.0)]  # standoffs on the bridge, over its posts
    deck_far = [(23.0, 110.0), (-23.0, 110.0)]  # tall posts on the floor, beyond the cells
    carrier_posts = [(-22.0, -7.0), (-22.0, 7.0), (22.0, -9.0)]  # standoffs on the bridge
    cover_latches = [(s, y) for y in (6.0, 75.0) for s in (1, -1)]
    windows = Part()  # tab windows in the chassis walls

    back_plate(u, p)
    drive_module(u, p, locked, L)

    # --- chassis: floor on the plate, a skirt round it down to the door, walls ---
    (x0, x1), (y0, y1) = PLATE_X, PLATE_Y
    pocket = rrect((x0 + x1) / 2, (y0 + y1 - SLIDE) / 2, x1 - x0 + 2 * SLIDE_FIT, y1 - y0 + SLIDE + 2 * SLIDE_FIT,
                   2.0 + SLIDE_FIT, -1, FLOOR[0])
    chassis = outline(*FLOOR, 0) + (outline(0, FLOOR[0], 0) - pocket) \
        + (outline(FLOOR[1], WALL_TOP, 0) - outline(FLOOR[1] - 1, WALL_TOP + 1, WALL_T))
    chassis += posts(bridge_posts, FLOOR[1], b0) + posts(deck_far, FLOOR[1], DECK_Z[0])
    chassis = floor_cuts(chassis) - chassis_floor_tongue(u)
    tp = max(p.tailpiece_w, p.tailpiece_t) / 2 + 0.3  # the tailpiece passes as the chassis slides down
    chassis -= box(-tp, tp, -SLIDE - tp, 0, FLOOR[0] - 1, FLOOR[1] + 1)
    for x, y in KEY_PEGS:
        chassis -= keyhole(x, y, SLIDE, PLATE_T, FLOOR[1])[1]
    chassis += battery_bay(u, dict(AA, desc="AA 1.5 V lithium cell (Energizer L91)"),
                           [-22.5, -7.5, 7.5, 22.5], 38.4, FLOOR[1], "chassis", rib_x=(-30.4, 30.4))
    cover_windows = Part()
    for i, (s, y) in enumerate(cover_latches):
        slits, barb, window = wall_latch(u, f"cover latch {i}", "chassis", Pos(s * X_OUT, y, 0) * Rot(0, 0, -90 * s),
                                         13.0, WALL_TOP, 27.0, WALL_T, gap=COVER_GAP, cover_t=COVER_T)
        chassis = chassis - slits + barb
        cover_windows += window
    u.add("chassis", chassis, PRINTED, "Chassis tray, hangs on the back plate, PETG/ASA")

    # --- bridge: gear retainer and motor mount, on four posts; tabs click into the walls ---
    bridge = rrect(0, 3.5, 2 * EDGE, 68.0, 4.0, b0, b1) - bridge_cuts(u, L) \
        - peg_holes(bridge_posts, b0 - 1, b0 + 2.3) + bridge_extras(u, L)
    c0 = L.board_bottom + 1.6 + P.HEADER_H  # the Hall carrier rests on the adapters' header plastic
    c1 = c0 + 2.0
    bridge += posts(deck_near, b1 - 0.01, DECK_Z[0], d=6.0) + posts(carrier_posts, b1 - 0.01, c0, d=6.0, peg=1.5)
    for s in (1, -1):
        for root, tip in ((-10.0, -22.0), (21.0, 33.0)):
            cut, barb, window = edge_tab(u, f"bridge tab {s:+d} {tip:g}", "bridge", s, EDGE, root, tip, b0, b1, X_IN)
            bridge = bridge - cut + barb
            windows += window
    mx, my = u.motor_xy
    enc_top = b1 + P.MOTOR["overall"] - 0.5  # the encoder board's top face
    for s in (1, -1):  # arms hooked over the encoder board's edges hold the motor down in its sleeve
        loc = Pos(mx + s * P.MOTOR["enc_w"] / 2, my, 0) * Rot(0, 0, -90 * s)
        arm, _ = hook_arm(u, f"motor clip {s:+d}", "bridge", loc, b1, enc_top, t=1.6, catch=0.8)
        bridge += arm
    u.add("bridge", bridge, PRINTED, "Bridge: gear retainer, motor sleeve and clips, deck and carrier standoffs")
    base_joints(u)
    u.joint("motor", "bridge")
    u.press[frozenset({"pinion", "motor"})] = "pressed onto the motor's D shaft"
    u.press[frozenset({"bolt_magnet", "bolt_flag"})] = "pressed (and glued) into the flag"

    # --- Hall switches: three SOT-23 adapters, chip-down, glued under a printed carrier. The
    # adapters' chip faces are where the product PCB's underside is, so the switches see the
    # magnet as they will on the product. Their header pins come up through the carrier.
    adapter = P.sot23_adapter_with_hall()
    carrier = rrect(0, -3.0, 2 * EDGE, 26.0, 3.0, c0, c1) - zcyl(13.0, c0 - 1, c1 + 1) \
        - peg_holes(carrier_posts, c0 - 1, c1 + 1)
    hall_pin = {}
    for name, (x, y, rot) in hall_places(L, p).items():
        loc = Pos(x, y, L.board_bottom) * Rot(0, 0, rot)
        for sx in (-3.81, 3.81):  # a slot for each 1x3 row of pins
            carrier -= loc * box(sx - 1.0, sx + 1.0, -3.6, 3.6, c0 - L.board_bottom - 1, c1 - L.board_bottom + 1)
        u.add(name, adapter.at(loc), BOUGHT,
              "SparkFun BOB-00717 SOT-23 adapter with a DRV5032FADBZR and 2x 1x3 header, worn chip-down")
        u.joint(name, "hall_carrier")
        hall_pin[name] = {h: adapter.pin(loc, h) for h in HALL_HOLE.values()}
    for s in (1, -1):
        cut, barb, window = edge_tab(u, f"carrier tab {s:+d}", "hall_carrier", s, EDGE, -15.0, -3.0, c0, c1, X_IN)
        carrier = carrier - cut + barb
        windows += window
    for s, y in cover_latches:  # room for the cover latch arms to flex in
        if y < 20:
            carrier -= _notch(s, y, 28.6, c0, c1)
    u.add("hall_carrier", carrier, PRINTED,
          "Hall carrier: holds the three adapters chip-down (glued in its slots), PETG")
    u.joint("hall_carrier", "bridge")

    # --- deck: replaces the main PCB; lift it out (four tabs) to change the cells ---
    deck = rrect(0, sum(DECK_Y) / 2, 2 * EDGE, DECK_Y[1] - DECK_Y[0], 3.0, *DECK_Z)
    deck -= box(-11.0, 11.0, DECK_Y[0] - 1, 36.0, DECK_Z[0] - 1, DECK_Z[1] + 1)  # round the motor
    for sx in (-1, 1):  # slots for the battery leads, over the near contacts' tabs
        deck -= box(min(sx * 14.0, sx * 29.0), max(sx * 14.0, sx * 29.0), 36.5, 42.0, DECK_Z[0] - 1, DECK_Z[1] + 1)
    deck -= peg_holes(deck_near + deck_far, DECK_Z[0] - 1, DECK_Z[1] + 1)
    for s in (1, -1):
        for root, tip in ((57.0, 45.0), (88.0, 100.0)):
            cut, barb, window = edge_tab(u, f"deck tab {s:+d} {tip:g}", "deck", s, EDGE, root, tip, *DECK_Z, X_IN)
            deck = deck - cut + barb
            windows += window
    for s, y in cover_latches:
        if y > 20:
            deck -= _notch(s, y, 28.8, *DECK_Z)
    for name in BOARDS:
        add, cut = _breadboard(u, name)
        deck = deck - cut + add
    deck &= rrect(0, sum(DECK_Y) / 2, 2 * EDGE, DECK_Y[1] - DECK_Y[0], 3.0, DECK_Z[0] - 1, DECK_Z[1] + 20)
    u.add("deck", deck, PRINTED, "Breadboard deck (in place of the main PCB), PETG")
    u.joint("deck", "chassis")
    u.joint("deck", "bridge")
    u.cut("chassis", windows)

    # --- the DevKitM-1 and the DRV8833 carrier, on their breadboards ---
    u.add("devkit", P.devkitm_1().at(DEVKIT_LOC), BOUGHT,
          "Espressif ESP32-C6-DevKitM-1 (the product's ESP32-C6-MINI-1 module), on its pin headers")
    u.joint("devkit", "bb_mcu")
    u.add("drv8833", P.pololu_2130().at(DRV_LOC), BOUGHT,
          "Pololu #2130 DRV8833 dual motor driver carrier, on 2x 1x8 pin headers")
    u.joint("drv8833", "bb_drv")
    pcb_top = BB_TOP + P.HEADER_H + P.DEVKITM_PCB_T

    # --- cover: the slab's shape, a big front window, a slot for the USB cables ---
    yc = BOARDS["bb_mcu"]
    cover = rrect(0, CY, W, H, R, 0, DEPTH) - rrect(0, CY, W - 2 * COVER_T, H - 2 * COVER_T, R - COVER_T, -1,
                                                    DEPTH - COVER_T)
    cover -= zcyl(13.0, DEPTH - 3, DEPTH + 1)
    cover -= rrect(0, 79.0, 56.0, 76.0, 4.0, DEPTH - 3, DEPTH + 1)
    cover -= box(-W / 2 - 1, -W / 2 + 3, yc - 12.0, yc + 12.0, pcb_top - 1.0, pcb_top + 6.0)
    cover -= cover_windows
    u.add("cover", cover, PRINTED, "Open cover with a front window and USB slot, PETG (clear works)")
    u.joint("cover", "chassis", COVER_GAP + 0.05)
    snap_knob(u, DEPTH + 0.5, DEPTH + 13.0)

    u.steps = assembly_steps(u)
    for i in range(4):
        for c in [n for n in u.items if n.startswith("contact_")]:
            u.press[frozenset({f"cell_{i}", c})] = "held against its contacts by the spring"
        u.press[frozenset({f"cell_{i}", "chassis"})] = "pushed against the rib by the spring"

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
