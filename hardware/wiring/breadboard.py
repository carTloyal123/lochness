"""Breadboard wiring drawings (SVG), straight from the wire plan.

Stage 2 (the printed dev kit): a top view of the deck at its true scale, every hole where
devkit/rig.py puts it, each wire from wire_plan() between the holes it really uses, and a
table of them. The parts off the breadboards (the Hall adapters under the deck, the motor
cable, the battery contacts) are drawn schematically at the deck's lower edge: their holes
and pins are real, their positions approximate.
"""

from __future__ import annotations

import math
from html import escape

from concepts import parts as P
from devkit import rig as R
from wiring.netlist import DEVKIT, HALL_HOLE, NET_TITLES

INK = {"red": "#d62828", "black": "#222", "yellow": "#e9c40a", "orange": "#f08a1c", "white": "#f4f4f4",
       "blue": "#2557d6", "purple": "#8b3fc1", "green": "#26a641"}
X0, X1, Y0, Y1 = -46.0, 46.0, -4.0, 122.0  # drawing area (rig mm)
TABLE_W = 128.0


def _p(x: float, y: float) -> tuple[float, float]:
    """rig XY (y up) -> SVG mm (y down)."""
    return round(x - X0, 3), round(Y1 - y, 3)


class Svg:
    def __init__(self):
        self.out: list[str] = []

    def add(self, s: str):
        self.out.append(s)

    def text(self, x, y, s, size=1.6, anchor="start", rot=0, fill="#333", weight="normal"):
        t = f' transform="rotate({rot} {x} {y})"' if rot else ""
        self.add(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{fill}" '
                 f'font-weight="{weight}"{t}>{escape(s)}</text>')

    def rect(self, x0, y0, x1, y1, fill, stroke="#999", sw=0.25, rx=0.6, opacity=1.0):
        (ax, ay), (bx, by) = _p(x0, y1), _p(x1, y0)
        self.add(f'<rect x="{ax}" y="{ay}" width="{round(bx - ax, 3)}" height="{round(by - ay, 3)}" rx="{rx}" '
                 f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}" fill-opacity="{opacity}"/>')


def _term_xy() -> dict[tuple[str, str], tuple[float, float]]:
    """Schematic positions for the off-board ends, under the deck."""
    xy = {}
    for k, pin in enumerate("123456"):  # motor cable, in the notch round the motor
        xy[("motor", pin)] = (-6.25 + k * 2.5, 30.0)
    for k, ref in enumerate(("hall_locked", "hall_unlocked", "hall_door")):  # adapters, pins up
        cx = -26.0 + 26.0 * k
        for hole_n in "123456":
            n = int(hole_n)
            col = -3.81 if n <= 3 else 3.81
            row = (n - 1) if n <= 3 else (6 - n)
            xy[(ref, hole_n)] = (cx + col, 12.0 + (1 - row) * 2.54)
    xy[("battery", "+")] = (22.0, 30.0)
    xy[("battery", "-")] = (-22.0, 30.0)
    return xy


def _end_xy(e, terms) -> tuple[float, float]:
    if isinstance(e, R.Hole):
        x, y, _ = e.xyz()
        return x, y
    return terms[(e.ref, e.pin)]


DEVKIT_PLACEMENT = [  # (part, where it goes): the layout drawing's list
    ("Deck", "on the chassis spacers and the two tall posts, over the cells"),
    ("MCU breadboard (170 points)", "upper, held by two clamp bars; strips run across the deck"),
    ("ESP32-C6-DevKitM-1", "on the MCU breadboard: J1 in column i, J3 in column b; USB end at the left, "
     "towards the cover's slot; antenna past the right edge"),
    ("DRV breadboard (170 points)", "lower, held by two clamp bars"),
    ("Pololu #2130 DRV8833 carrier", "on the DRV breadboard: control row in column g, power row in column e"),
    (f"3V3 and GND rows", f"DRV breadboard rows {R.RAIL_3V3} and {R.RAIL_GND}, columns f-j: shared supply rows"),
    ("3x DRV5032 on BOB-00717 adapters", "glued chip-down in the Hall carrier, under the deck, pins up"),
    ("Motor cable", "from the motor's side connector, through the notch, bare ends into the breadboard"),
    ("4xAA cells", "in the chassis under the deck; leads from the end contacts"),
]


