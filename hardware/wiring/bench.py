"""Stage 1: the bench. The lock's circuit on one full-size breadboard, built from the parts
on hand (2026-11-11), with the dev kit's pin map so the same firmware runs on both.

| Block | Bench part | Dev kit / product part |
|---|---|---|
| MCU | ESP32-C6-DevKitC-1 | ESP32-C6-DevKitM-1 / ESP32-C6-MINI-1 |
| Motor driver | TB6612FNG breakout | DRV8833 |
| Motor | N20 gearmotor, no encoder | Pololu #5193 with encoder |
| Bolt and door switches | 3x KY-003 Hall switch module (A3144), each with a 20k to GND | DRV5032FA |
| Supplies | SunFounder BreadVolt on the rails: 3.3 V (L side), 5 V (R side, motor too) | 4x AA, DevKit 3V3 |
| Breadboard | ELEGOO 830-point, with power rails | 2x 170-point minis |

Differences that matter to the firmware:
- TB6612FNG: AIN1/AIN2 on GPIO 18/19 as on the DRV8833, STBY on GPIO 20 (where nSLEEP was),
  and PWMA tied to 3.3 V, so PWM on AIN1/AIN2 drives it the same way. It has no fault output:
  GPIO 3 is unused.
- No encoder: GPIO 21 and 4 are unused; the bolt Hall switches and timeouts stand in.
- KY-003 module (an A3144 with an LED and a pull-up on a small board, pins - + S): the A3144
  needs 4.5-24 V, so it runs from the 5 V rail. The board pulls S up to its own supply (5 V,
  through about 10k), too much for a GPIO, so a 20k from S to GND makes it about 3.3 V. S goes
  low (and the LED lights) when a magnet's south pole faces the chip's branded side: the A3144
  is unipolar. About 4 mA each, so it's for the bench only.
- Power: the BreadVolt (one 14500 cell, jumpers per side for off / 3.3 V / 5 V) feeds both rails.
  The DevKit runs from its own USB and shares only ground with them: its 3V3 and 5V pins stay
  unwired, so no jumper setting can back-feed it. The motor runs at 5 V, a little slower than 6 V.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from html import escape

from .breadboard import INK, Svg
from .netlist import (HALL_PARTS, NET_TITLES, PIN_MAP, Part, Stage, devkit_blocks, devkit_ground_tie,
                      devkit_part, devkit_pin_name, merge)

# ESP32-C6-DevKitC-1 headers, pin 1 at the antenna end (Espressif's user guide, "Header Block";
# rows 0.9" apart, 2.54 mm pitch, per its v1.2 dimension drawing). USB-C ports beyond pin 16.
DEVKITC_J1 = ["3V3", "RST", "4", "5", "6", "7", "0", "1", "8", "10", "11", "2", "3", "5V", "G", "NC"]
DEVKITC_J3 = ["G", "TX", "RX", "15", "23", "22", "21", "20", "19", "18", "9", "G", "13", "12", "G", "NC"]

# SparkFun ROB-14451 (v11 board file, SparkFun_Motor_Driver-TB6612FNG_v11.brd): a 0.8" square board with
# two 1x8 headers 0.6" apart. Seen chips-up, pin 1 of each list sits at the same end: PWMA is directly
# opposite VM, and the last GND opposite GND.
TB_CTL = ["PWMA", "AIN2", "AIN1", "STBY", "BIN1", "BIN2", "PWMB", "GND"]
TB_PWR = ["VM", "VCC", "GND", "AO1", "AO2", "BO2", "BO1", "GND2"]
KY003 = {"GND": "-", "VCC": "+", "OUT": "S"}  # module pins, left to right as labelled: - + S
KY003_ORDER = ["-", "+", "S"]
R_DIV = {h: "rdiv_" + h.split("_", 1)[1] for h in ("hall_locked", "hall_unlocked", "hall_door")}  # S to GND
BV_PINS = ("L+", "L-", "R+", "R-")  # SunFounder BreadVolt: one + and one - pin into each side's rails

BENCH = Stage(
    key="bench", title="Stage 1: bench breadboard",
    parts={
        "devkit": devkit_part("ESP32-C6-DevKitC-1", DEVKITC_J1, DEVKITC_J3),
        "tb6612": Part("tb6612", "TB6612FNG breakout", [f"C-{n}" for n in TB_CTL] + [f"P-{n}" for n in TB_PWR]),
        **{r: Part(r, f"KY-003 Hall switch module ({r.split('_')[1]})", KY003_ORDER) for r in HALL_PARTS.values()},
        **{r: Part(r, f"20k, {h} S to GND", ["1", "2"]) for h, r in R_DIV.items()},
        "motor": Part("motor", "N20 gearmotor (no encoder)", ["M1", "M2"]),
        "breadvolt": Part("breadvolt", "SunFounder BreadVolt (L side 3.3 V, R side 5 V)", list(BV_PINS)),
    },
    blocks={
        **devkit_blocks(DEVKITC_J1, DEVKITC_J3),
        ("DRIVER", "VM"): ("tb6612", "P-VM"),
        ("DRIVER", "GND"): ("tb6612", "C-GND"),
        ("DRIVER", "AIN1"): ("tb6612", "C-AIN1"),
        ("DRIVER", "AIN2"): ("tb6612", "C-AIN2"),
        ("DRIVER", "nSLEEP"): ("tb6612", "C-STBY"),
        ("DRIVER", "AOUT1"): ("tb6612", "P-AO1"),
        ("DRIVER", "AOUT2"): ("tb6612", "P-AO2"),
        ("MOTOR", "6"): ("motor", "M1"),
        ("MOTOR", "5"): ("motor", "M2"),
        **{(b, sig): (r, KY003[sig]) for b, r in HALL_PARTS.items() for sig in KY003},
        # The battery as the driver sees it: the BreadVolt's 5 V side, shared with the Hall modules.
        ("BATTERY", "+"): ("breadvolt", "R+"),
        ("BATTERY", "-"): ("breadvolt", "R-"),
    },
    omit={("MCU", "GPIO3"): "TB6612FNG has no fault output", ("DRIVER", "nFAULT"): "TB6612FNG has no fault output",
          ("MCU", "GPIO21"): "N20 has no encoder", ("MCU", "GPIO4"): "N20 has no encoder",
          **{("MOTOR", p): "N20 has no encoder" for p in "1234"},
          ("MCU", "3V3"): "the DevKit runs from its USB; the BreadVolt powers the rails"},
    moves={(b, "VCC"): "+5V" for b in HALL_PARTS},  # the A3144 needs 4.5 V or more
    aliases={"VBAT": "+5V"},  # the motor runs from the same 5 V rail
    extra={"+3V3": [("breadvolt", "L+"), ("tb6612", "P-VCC"), ("tb6612", "C-PWMA")],
           "GND": [("tb6612", "P-GND"), ("breadvolt", "L-"), *((r, "2") for r in R_DIV.values())],
           **{b: [(R_DIV[h], "1")] for b, h in HALL_PARTS.items()}},
    ties=[(("tb6612", "C-GND"), ("tb6612", "P-GND"), ("tb6612", "P-GND2")),
          (("breadvolt", "L-"), ("breadvolt", "R-")),  # common inside the module (checked with a meter)
          devkit_ground_tie(DEVKITC_J1, DEVKITC_J3)],
)
TITLES = NET_TITLES | {"+5V": "5 V (BreadVolt R side)", "+3V3": "3.3 V (BreadVolt L side)",
                       **{n: d.replace("DRV5032 OUT", "KY-003 S") for n, d in NET_TITLES.items() if n.startswith("HALL_")}}

# --- layout on the 830-point breadboard ------------------------------------------------------
# Rows 1-63 run along the board; columns a-e and f-j either side of the centre gap. Two rails
# each side: L+ and L- beside column a, R+ and R- beside column j. Top to bottom the board reads
# R+, R-, j..f, e..a, L+, L-: "+ -, + -", as on the ELEGOO 830-point board.
RAILS = {"L+": "+3V3", "L-": "GND", "R+": "+5V", "R-": "GND"}
DEVKIT_ROW = 18  # DevKit pin k in row 18 - k: J3 in column b, J1 in column i, USB off the row-1 end
# (pin 16 is in row 2; the USB-C ports overhang the board's end), antenna towards row 20
TB_ROW = 22  # TB6612 pin k in row 21 + k, chips up: control row in column h (PWMA top left, at 22h), power
# row in column d (VM at 22d), 0.6" apart. The board's edges cover columns i and c.
HALL_ROWS = {"hall_locked": 34, "hall_unlocked": 39, "hall_door": 44}  # pins - + S in column f; 20k from S (h) to R-
BV_ROW = 61  # the BreadVolt sits over the row-63 end (the DevKit's USB hangs off the row-1 end)


@dataclass(frozen=True)
class Hole:
    row: int
    col: str  # a-j, or a rail: L+ L- R+ R-

    def strip(self):
        if self.col in RAILS:  # rails run the board's length (bridge them if yours are split)
            return ("rail", RAILS[self.col] if RAILS[self.col] != "GND" else "GND" + self.col[0])
        return ("strip", self.row, "a-e" if self.col in "abcde" else "f-j")


@dataclass(frozen=True)
class Term:
    ref: str
    pin: str


@dataclass(frozen=True)
class Wire:
    net: str
    colour: str
    a: Hole | Term
    b: Hole | Term
    desc: str


def pin_hole(ref: str, pin: str, col: str = "") -> Hole:
    """The hole a part's pin sits in, or another hole (col) of its strip."""
    if ref == "devkit":
        hdr, k = pin.split("-")
        return Hole(DEVKIT_ROW - int(k), col or ("j" if hdr == "J1" else "a"))
    if ref == "tb6612":
        side, name = pin.split("-", 1)
        names = TB_CTL if side == "C" else TB_PWR
        return Hole(TB_ROW + names.index(name), col or ("j" if side == "C" else "a"))
    if ref in HALL_ROWS:
        return Hole(HALL_ROWS[ref] + KY003_ORDER.index(pin), col or "j")
    if ref in R_DIV.values():
        row = HALL_ROWS[next(h for h, r in R_DIV.items() if r == ref)] + KY003_ORDER.index("S")
        return Hole(row, "h" if pin == "1" else "R-")
    if ref == "breadvolt":
        return Hole(BV_ROW, pin)
    raise KeyError(ref)


