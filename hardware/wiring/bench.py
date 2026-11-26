"""Stage 1: the bench. The lock's circuit on one full-size breadboard, built from the parts
on hand (2026-11-11), with the dev kit's pin map so the same firmware runs on both.

| Block | Bench part | Dev kit / product part |
|---|---|---|
| MCU | ESP32-C6-DevKitC-1 | ESP32-C6-DevKitM-1 / ESP32-C6-MINI-1 |
| Motor driver | TB6612FNG breakout | DRV8833 |
| Motor | N20 gearmotor, no encoder | Pololu #5193 with encoder |
| Bolt and door switches | 3x A3144 (TO-92) | DRV5032FA |
| Power | ELEGOO breadboard power module (5 V and 3.3 V rails); the DevKit on its own USB | 4x AA |
| Breadboard | ELEGOO 830-point, with power rails | 2x 170-point minis |

Differences that matter to the firmware:
- TB6612FNG: AIN1/AIN2 on GPIO 18/19 as on the DRV8833, STBY on GPIO 20 (where nSLEEP was),
  and PWMA tied to 3.3 V, so PWM on AIN1/AIN2 drives it the same way. It has no fault output:
  GPIO 3 is unused.
- No encoder: GPIO 21 and 4 are unused; the bolt Hall switches and timeouts stand in.
- Motor at 5 V (the power module's 5 V rail) instead of 6 V: a little slower and weaker
  than in the dev kit, so time-outs tuned here are on the long side.
- A3144: needs 4.5-24 V, so it runs from the 5 V rail. Its output is open collector:
  turn on the GPIO's internal pull-up (to 3.3 V). It is unipolar: it switches only when a
  magnet's south pole faces its branded side. About 4 mA each, so it's for the bench only.
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

TB_CTL = ["PWMA", "AIN2", "AIN1", "STBY", "BIN1", "BIN2", "PWMB", "GND"]
TB_PWR = ["VM", "VCC", "GND", "AO1", "AO2", "BO2", "BO1", "GND2"]  # SparkFun ROB-14451 order
A3144 = {"VCC": "1", "GND": "2", "OUT": "3"}  # branded face towards you, legs down, left to right

BENCH = Stage(
    key="bench", title="Stage 1: bench breadboard",
    parts={
        "devkit": devkit_part("ESP32-C6-DevKitC-1", DEVKITC_J1, DEVKITC_J3),
        "tb6612": Part("tb6612", "TB6612FNG breakout", [f"C-{n}" for n in TB_CTL] + [f"P-{n}" for n in TB_PWR]),
        **{r: Part(r, f"A3144 Hall switch ({r.split('_')[1]})", list(A3144.values())) for r in HALL_PARTS.values()},
        "motor": Part("motor", "N20 gearmotor (no encoder)", ["M1", "M2"]),
        # Plugs into the four rails at one end of the board; each side's jumper picks its voltage.
        "psu": Part("psu", "ELEGOO breadboard power module (MB102)", ["L+", "L-", "R+", "R-"]),
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
        **{(b, sig): (r, A3144[sig]) for b, r in HALL_PARTS.items() for sig in A3144},
        # The motor supply is the power module's 5 V side (VBAT is merged into +5V below).
        ("BATTERY", "+"): ("psu", "R+"),
        ("BATTERY", "-"): ("psu", "R-"),
    },
    omit={("MCU", "GPIO3"): "TB6612FNG has no fault output", ("DRIVER", "nFAULT"): "TB6612FNG has no fault output",
          ("MCU", "GPIO21"): "N20 has no encoder", ("MCU", "GPIO4"): "N20 has no encoder",
          **{("MOTOR", p): "N20 has no encoder" for p in "1234"},
          ("MCU", "3V3"): "the DevKit runs from its own USB; its 3V3 pin on the power module's 3.3 V "
                          "rail would put two regulators on one net"},
    moves={(b, "VCC"): "+5V" for b in HALL_PARTS},  # A3144 needs 4.5 V or more
    extra={"+3V3": [("psu", "L+"), ("tb6612", "P-VCC"), ("tb6612", "C-PWMA")],
           "GND": [("psu", "L-"), ("tb6612", "P-GND")]},
    aliases={"VBAT": "+5V"},  # the motor runs off the 5 V rail
    ties=[(("tb6612", "C-GND"), ("tb6612", "P-GND"), ("tb6612", "P-GND2")),
          (("psu", "L-"), ("psu", "R-")),
          devkit_ground_tie(DEVKITC_J1, DEVKITC_J3)],
)
TITLES = NET_TITLES | {"+5V": "5 V (power module)", "+3V3": "3.3 V (power module)",
                       **{n: d.replace("DRV5032", "A3144") for n, d in NET_TITLES.items() if n.startswith("HALL_")}}

# --- layout on the 830-point breadboard ------------------------------------------------------
# Rows 1-63 run along the board; columns a-e and f-j either side of the centre gap. Two rails
# each side, in the same order across the board (+ - | strips | + -): R+ on the outside and
# R- beside column j; L+ beside column a and L- on the outside.
RAILS = {"L+": "+3V3", "L-": "GND", "R+": "+5V", "R-": "GND"}
# The power module plugs into all four rails at the row-63 end (the DevKit's USB overhangs the
# row-1 end). Its body covers the last rows: keep rows PSU_CLEAR-63 free of parts and wires.
PSU_ROW = 62  # where its pins are drawn; it only needs to be in the rails
PSU_CLEAR = 52
DEVKIT_ROW = 18  # DevKit pin k in row 18 - k: J3 in column b, J1 in column i, USB off the row-1 end
# (pin 16 is in row 2; the USB-C ports overhang the board's end), antenna towards row 20
# TB6612 pin k in row 21 + k: control row (PWMA..GND) in column h, at the top of the drawing; power
# row (VM..GND) in column c, at the bottom; 0.7" apart. That is the board turned with its header
# pin 1s (PWMA, VM) towards row 22: the control header then lies on the j side.
TB_ROW = 22
# Legs in column f, rows r..r+2. At least HALL_GAP empty rows between two switches: TO-92s
# side by side don't fit in fewer.
HALL_GAP = 4
HALL_ROWS = {"hall_locked": 34, "hall_unlocked": 41, "hall_door": 48}


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
        return Hole(HALL_ROWS[ref] + int(pin) - 1, col or "j")
    if ref == "psu":
        return Hole(PSU_ROW, pin)
    raise KeyError(ref)


def _seated(ref: str, pin: str) -> Hole:
    """Where the pin itself goes in."""
    if ref == "devkit":
        return pin_hole(ref, pin, "i" if pin.startswith("J1") else "b")
    if ref == "tb6612":
        return pin_hole(ref, pin, "h" if pin.startswith("C") else "c")
    if ref == "psu":
        return pin_hole(ref, pin)
    return pin_hole(ref, pin, "f")


ON_BOARD = ("devkit", "tb6612", *HALL_ROWS, "psu")


def wire_plan() -> list[Wire]:
    nets = BENCH.nets()

    def sig(net, col=""):  # (MCU end, other end) of a signal net
        a, b = nets[net]
        return pin_hole(*a), pin_hole(*b, col)

    w = [Wire("MOT_IN1", "yellow", *sig("MOT_IN1"), "GPIO 18 to TB6612 AIN1"),
         Wire("MOT_IN2", "green", *sig("MOT_IN2"), "GPIO 19 to TB6612 AIN2"),
         Wire("DRV_nSLEEP", "white", *sig("DRV_nSLEEP"), "GPIO 20 to TB6612 STBY"),
         Wire("GND", "black", pin_hole("devkit", "J3-1"), Hole(17, "L-"), "DevKit GND to the GND rail (common ground)"),
         Wire("+3V3", "red", pin_hole("tb6612", "C-PWMA"), Hole(22, "L+"), "TB6612 PWMA held high"),
         Wire("GND", "black", pin_hole("tb6612", "C-GND"), Hole(29, "R-"), "TB6612 GND (control side)"),
         Wire("+3V3", "red", pin_hole("tb6612", "P-VCC"), Hole(23, "L+"), "TB6612 VCC (logic 3.3 V)"),
         Wire("GND", "black", pin_hole("tb6612", "P-GND"), Hole(24, "L-"), "TB6612 GND (power side)"),
         Wire("+5V", "orange", pin_hole("tb6612", "P-VM"), Hole(21, "R+"), "TB6612 VM (motor supply) from the 5V rail"),
         Wire("MOT_M1", "red", pin_hole("tb6612", "P-AO1"), Term("motor", "M1"), "TB6612 AO1 to the motor"),
         Wire("MOT_M2", "black", pin_hole("tb6612", "P-AO2"), Term("motor", "M2"), "TB6612 AO2 to the motor")]
    for ref, r in HALL_ROWS.items():
        w.append(Wire("+5V", "orange", pin_hole(ref, "1"), Hole(r, "R+"), f"{ref} VCC to the 5V rail"))
        w.append(Wire("GND", "black", pin_hole(ref, "2"), Hole(r + 1, "R-"), f"{ref} GND to the GND rail"))
        net = next(n for n, (_, b, _) in PIN_MAP.items() if HALL_PARTS.get(b[0]) == ref)
        mcu = pin_hole(*nets[net][0])
        w.append(Wire(net, "blue", pin_hole(ref, "3", "g"), mcu, f"{ref} OUT to GPIO {PIN_MAP[net][0]}"))
    assert all(e.row < PSU_CLEAR for wr in w for e in (wr.a, wr.b) if isinstance(e, Hole))
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
        return {"motor": "motor"}[e.ref] + f" {e.pin}"
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
                elif ref == "psu":
                    continue
                else:
                    name = f"{ref} {next(s for s, p in A3144.items() if p == pin)}"
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
    ("TB6612FNG breakout", f"Control row (PWMA..GND) in column h, power row (VM..GND) in column c, rows {TB_ROW}-{TB_ROW + 7}",
     "PWMA and VM in row 22; check your board's silkscreen order"),
    *((f"A3144 {ref.split('_')[1]}", f"Legs in column f, rows {r}-{r + 2}", "Branded (flat) face towards the centre gap (column e), so VCC, GND, OUT run left to right "
      "as you face it")
      for ref, r in HALL_ROWS.items()),
    ("ELEGOO power module", f"Pins in the four rails at the row-63 end; keep rows {PSU_CLEAR}-63 clear",
     "Barrel jack off the board's end; jumper on the j side at 5 V, on the a side at 3.3 V"),
    ("N20 motor", "Off the board, on the bench", "Leads soldered to its tabs"),
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
    s.text(MX, 15.0, "Power: the ELEGOO power module feeds the rails (R+ 5 V: Hall switches and TB6612 VM; L+ 3.3 V: "
                     "TB6612 logic). The DevKit runs from its own USB; only its GND joins the board.", 1.4, fill="#555")

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
    outline(TB_ROW, TB_ROW + 7, "h", "c", "#c0392b", "TB6612FNG", "check the pin order on yours")
    (ax, ay), (bx, by) = xy(Hole(PSU_CLEAR, "R+")), xy(Hole(63, "L-"))
    s.add(f'<rect x="{ax - 1.2:.2f}" y="{ay - 1.6:.2f}" width="{bx - ax + 6.0:.2f}" height="{by - ay + 3.2:.2f}" rx="0.8" '
          'fill="#1d4fa8" fill-opacity="0.14" stroke="#333" stroke-width="0.3" stroke-dasharray="1,0.6"/>')
    s.text((ax + bx) / 2 + 2, (ay + by) / 2 - 2.0, "ELEGOO power", 1.5, "middle", fill="#111", weight="bold")
    s.text((ax + bx) / 2 + 2, (ay + by) / 2 + 0.2, "module", 1.5, "middle", fill="#111", weight="bold")
    s.text((ax + bx) / 2 + 2, (ay + by) / 2 + 2.6, "j side: 5 V", 1.1, "middle", fill="#333")
    s.text((ax + bx) / 2 + 2, (ay + by) / 2 + 4.4, "a side: 3.3 V", 1.1, "middle", fill="#333")
    s.text((ax + bx) / 2 + 2, (ay + by) / 2 + 6.2, "barrel jack off this end", 1.0, "middle", fill="#333")
    for ref, r in HALL_ROWS.items():
        x, y = xy(Hole(r + 1, "f"))
        s.text(x, y + 3.4, ref.replace("hall_", "A3144 "), 1.1, "middle", fill="#111", weight="bold")
    for ref in ON_BOARD:
        for pin in BENCH.parts[ref].pins:
            h = _seated(ref, pin)
            x, y = xy(h)
            if ref == "devkit":
                name = devkit_pin_name(pin, DEVKITC_J1, DEVKITC_J3)
            elif ref == "tb6612":
                name = pin.split("-", 1)[1].replace("GND2", "GND")
            elif ref == "psu":
                s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
                continue
            else:
                name = next(sg for sg, p in A3144.items() if p == pin)
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
        s.text(x + 1.4, y + 0.45, pin, 1.0, "start")

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
    for note in ("Before switching on: set the power module's jumpers (j side 5 V, a side 3.3 V) and check its + pins sit "
                 "in the red rails. Never wire the DevKit's 3V3 or 5V pin to a rail: it has its own USB supply.",
                 "The module feeds the rails from the row-63 end. If your rails are split halfway along, bridge each one "
                 "across the split, or the DevKit, TB6612 and switches get no power.",
                 "A3144: branded face towards you, legs 1 VCC, 2 GND, 3 OUT; needs a magnet's SOUTH pole on the branded "
                 "face. Firmware: enable the pull-up on GPIO 22, 23 and 2.",
                 "TB6612 pin order is SparkFun's (ROB-14451); check your board's silkscreen. The DevKit is the "
                 "DevKitC-1 (16 pins a side); a DevKitM-1 has the same GPIOs on other rows."):
        s.text(MX, y, note, 1.25, fill="#7b241c")
        y += 2.4
    s.add("</svg>")
    return "\n".join(s.out) + "\n"