def devkit_svg(wires: bool = True) -> str:
    """The dev kit drawing: with wires (the wiring diagram) or without (where the parts go)."""
    s = Svg()
    plan = R.wire_plan() if wires else []
    terms = _term_xy()
    W, H = X1 - X0 + TABLE_W, Y1 - Y0
    s.add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W * 10:.0f}" height="{H * 10:.0f}" '
          'font-family="Helvetica, Arial, sans-serif">')
    s.add(f'<rect width="{W}" height="{H}" fill="#fbfaf7"/>')

    # deck
    s.rect(-30.0, R.DECK_Y[0], 30.0, R.DECK_Y[1], "#e7e3da", "#b8b2a4", 0.3, 3.0)

    # breadboards: body, strip bars (what's joined underneath), holes, row/column labels
    l, w = R.MINI_BB["l"], R.MINI_BB["w"]
    for bb, yc in R.BOARDS.items():
        s.rect(-l / 2, yc - w / 2, l / 2, yc + w / 2, "#ffffff", "#c9c5bd", 0.3, 1.0)
        for row in range(1, 18):
            for cols in ("abcde", "fghij"):
                (ax, ay), (bx, by) = _p(*R.hole(bb, row, cols[0])[:2]), _p(*R.hole(bb, row, cols[-1])[:2])
                s.add(f'<line x1="{ax}" y1="{ay}" x2="{bx}" y2="{by}" stroke="#e3e0d8" stroke-width="1.5" '
                      'stroke-linecap="round"/>')
            for col in "abcdefghij":
                x, y = _p(*R.hole(bb, row, col)[:2])
                s.add(f'<rect x="{x - 0.45}" y="{y - 0.45}" width="0.9" height="0.9" fill="#7d7a73"/>')
            if row % 2 == 1 or row in (R.RAIL_3V3, R.RAIL_GND):
                x, y = _p(*R.hole(bb, row, "a")[:2])
                s.text(x, y + 2.9, str(row), 1.1, "middle", fill="#aaa")
        for col in "abcdefghij":
            x, y = _p(*R.hole(bb, 1, col)[:2])
            s.text(x - 2.6, y + 0.4, col, 1.1, "middle", fill="#aaa")
        x, y = _p(l / 2 + 1.0, yc + w / 2 - 1.0)
        s.text(x, y, {"bb_mcu": "MCU breadboard", "bb_drv": "DRV breadboard"}[bb], 1.5, rot=90, fill="#8a8476")

    # rails on the DRV breadboard
    for row, name, colour in ((R.RAIL_3V3, "3V3 row", INK["red"]), (R.RAIL_GND, "GND row", "#222")):
        (ax, ay), (bx, by) = _p(*R.hole("bb_drv", row, "f")[:2]), _p(*R.hole("bb_drv", row, "j")[:2])
        s.add(f'<line x1="{ax}" y1="{ay}" x2="{bx}" y2="{by}" stroke="{colour}" stroke-width="1.7" '
              'stroke-opacity="0.18" stroke-linecap="round"/>')
        s.text(ax + 0.4, ay - 1.2, name, 1.1, "start", rot=-90, fill=colour, weight="bold")

    # boards over the breadboards, with their pin names at the header holes
    yc = R.BOARDS["bb_mcu"]
    s.rect(-28.905, yc + 12.7 - 25.4, -28.905 + 48.26, yc + 12.7, "#2b2b2b", "#111", 0.3, 0.6, 0.18)
    s.rect(-28.905 + 48.26, yc + 12.7 - 25.4 + 6.1, -28.905 + 53.63, yc + 12.7 - 6.1, "#c9a227", "#9c7d16", 0.2, 0.3, 0.35)
    s.text(*_p(-28.0, yc - 0.3), "ESP32-C6-DevKitM-1", 2.0, fill="#111", weight="bold")
    s.text(*_p(-28.0, yc - 3.0), "USB end (cover slot) at left, antenna at right", 1.2, fill="#444")
    yd = R.BOARDS["bb_drv"] + R.COL_OFF["e"] - 1.27
    s.rect(8.89 - 20.3, yd, 8.89, yd + 12.7, "#1d4fa8", "#123", 0.3, 0.5, 0.18)
    s.text(*_p(-11.0, yd + 7.6), "Pololu #2130", 1.4, fill="#123", weight="bold")
    s.text(*_p(-11.0, yd + 5.4), "DRV8833", 1.4, fill="#123", weight="bold")
    for ref, names in (("devkit", None), ("drv8833", None)):
        for pin in DEVKIT.parts[ref].pins:
            h = R.board_hole(ref, pin, "i" if pin.startswith("J1") else "b" if pin.startswith("J3")
                             else "e" if pin.startswith("P-") else "g")
            x, y = _p(*h.xyz()[:2])
            if ref == "devkit":
                hdr, k = pin.split("-")
                name = (P.DEVKITM_J1 if hdr == "J1" else P.DEVKITM_J3)[int(k) - 1]
            else:
                name = pin.split("-", 1)[1]
            s.add(f'<circle cx="{x}" cy="{y}" r="0.75" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
            up = pin.startswith(("J1", "C-"))  # names run along the strip, away from the board
            s.text(x + 0.3, y + (-1.1 if up else 1.1), name, 0.95, "start" if up else "end", rot=-90, fill="#111")

    # off-board ends
    for (ref, pin), (x, y) in terms.items():
        px, py = _p(x, y)
        s.add(f'<circle cx="{px}" cy="{py}" r="0.7" fill="#d4af37" stroke="#7a6418" stroke-width="0.15"/>')
    for k, ref in enumerate(("hall_locked", "hall_unlocked", "hall_door")):
        cx = -26.0 + 26.0 * k
        s.rect(cx - 5.08, 12.0 - 3.81, cx + 5.08, 12.0 + 3.81, "#c0392b", "#7b241c", 0.25, 0.4, 0.22)
        s.text(*_p(cx, 6.6), f"{ref} (BOB-00717, pins up)", 1.25, "middle", weight="bold")
        for sig, hole_n in HALL_HOLE.items():
            x, y = _p(*terms[(ref, hole_n)])
            s.text(x - 1.2, y + 0.45, f"{hole_n} {sig}", 0.9, "end")
    s.text(*_p(0.0, 17.5), "Hall carrier, under the deck: holes VCC 1, OUT 3, GND 5 ASSUMED (check the silkscreen)",
           1.2, "middle", fill="#7b241c")
    s.rect(-8.0, 28.4, 8.0, 31.6, "#f2f2f2", "#888", 0.25, 0.4)
    s.text(*_p(0.0, 26.6), "motor cable (JST SH, pins 1-6)", 1.2, "middle", weight="bold")
    for k, pin in enumerate("123456"):
        x, y = _p(*terms[("motor", pin)])
        s.text(x, y + 2.6, pin, 0.9, "middle")
    for sign in "+-":
        x, y = _p(*terms[("battery", sign)])
        s.text(x, y + 2.4, f"4xAA {sign}", 1.2, "middle", weight="bold")

    # wires: a gentle arc between the two holes, each bowed its own amount so none hide
    for k, wr in enumerate(plan):
        (ax, ay), (bx, by) = _p(*_end_xy(wr.a, terms)), _p(*_end_xy(wr.b, terms))
        dx, dy = bx - ax, by - ay
        d = math.hypot(dx, dy) or 1.0
        bow = (3.0 + 1.6 * (k % 5)) * (1 if k % 2 else -1) * min(1.0, d / 25.0)
        cx, cy = (ax + bx) / 2 - dy / d * bow, (ay + by) / 2 + dx / d * bow
        path = f"M{ax:.2f},{ay:.2f} Q{cx:.2f},{cy:.2f} {bx:.2f},{by:.2f}"
        edge = "#555" if wr.colour in ("white", "yellow") else "#fff"
        s.add(f'<path d="{path}" fill="none" stroke="{edge}" stroke-width="0.95" stroke-linecap="round" opacity="0.9"/>')
        s.add(f'<path d="{path}" fill="none" stroke="{INK[wr.colour]}" stroke-width="0.6" stroke-linecap="round">'
              f'<title>{escape(wr.desc)}</title></path>')
        for x, y in ((ax, ay), (bx, by)):
            s.add(f'<circle cx="{x}" cy="{y}" r="0.5" fill="{INK[wr.colour]}" stroke="{edge}" stroke-width="0.15"/>')

    # table
    tx = X1 - X0 + 3.0
    s.text(tx, 6.0, f"lochness dev kit {'wiring' if wires else 'layout'} (stage 2)", 3.0, weight="bold", fill="#111")
    s.text(tx, 9.5, "Generated by build_wiring.py from wiring/netlist.py (DEVKIT) and devkit/rig.py "
                    "wire_plan(). Holes: board, row 1-17, column a-j.", 1.3, fill="#555")
    s.text(tx, 11.5, "DevKit from USB (its 3V3 feeds the rows). 4xAA into the carrier's VIN. All grounds common.",
           1.3, fill="#555")
    y = 15.5
    if not wires:
        for part, where in DEVKIT_PLACEMENT:
            s.text(tx, y + 0.5, part, 1.35, weight="bold", fill="#111")
            s.text(tx, y + 2.6, where, 1.15)
            y += 5.6
        s.add("</svg>")
        return "\n".join(s.out) + "\n"
    for head, xo in (("#", 0), ("Signal", 4), ("From", 34), ("To", 82)):
        s.text(tx + xo, y, head, 1.4, weight="bold")
    for k, wr in enumerate(plan, 1):
        y += 3.2
        s.add(f'<rect x="{tx - 1}" y="{y - 2.2}" width="{TABLE_W - 4}" height="3.2" fill="{"#f1eee7" if k % 2 else "none"}"/>')
        s.add(f'<line x1="{tx + 1.5}" y1="{y - 0.5}" x2="{tx + 3.2}" y2="{y - 0.5}" stroke="{INK[wr.colour]}" '
              f'stroke-width="1.2"/>')
        s.text(tx + 0.4, y, str(k), 1.2, "end")
        s.text(tx + 4, y, NET_TITLES[DEVKIT.net_of(wr.net)], 1.25)
        s.text(tx + 34, y, R.end_label(wr.a), 1.2)
        s.text(tx + 82, y, R.end_label(wr.b), 1.2)
    y += 6
    for note in ("Check before powering: the Hall adapter holes (VCC/OUT/GND) depend on how each chip is soldered;",
                 "the DevKitM-1 board thickness and mini breadboard size are UNCONFIRMED (fit only, not wiring).",
                 "The dev kit powers the motor encoder all the time; the product switches it (ENC_nEN, GPIO 5)."):
        s.text(tx, y, note, 1.2, fill="#7b241c")
        y += 2.2
    s.add("</svg>")
    return "\n".join(s.out) + "\n"