def _seated(ref: str, pin: str) -> Hole:
    """Where the pin itself goes in."""
    if ref == "devkit":
        return pin_hole(ref, pin, "i" if pin.startswith("J1") else "b")
    if ref == "tb6612":
        return pin_hole(ref, pin, "h" if pin.startswith("C") else "d")
    if ref == "breadvolt" or ref in R_DIV.values():
        return pin_hole(ref, pin)
    return pin_hole(ref, pin, "f")


ON_BOARD = ("devkit", "tb6612", *HALL_ROWS, *R_DIV.values(), "breadvolt")


def wire_plan() -> list[Wire]:
    nets = BENCH.nets()

    def sig(net, col=""):  # (MCU end, other end) of a signal net
        a, b = nets[net]
        return pin_hole(*a), pin_hole(*b, col)

    w = [Wire("MOT_IN1", "yellow", *sig("MOT_IN1"), "GPIO 18 to TB6612 AIN1"),
         Wire("MOT_IN2", "green", *sig("MOT_IN2"), "GPIO 19 to TB6612 AIN2"),
         Wire("DRV_nSLEEP", "white", *sig("DRV_nSLEEP"), "GPIO 20 to TB6612 STBY"),
         Wire("GND", "black", pin_hole("devkit", "J3-1"), Hole(17, "L-"), "DevKit GND to the GND rail"),
         Wire("+3V3", "red", pin_hole("tb6612", "C-PWMA"), Hole(22, "L+"), "TB6612 PWMA held high"),
         Wire("GND", "black", pin_hole("tb6612", "C-GND"), Hole(29, "R-"), "TB6612 GND (control side)"),
         Wire("+3V3", "red", pin_hole("tb6612", "P-VCC"), Hole(23, "L+"), "TB6612 VCC (logic 3.3 V)"),
         Wire("GND", "black", pin_hole("tb6612", "P-GND"), Hole(24, "L-"), "TB6612 GND (power side)"),
         Wire("+5V", "orange", pin_hole("tb6612", "P-VM"), Hole(22, "R+"), "TB6612 VM (motor supply) to the 5V rail"),
         Wire("MOT_M1", "red", pin_hole("tb6612", "P-AO1"), Term("motor", "M1"), "TB6612 AO1 to the motor"),
         Wire("MOT_M2", "black", pin_hole("tb6612", "P-AO2"), Term("motor", "M2"), "TB6612 AO2 to the motor")]
    for ref, r in HALL_ROWS.items():
        w.append(Wire("GND", "black", pin_hole(ref, "-"), Hole(r, "R-"), f"{ref} - to the GND rail"))
        w.append(Wire("+5V", "orange", pin_hole(ref, "+"), Hole(r + 1, "R+"), f"{ref} + to the 5V rail"))
        net = next(n for n, (_, b, _) in PIN_MAP.items() if HALL_PARTS.get(b[0]) == ref)
        mcu = pin_hole(*nets[net][0])
        w.append(Wire(net, "blue", pin_hole(ref, "S", "g"), mcu, f"{ref} S to GPIO {PIN_MAP[net][0]}"))
    return w


