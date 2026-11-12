"""Stage 1: the bench. The lock's circuit on one full-size breadboard, built from the parts
on hand (2026-11-11), with the dev kit's pin map so the same firmware runs on both.

| Block | Bench part | Dev kit / product part |
|---|---|---|
| MCU | Espressif ESP32-C6 DevKit (drawn as the DevKitM-1) | the same / ESP32-C6-MINI-1 |
| Motor driver | TB6612FNG breakout | DRV8833 |
| Motor | N20 gearmotor, no encoder | Pololu #5193 with encoder |
| Bolt and door switches | 3x A3144 (TO-92) | DRV5032FA |
| Motor supply | 2x 18650 in series -> MP1584EN buck set to 6 V | 4x AA |
| Breadboard | ELEGOO 830-point, with power rails | 2x 170-point minis |

Differences that matter to the firmware:
- TB6612FNG: AIN1/AIN2 on GPIO 18/19 as on the DRV8833, STBY on GPIO 20 (where nSLEEP was),
  and PWMA tied to 3.3 V, so PWM on AIN1/AIN2 drives it the same way. It has no fault output:
  GPIO 3 is unused.
- No encoder: GPIO 21 and 4 are unused; the bolt Hall switches and timeouts stand in.
- A3144: needs 4.5-24 V, so it runs from the DevKit's 5 V pin. Its output is open collector:
  turn on the GPIO's internal pull-up (to 3.3 V). It is unipolar: it switches only when a
  magnet's south pole faces its branded side. About 4 mA each, so it's for the bench only.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from html import escape

from concepts.parts import DEVKITM_J1, DEVKITM_J3
from .breadboard import INK, Svg
from .netlist import DEVKIT, HALL_PARTS, NET_TITLES, PIN_MAP, Part, Stage, merge

TB_CTL = ["PWMA", "AIN2", "AIN1", "STBY", "BIN1", "BIN2", "PWMB", "GND"]
TB_PWR = ["VM", "VCC", "GND", "AO1", "AO2", "BO2", "BO1", "GND2"]  # SparkFun ROB-14451 order
A3144 = {"VCC": "1", "GND": "2", "OUT": "3"}  # branded face towards you, legs down, left to right

BENCH = Stage(
    key="bench", title="Stage 1: bench breadboard",
    parts={
        "devkit": DEVKIT.parts["devkit"],
        "tb6612": Part("tb6612", "TB6612FNG breakout", [f"C-{n}" for n in TB_CTL] + [f"P-{n}" for n in TB_PWR]),
        **{r: Part(r, f"A3144 Hall switch ({r.split('_')[1]})", list(A3144.values())) for r in HALL_PARTS.values()},
        "motor": Part("motor", "N20 gearmotor (no encoder)", ["M1", "M2"]),
        "buck": Part("buck", "MP1584EN buck module, set to 6 V", ["IN+", "IN-", "OUT+", "OUT-"]),
        "battery": Part("battery", "2x 18650 in series (6.0-8.4 V)", ["+", "-"]),
    },
    blocks={
        **{k: v for k, v in DEVKIT.blocks.items() if k[0] == "MCU"},
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
        # The battery as the driver sees it: after the buck, 6 V.
        ("BATTERY", "+"): ("buck", "OUT+"),
        ("BATTERY", "-"): ("battery", "-"),
    },
    omit={("MCU", "GPIO3"): "TB6612FNG has no fault output", ("DRIVER", "nFAULT"): "TB6612FNG has no fault output",
          ("MCU", "GPIO21"): "N20 has no encoder", ("MCU", "GPIO4"): "N20 has no encoder",
          **{("MOTOR", p): "N20 has no encoder" for p in "1234"}},
    moves={(b, "VCC"): "+5V" for b in HALL_PARTS},  # A3144 needs 4.5 V or more
    extra={"+5V": [("devkit", "J1-14")],
           "+3V3": [("tb6612", "P-VCC"), ("tb6612", "C-PWMA")],
           "GND": [("tb6612", "P-GND"), ("buck", "IN-"), ("buck", "OUT-")],
           "VBAT_2S": [("battery", "+"), ("buck", "IN+")]},
    ties=[(("tb6612", "C-GND"), ("tb6612", "P-GND"), ("tb6612", "P-GND2")),
          (("buck", "IN-"), ("buck", "OUT-")),
          DEVKIT.ties[1]],
)
TITLES = NET_TITLES | {"+5V": "5 V (DevKit 5V pin, from USB)", "VBAT_2S": "2x 18650 + (6.0-8.4 V)",
                       "VBAT": "Motor supply (MP1584 out, 6 V)",
                       **{n: d.replace("DRV5032", "A3144") for n, d in NET_TITLES.items() if n.startswith("HALL_")}}

# --- layout on the 830-point breadboard ------------------------------------------------------
# Rows 1-63 run along the board; columns a-e and f-j either side of the centre gap. Two rails
# each side: L+ and L- beside column a, R+ and R- beside column j.
RAILS = {"L+": "+3V3", "L-": "GND", "R+": "+5V", "R-": "GND"}
DEVKIT_ROW = 18  # DevKit pin k in row 18 - k: J3 in column b, J1 in column i, USB off the row-1 end
TB_ROW = 22  # TB6612 pin k in row 21 + k: control row in column c, power row in column h (0.7")
HALL_ROWS = {"hall_locked": 34, "hall_unlocked": 39, "hall_door": 44}  # legs in column f


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
        return Hole(TB_ROW + names.index(name), col or ("a" if side == "C" else "j"))
    if ref in HALL_ROWS:
        return Hole(HALL_ROWS[ref] + int(pin) - 1, col or "j")
    raise KeyError(ref)


def _seated(ref: str, pin: str) -> Hole:
    """Where the pin itself goes in."""
    if ref == "devkit":
        return pin_hole(ref, pin, "i" if pin.startswith("J1") else "b")
    if ref == "tb6612":
        return pin_hole(ref, pin, "c" if pin.startswith("C") else "h")
    return pin_hole(ref, pin, "f")


ON_BOARD = ("devkit", "tb6612", *HALL_ROWS)


def wire_plan() -> list[Wire]:
    nets = BENCH.nets()

    def sig(net, col=""):  # (MCU end, other end) of a signal net
        a, b = nets[net]
        return pin_hole(*a), pin_hole(*b, col)

    w = [Wire("MOT_IN1", "yellow", *sig("MOT_IN1"), "GPIO 18 to TB6612 AIN1"),
         Wire("MOT_IN2", "green", *sig("MOT_IN2"), "GPIO 19 to TB6612 AIN2"),
         Wire("DRV_nSLEEP", "white", *sig("DRV_nSLEEP"), "GPIO 20 to TB6612 STBY"),
         Wire("+3V3", "red", pin_hole("devkit", "J1-1"), Hole(17, "L+"), "DevKit 3V3 to the 3V3 rail"),
         Wire("GND", "black", pin_hole("devkit", "J3-1"), Hole(17, "L-"), "DevKit GND to the GND rail"),
         Wire("+5V", "orange", pin_hole("devkit", "J1-14"), Hole(4, "R+"), "DevKit 5V to the 5V rail"),
         Wire("GND", "black", Hole(20, "L-"), Hole(20, "R-"), "joins the two GND rails"),
         Wire("+3V3", "red", pin_hole("tb6612", "C-PWMA"), Hole(22, "L+"), "TB6612 PWMA held high"),
         Wire("GND", "black", pin_hole("tb6612", "C-GND"), Hole(29, "L-"), "TB6612 GND (control side)"),
         Wire("+3V3", "red", pin_hole("tb6612", "P-VCC"), Hole(23, "L+"), "TB6612 VCC (logic 3.3 V)"),
         Wire("GND", "black", pin_hole("tb6612", "P-GND"), Hole(24, "R-"), "TB6612 GND (power side)"),
         Wire("VBAT", "red", Term("buck", "OUT+"), pin_hole("tb6612", "P-VM"), "MP1584 OUT+ (6 V) to TB6612 VM"),
         Wire("GND", "black", Term("buck", "OUT-"), Hole(30, "R-"), "MP1584 OUT- to the GND rail"),
         Wire("MOT_M1", "red", pin_hole("tb6612", "P-AO1"), Term("motor", "M1"), "TB6612 AO1 to the motor"),
         Wire("MOT_M2", "black", pin_hole("tb6612", "P-AO2"), Term("motor", "M2"), "TB6612 AO2 to the motor")]
    for ref, r in HALL_ROWS.items():
        w.append(Wire("+5V", "orange", pin_hole(ref, "1"), Hole(r, "R+"), f"{ref} VCC to the 5V rail"))
        w.append(Wire("GND", "black", pin_hole(ref, "2"), Hole(r + 1, "R-"), f"{ref} GND to the GND rail"))
        net = next(n for n, (_, b, _) in PIN_MAP.items() if HALL_PARTS.get(b[0]) == ref)
        mcu = pin_hole(*nets[net][0])
        w.append(Wire(net, "blue", pin_hole(ref, "3", "g"), mcu, f"{ref} OUT to GPIO {PIN_MAP[net][0]}"))
    w += [Wire("VBAT_2S", "red", Term("battery", "+"), Term("buck", "IN+"), "18650 pack + to MP1584 IN+"),
          Wire("GND", "black", Term("battery", "-"), Term("buck", "IN-"), "18650 pack - to MP1584 IN-")]
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
        return {"buck": "MP1584", "battery": "18650 pack", "motor": "motor"}[e.ref] + f" {e.pin}"
    if e.col in RAILS:
        return f"{RAILS[e.col].lstrip('+')} rail ({e.col}, row {e.row})"
    for ref in ON_BOARD:
        for pin in BENCH.parts[ref].pins:
            if _seated(ref, pin).strip() == e.strip():
                if ref == "devkit":
                    hdr, k = pin.split("-")
                    n = (DEVKITM_J1 if hdr == "J1" else DEVKITM_J3)[int(k) - 1]
                    name = f"DevKit {'GPIO ' + n if n.isdigit() else n}"
                elif ref == "tb6612":
                    name = f"TB6612 {pin.split('-', 1)[1].replace('GND2', 'GND')}"
                else:
                    name = f"{ref} {next(s for s, p in A3144.items() if p == pin)}"
                return f"{name} ({e.row}{e.col})"
    return f"{e.row}{e.col}"


# --- drawing ------------------------------------------------------------------------------
P = 2.54
MX, MY = 12.0, 22.0  # where row 0 / the R+ rail sit on the sheet
COL_Y = {"R+": 0.0, "R-": P, **{c: 3 * P + i * P for i, c in enumerate("jihgf")},
         **{c: 10 * P + i * P for i, c in enumerate("edcba")}, "L-": 16 * P, "L+": 17 * P}


def xy(e: Hole | Term) -> tuple[float, float]:
    if isinstance(e, Term):
        return TERMS[(e.ref, e.pin)]
    return round(MX + e.row * P, 3), round(MY + COL_Y[e.col], 3)


_BY = MY + 17 * P + 18.0  # off-board parts, under the breadboard
TERMS = {("motor", "M1"): (MX + 60.0, _BY), ("motor", "M2"): (MX + 66.0, _BY),
         ("buck", "IN+"): (MX + 92.0, _BY), ("buck", "IN-"): (MX + 92.0, _BY + 6.0),
         ("buck", "OUT+"): (MX + 104.0, _BY), ("buck", "OUT-"): (MX + 104.0, _BY + 6.0),
         ("battery", "+"): (MX + 130.0, _BY), ("battery", "-"): (MX + 130.0, _BY + 6.0)}


def bench_svg() -> str:
    s = Svg()
    plan = wire_plan()
    W = MX * 2 + 64 * P
    table_y = _BY + 22.0
    H = table_y + 8 + 3.1 * len(plan) + 30
    s.add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.1f} {H:.1f}" width="{W * 10:.0f}" '
          f'height="{H * 10:.0f}" font-family="Helvetica, Arial, sans-serif">')
    s.add(f'<rect width="{W:.1f}" height="{H:.1f}" fill="#fbfaf7"/>')
    s.text(MX, 8.0, "lochness bench wiring (stage 1): parts on hand, one 830-point breadboard", 3.0, weight="bold", fill="#111")
    s.text(MX, 12.5, "Generated by build_wiring.py from wiring/bench.py. Same GPIO map as the dev kit, so the same "
                     "firmware runs on both. Hole = row (1-63) + column (a-j) or rail.", 1.4, fill="#555")
    s.text(MX, 15.0, "Power: USB into the DevKit (its 3V3 and 5V feed the rails). 2x 18650 in series -> MP1584 "
                     "(set to 6.0 V BEFORE connecting) -> TB6612 VM. All grounds common.", 1.4, fill="#555")

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

    outline(-2, 19, "i", "b", "#2b2b2b", "ESP32-C6 DevKit", "USB end off the board at row 1")
    outline(TB_ROW, TB_ROW + 7, "h", "c", "#c0392b", "TB6612FNG", "check the pin order on yours")
    for ref, r in HALL_ROWS.items():
        x, y = xy(Hole(r + 1, "f"))
        s.text(x, y + 3.4, ref.replace("hall_", "A3144 "), 1.1, "middle", fill="#111", weight="bold")
    for ref in ON_BOARD:
        for pin in BENCH.parts[ref].pins:
            h = _seated(ref, pin)
            x, y = xy(h)
            if ref == "devkit":
                hdr, k = pin.split("-")
                name = (DEVKITM_J1 if hdr == "J1" else DEVKITM_J3)[int(k) - 1]
            elif ref == "tb6612":
                name = pin.split("-", 1)[1].replace("GND2", "GND")
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
    box(MX + 88, _BY - 3, MX + 108, _BY + 9, "#1d4fa8", "MP1584EN (6.0 V out)")
    box(MX + 124, _BY - 3, MX + 150, _BY + 9, "#26a641", "2x 18650 in series")
    for (ref, pin), (x, y) in TERMS.items():
        s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
        s.text(x + (1.4 if ref != "buck" or pin.startswith("OUT") else -1.4), y + 0.45, pin, 1.0,
               "start" if ref != "buck" or pin.startswith("OUT") else "end")

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
    for note in ("Before powering: set the MP1584 to 6.0 V with a meter, unloaded. Charge each 18650 on its own with the "
                 "TP4056 (it is a 1-cell charger), never the pair in series.",
                 "The pack has no protection: stop at 3.0 V per cell (6.0 V pack). If your rails are split halfway along, "
                 "bridge each one across the split.",
                 "A3144: branded face towards you, legs 1 VCC, 2 GND, 3 OUT; needs a magnet's SOUTH pole on the branded "
                 "face. Firmware: enable the pull-up on GPIO 22, 23 and 2.",
                 "TB6612 pin order is SparkFun's (ROB-14451); check your board's silkscreen. The DevKit is drawn as the "
                 "DevKitM-1: GPIO numbers hold on a DevKitC-1, the rows don't."):
        s.text(MX, y, note, 1.25, fill="#7b241c")
        y += 2.4
    s.add("</svg>")
    return "\n".join(s.out) + "\n"