def plan_connectivity(plan: list[Wire] | None = None) -> set[frozenset]:
    """What the wires, strips, rails and the boards' own copper join."""
    groups = [{_seated(ref, pin).strip(), (ref, pin)} for ref in ON_BOARD for pin in BENCH.parts[ref].pins]
    for wr in plan or wire_plan():
        groups.append({e.strip() if isinstance(e, Hole) else (e.ref, e.pin) for e in (wr.a, wr.b)})
    groups += [set(t) for t in BENCH.ties]
    nets = {frozenset(p for p in g if len(p) == 2 and p[0] not in ("rail",)) for g in merge(groups)}
    return {n for n in nets if len(n) > 1}


def end_label(e: Hole | Term) -> str:
    if isinstance(e, Term):
        return f"{e.ref} {e.pin}"
    if e.col in RAILS:
        return f"{RAILS[e.col].lstrip('+')} rail ({e.col}, row {e.row})"
    for ref in ON_BOARD:
        for pin in BENCH.parts[ref].pins:
            if _seated(ref, pin).strip() == e.strip():
                if ref == "devkit":
                    n = devkit_pin_name(pin, DEVKITC_J1, DEVKITC_J3)
                    name = f"DevKit {'GPIO ' + n if n.isdigit() else n}"
                elif ref == "tb6612":
                    name = f"TB6612 {pin.split('-', 1)[1].replace('GND2', 'GND')}"
                elif ref == "breadvolt" or ref in R_DIV.values():
                    continue
                else:
                    name = f"{ref} {pin}"
                return f"{name} ({e.row}{e.col})"
    return f"{e.row}{e.col}"


# --- drawing ------------------------------------------------------------------------------
P = 2.54
MX, MY = 12.0, 22.0  # where row 0 / the R+ rail sit on the sheet
COL_Y = {"R+": 0.0, "R-": P, **{c: 3 * P + i * P for i, c in enumerate("jihgf")},
         **{c: 10 * P + i * P for i, c in enumerate("edcba")}, "L+": 16 * P, "L-": 17 * P}


def xy(e: Hole | Term) -> tuple[float, float]:
    if isinstance(e, Term):
        return TERMS[(e.ref, e.pin)]
    return round(MX + e.row * P, 3), round(MY + COL_Y[e.col], 3)


_BY = MY + 17 * P + 18.0  # off-board parts, under the breadboard
TERMS = {("motor", "M1"): (MX + 60.0, _BY), ("motor", "M2"): (MX + 66.0, _BY)}


PLACEMENT = [  # (part, where it goes, how it faces): the layout drawing's table
    ("ESP32-C6-DevKitC-1", "J1 pins in column i, J3 pins in column b; pin 1 (3V3 / G) in row 17, pin 16 (NC) "
     "in row 2", "Antenna towards row 20, USB-C ports hanging off the row-1 end"),
    ("TB6612FNG breakout", f"Control row (PWMA..GND) in column h, power row (VM..GND) in column d, rows {TB_ROW}-{TB_ROW + 7}",
     "Chips up. PWMA top left (22h), VM opposite it (22d)"),
    *((f"KY-003 {ref.split('_')[1]}", f"Pins in column f, rows {r}-{r + 2}", "Labels facing you (towards column e), so "
       "-, +, S run left to right")
      for ref, r in HALL_ROWS.items()),
    ("3x 20k resistor", "From each module's S row (column h) to the top - rail: rows "
     + ", ".join(str(r + 2) for r in HALL_ROWS.values()), "Either way round"),
    ("SunFounder BreadVolt", f"Over the row-63 end, its four pins in the four rails (about row {BV_ROW})",
     "Minus pins in the blue (-) rails. Jumpers: L side (over L+/L-) on 3V3, R side (over R+/R-) on 5V"),
    ("N20 motor", "Off the board, on the bench", "Red and black leads soldered on, into the wired holes"),
]


def bench_svg(wires: bool = True) -> str:
    """The bench drawing: with wires (the wiring diagram) or without (where the parts go)."""
    s = Svg()
    plan = wire_plan() if wires else []
    W = MX * 2 + 64 * P
    table_y = _BY + 22.0
    H = table_y + 8 + (3.1 * len(plan) + 30 if wires else 3.1 * len(PLACEMENT) + 4)
    s.add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.1f} {H:.1f}" width="{W * 10:.0f}" '
          f'height="{H * 10:.0f}" font-family="Helvetica, Arial, sans-serif">')
    s.add(f'<rect width="{W:.1f}" height="{H:.1f}" fill="#fbfaf7"/>')
    s.text(MX, 8.0, f"lochness bench {'wiring' if wires else 'layout'} (stage 1): parts on hand, one 830-point "
                    "breadboard", 3.0, weight="bold", fill="#111")
    s.text(MX, 12.5, "Generated by build_wiring.py from wiring/bench.py. Same GPIO map as the dev kit, so the same "
                     "firmware runs on both. Hole = row (1-63) + column (a-j) or rail.", 1.4, fill="#555")
    s.text(MX, 15.0, "Power: the BreadVolt feeds the rails, L side 3.3 V and R side 5 V (Hall modules, motor). USB "
                     "powers the DevKit only: its 3V3 and 5V pins stay unwired. All grounds common.", 1.4, fill="#555")

    # board, strips, holes
    x0, x1 = MX - 1.5, MX + 64 * P - 1.0
    s.add(f'<rect x="{x0}" y="{MY - 2.5}" width="{x1 - x0:.1f}" height="{17 * P + 5:.1f}" rx="1.2" fill="#fff" '
          'stroke="#c9c5bd" stroke-width="0.3"/>')
    for rail, net in RAILS.items():
        y = MY + COL_Y[rail]
        colour = INK["black"] if net == "GND" else INK["red"]
        s.add(f'<line x1="{MX + P - 1}" y1="{y}" x2="{MX + 63 * P + 1}" y2="{y}" stroke="{colour}" '
              'stroke-width="1.6" stroke-opacity="0.13" stroke-linecap="round"/>')
        s.text(MX - 2.2, y + 0.5, f"{rail} {net}", 1.1, "end", fill=colour, weight="bold")
    for row in range(1, 64):
        for cols in ("jihgf", "edcba"):
            (ax, ay), (bx, by) = xy(Hole(row, cols[0])), xy(Hole(row, cols[-1]))
            s.add(f'<line x1="{ax}" y1="{ay}" x2="{bx}" y2="{by}" stroke="#e3e0d8" stroke-width="1.5" stroke-linecap="round"/>')
        for col in COL_Y:
            x, y = xy(Hole(row, col))
            s.add(f'<rect x="{x - 0.45:.2f}" y="{y - 0.45:.2f}" width="0.9" height="0.9" fill="#8d8a83"/>')
        if row % 5 == 0 or row == 1:
            x, y = xy(Hole(row, "a"))
            s.text(x, y + 6.6, str(row), 1.1, "middle", fill="#999")
    for col in "abcdefghij":
        x, y = xy(Hole(1, col))
        s.text(x - 2.4, y + 0.4, col, 1.1, "middle", fill="#999")

    # parts on the board
    def outline(r0, r1, c0, c1, fill, label, sub=""):
        (ax, ay), (bx, by) = xy(Hole(r0, c0)), xy(Hole(r1, c1))
        s.add(f'<rect x="{min(ax, bx) - 1.6:.2f}" y="{min(ay, by) - 1.6:.2f}" width="{abs(bx - ax) + 3.2:.2f}" '
              f'height="{abs(by - ay) + 3.2:.2f}" rx="0.6" fill="{fill}" fill-opacity="0.16" stroke="#333" stroke-width="0.3"/>')
        s.text((ax + bx) / 2, (ay + by) / 2 - 0.5, label, 1.5, "middle", fill="#111", weight="bold")
        if sub:
            s.text((ax + bx) / 2, (ay + by) / 2 + 1.8, sub, 1.1, "middle", fill="#333")

    (ax, ay), (bx, by) = xy(Hole(17, "i")), xy(Hole(17, "b"))  # module antenna past pin 1, 18 mm wide
    s.add(f'<rect x="{ax + 1.6:.2f}" y="{(ay + by) / 2 - 9:.2f}" width="6.35" height="18" fill="#c9a227" '
          'fill-opacity="0.3" stroke="#9c7d16" stroke-width="0.2"/>')
    outline(-2, 17, "i", "b", "#2b2b2b", "ESP32-C6-DevKitC-1", "USB-C ports off the board's end")
    outline(TB_ROW, TB_ROW + 7, "i", "c", "#c0392b", "TB6612FNG", "chips up, PWMA top left")
    for ref, r in HALL_ROWS.items():
        x, y = xy(Hole(r + 1, "f"))
        s.text(x, y + 3.4, ref.replace("hall_", "KY-003 "), 1.1, "middle", fill="#111", weight="bold")
    bv0, bv1 = xy(Hole(BV_ROW - 8, "R+")), xy(Hole(63, "L-"))  # outermost rails, top and bottom
    s.add(f'<rect x="{bv0[0]:.2f}" y="{bv0[1] - 2.0:.2f}" width="{bv1[0] - bv0[0] + 3.5:.2f}" '
          f'height="{bv1[1] - bv0[1] + 4.0:.2f}" rx="0.8" fill="#6c3483" fill-opacity="0.14" stroke="#333" stroke-width="0.3"/>')
    for k, t in enumerate(("SunFounder BreadVolt", "L side jumper: 3V3", "R side jumper: 5V", "minus pins in blue rails")):
        s.text((bv0[0] + bv1[0]) / 2 - 2.0, (bv0[1] + bv1[1]) / 2 - 3 + k * 2.3, t, 1.5 if not k else 1.1, "middle",
               fill="#111", weight="bold" if k < 3 else "normal")
    for ref in ON_BOARD:
        for pin in BENCH.parts[ref].pins:
            h = _seated(ref, pin)
            x, y = xy(h)
            if ref == "devkit":
                name = devkit_pin_name(pin, DEVKITC_J1, DEVKITC_J3)
            elif ref == "tb6612":
                name = pin.split("-", 1)[1].replace("GND2", "GND")
            elif ref == "breadvolt":
                s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
                s.text(x + 1.3, y + 0.45, f"BV {pin}", 0.95, fill="#111")
                continue
            elif ref in R_DIV.values():
                if pin == "1":  # draw the resistor once, from S (h) to the rail
                    x2, y2 = xy(_seated(ref, "2"))
                    s.add(f'<line x1="{x}" y1="{y}" x2="{x2}" y2="{y2}" stroke="#777" stroke-width="0.3"/>')
                    s.add(f'<rect x="{x - 0.7:.2f}" y="{(y + y2) / 2 - 2.0:.2f}" width="1.4" height="4.0" rx="0.5" '
                          'fill="#d9c08c" stroke="#7a6418" stroke-width="0.15"/>')
                    s.text(x + 1.0, (y + y2) / 2 + 0.4, "20k", 0.95, fill="#111")
                for xx, yy in ((x, y),):
                    s.add(f'<circle cx="{xx}" cy="{yy}" r="0.45" fill="#7a6418"/>')
                continue
            else:
                name = pin
            s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
            inward = h.col in "abcde" if ref != "devkit" else h.col == "b"
            s.text(x + 0.35, y + (1.2 if inward else -1.2), name, 0.95, "start" if not inward else "end", rot=-90, fill="#111")

    # off-board parts
    def box(x0_, y0_, x1_, y1_, fill, label):
        s.add(f'<rect x="{x0_}" y="{y0_}" width="{x1_ - x0_}" height="{y1_ - y0_}" rx="0.8" fill="{fill}" '
              'fill-opacity="0.18" stroke="#333" stroke-width="0.3"/>')
        s.text((x0_ + x1_) / 2, y1_ + 2.6, label, 1.3, "middle", fill="#111", weight="bold")

    box(MX + 56, _BY - 3, MX + 70, _BY + 3, "#999", "N20 motor")
    for (ref, pin), (x, y) in TERMS.items():
        s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
        s.text(x + 1.4, y + 0.45, pin, 1.0)

    # wires
    for k, wr in enumerate(plan):
        (ax, ay), (bx, by) = xy(wr.a), xy(wr.b)
        dx, dy = bx - ax, by - ay
        d = math.hypot(dx, dy) or 1.0
        bow = (2.0 + 1.3 * (k % 5)) * (1 if k % 2 else -1) * min(1.0, d / 20.0)
        cx, cy = (ax + bx) / 2 - dy / d * bow, (ay + by) / 2 + dx / d * bow
        path = f"M{ax:.2f},{ay:.2f} Q{cx:.2f},{cy:.2f} {bx:.2f},{by:.2f}"
        edge = "#555" if wr.colour in ("white", "yellow") else "#fff"
        s.add(f'<path d="{path}" fill="none" stroke="{edge}" stroke-width="0.95" stroke-linecap="round" opacity="0.9"/>')
        s.add(f'<path d="{path}" fill="none" stroke="{INK[wr.colour]}" stroke-width="0.6" stroke-linecap="round">'
              f'<title>{escape(wr.desc)}</title></path>')
        for x, y in ((ax, ay), (bx, by)):
            s.add(f'<circle cx="{x}" cy="{y}" r="0.5" fill="{INK[wr.colour]}" stroke="{edge}" stroke-width="0.15"/>')

    if not wires:  # placement table, then done
        y = table_y
        for head, xo in (("Part", 0), ("Where", 38), ("Orientation", 128)):
            s.text(MX + xo, y, head, 1.4, weight="bold")
        for k, (part, where, how) in enumerate(PLACEMENT, 1):
            y += 3.1
            if k % 2:
                s.add(f'<rect x="{MX - 1}" y="{y - 2.2:.2f}" width="{64 * P:.1f}" height="3.1" fill="#f1eee7"/>')
            for xo, t in ((0, part), (38, where), (128, how)):
                s.text(MX + xo, y, t, 1.15)
        s.add("</svg>")
        return "\n".join(s.out) + "\n"

    # table
    y = table_y
    for head, xo in (("#", 0), ("Signal", 4), ("From", 52), ("To", 100)):
        s.text(MX + xo, y, head, 1.4, weight="bold")
    for k, wr in enumerate(plan, 1):
        y += 3.1
        if k % 2:
            s.add(f'<rect x="{MX - 1}" y="{y - 2.2:.2f}" width="{64 * P:.1f}" height="3.1" fill="#f1eee7"/>')
        s.add(f'<line x1="{MX + 1.5}" y1="{y - 0.5:.2f}" x2="{MX + 3.2}" y2="{y - 0.5:.2f}" stroke="{INK[wr.colour]}" stroke-width="1.2"/>')
        s.text(MX + 0.4, y, str(k), 1.2, "end")
        s.text(MX + 4, y, f"{TITLES[wr.net]}: {wr.desc}", 1.2)
        s.text(MX + 52, y, end_label(wr.a), 1.2)
        s.text(MX + 100, y, end_label(wr.b), 1.2)
    y += 6
    for note in ("BreadVolt jumpers BEFORE switching on: L side 3V3, R side 5V; check with a meter (L+ 3.2-3.4 V, "
                 "R+ 4.75-5.25 V). Never wire the DevKit's 3V3 or 5V pin to a rail.",
                 "Rails top to bottom: + -, then + - (as on the ELEGOO board); + is the red stripe, - the blue. If the rails are "
                 "split halfway along, bridge each one across the split (the BreadVolt feeds only the row-63 half).",
                 "KY-003 modules: pins - + S as labelled; a 20k from each S to GND keeps S at ~3.3 V (the board pulls it up to 5 V). "
                 "Its LED lights when a magnet's SOUTH pole faces the A3144's branded face.",
                 "TB6612: SparkFun ROB-14451 pinout, headers 0.6\" apart, chips up with PWMA top left (22h) and VM opposite (22d). The DevKit is the "
                 "DevKitC-1 (16 pins a side); a DevKitM-1 has the same GPIOs on other rows."):
        s.text(MX, y, note, 1.25, fill="#7b241c")
        y += 2.4
    s.add("</svg>")
    return "\n".join(s.out) + "\n"
