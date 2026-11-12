"""Bought electronics and the motor, modelled from their spec sheets.

The product and the dev kit use the same chips: the product solders them to its main PCB,
the dev kit uses a breakout of each.

| Function | Chip (product) | Breakout (dev kit) |
|---|---|---|
| MCU, Thread + BLE | ESP32-C6-MINI-1-N4 | ESP32-C6-DevKitM-1 (the same module) |
| Motor driver | DRV8833PWPR | Pololu #2130 carrier |
| Bolt and door position | DRV5032FADBZR | SparkFun BOB-00717 SOT-23 adapter |
| 3.3 V from the cells | TPS629206DRLR | none: the DevKit makes its own 3.3 V |
| Motor + encoder | Pololu #5193 (150:1) / #5185 (30:1, worm) | the same |

Each model is built in its own frame (see each function), as coloured solids plus named
points (`pins`): header pins for wiring, the Hall element for magnet placement. Model.at()
places it. Dimensions are from the documents in SOURCES. Where a drawing gives no figure the
value is marked TYPICAL or UNCONFIRMED, and is listed in the README's unmeasured items.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from build123d import Box, Color, Compound, Cylinder, Location, Part, Pos, Rot

SOURCES = {
    "ESP32-C6-MINI-1": "https://www.espressif.com/sites/default/files/documentation/esp32-c6-mini-1_datasheet_en.pdf",
    "ESP32-C6-DevKitM-1": "https://dl.espressif.com/dl/schematics/esp32-c6-devkitm-1-dimensions.pdf",
    "DRV8833": "https://www.ti.com/lit/ds/symlink/drv8833.pdf",
    "DRV5032": "https://www.ti.com/lit/ds/symlink/drv5032.pdf",
    "TPS629206": "https://www.ti.com/lit/ds/symlink/tps629206.pdf",
    "Pololu 2130": "https://www.pololu.com/file/0J1615/drv8833-dual-motor-driver-carrier-dimensions.pdf",
    "Pololu 5193": "https://www.pololu.com/file/0J949/micro-metal-gearmotors-dimensions.pdf",
    "SparkFun BOB-00717": "https://cdn.sparkfun.com/datasheets/BreakoutBoards/SparkFun_SOT23-DIP-Adapter-v10.zip",
    "JST SH": "https://www.jst-mfg.com/product/pdf/eng/eSH.pdf",
}

EPOXY = Color(0.15, 0.15, 0.17)
TIN = Color(0.78, 0.78, 0.8)
SHIELD = Color(0.75, 0.76, 0.78)
GOLD = Color(0.85, 0.7, 0.3)
NYLON = Color(0.93, 0.93, 0.88)
HEADER = Color(0.1, 0.1, 0.1)
FR4_GREEN = Color(0.1, 0.45, 0.2)
FR4_BLUE = Color(0.1, 0.25, 0.6)
FR4_BLACK = Color(0.12, 0.12, 0.14)
FR4_RED = Color(0.65, 0.12, 0.12)
STEEL = Color(0.7, 0.7, 0.72)
MOTOR_CAN = Color(0.8, 0.8, 0.82)
BRASS = Color(0.85, 0.7, 0.3)

PITCH = 2.54
HEADER_H = 2.5  # pin header's plastic body


def _box(x0, x1, y0, y1, z0, z1, color) -> Part:
    b = Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)
    b.color = color
    return b


def _cyl(d, z0, z1, x, y, color) -> Part:
    c = Pos(x, y, (z0 + z1) / 2) * Cylinder(d / 2, z1 - z0)
    c.color = color
    return c


@dataclass
class Model:
    """A bought part in its own frame: coloured solids and named points."""
    solids: list[Part]
    pins: dict[str, tuple[float, float, float]] = field(default_factory=dict)

    def at(self, loc: Location) -> Compound:
        """The part placed by loc, as a Compound whose children keep their colours."""
        return Compound(children=[_moved(s, loc) for s in self.solids])

    def pin(self, loc: Location, name: str) -> tuple[float, float, float]:
        v = (loc * Pos(*self.pins[name])).position
        return (v.X, v.Y, v.Z)


def _moved(s: Part, loc: Location) -> Part:
    m = loc * s
    m.color = s.color
    return m


def _header_row(x: float, y0: float, n: int, z0: float, z1: float, along: str = "y") -> Part:
    """n-pin header body (plastic only; the pins are inside the breadboard or the board)."""
    if along == "y":
        return _box(x - PITCH / 2, x + PITCH / 2, y0 - PITCH / 2, y0 + (n - 0.5) * PITCH, z0, z1, HEADER)
    return _box(y0 - PITCH / 2, y0 + (n - 0.5) * PITCH, x - PITCH / 2, x + PITCH / 2, z0, z1, HEADER)


# --- chips: origin at the footprint centre, z = 0 on the PCB's top face, body up. -------
# Place on the underside of a board with Rot(180, 0, 0) at the board's bottom face.

def _gullwing(n_side: int, pitch: float, body_w: float, span: float, lead_w: float) -> list[Part]:
    """Leads on both long sides (the body's length is along X)."""
    leads = []
    for side in (-1, 1):
        for k in range(n_side):
            x = (k - (n_side - 1) / 2) * pitch
            y0, y1 = sorted((side * body_w / 2, side * span / 2))
            leads.append(_box(x - lead_w / 2, x + lead_w / 2, y0, y1, 0, 0.15, TIN))
    return leads


def drv5032_dbz() -> Model:
    """DRV5032FADBZR, SOT-23 (DBZ). Body 2.92 x 1.30 (2.80-3.04 x 1.20-1.40), 1.12 max high,
    2.37 across the leads (2.10-2.64), 0.95 pitch. Pins: 1 VCC, 2 OUT (one side), 3 GND.
    The Hall element is central (+-70 um) and 0.65 mm above the seating plane."""
    body = _box(-1.46, 1.46, -0.65, 0.65, 0.05, 1.12, EPOXY)
    leads = [_box(x - 0.2, x + 0.2, -1.185, -0.65, 0, 0.15, TIN) for x in (-0.95, 0.95)]
    leads.append(_box(-0.2, 0.2, 0.65, 1.185, 0, 0.15, TIN))
    return Model([body, *leads], {"VCC": (-0.95, -1.0, 0), "OUT": (0.95, -1.0, 0), "GND": (0, 1.0, 0),
                                  "sensor": (0, 0, 0.65)})


def drv8833_pwp() -> Model:
    """DRV8833PWPR, HTSSOP-16 (PWP0016C) with thermal pad. Body 5.0 x 4.4 (4.9-5.1 x 4.3-4.5),
    1.2 max high, 6.4 across the leads (6.2-6.6), 0.65 pitch, 8 per side."""
    return Model([_box(-2.5, 2.5, -2.2, 2.2, 0.05, 1.2, EPOXY), *_gullwing(8, 0.65, 4.4, 6.4, 0.25)])


def tps629206_drl() -> Model:
    """TPS629206DRLR, SOT-583 (DRL0008A). Body 2.1 x 1.2 (2.0-2.2 x 1.1-1.3), 0.6 max high,
    1.6 across the leads (1.5-1.7), 0.5 pitch, 4 per side."""
    return Model([_box(-1.05, 1.05, -0.6, 0.6, 0.0, 0.6, EPOXY), *_gullwing(4, 0.5, 1.2, 1.6, 0.2)])


def esp32_c6_mini_1() -> Model:
    """ESP32-C6-MINI-1-N4: 13.2 x 16.6 x 2.4, 0.8 mm module PCB, shield can ~11.95 x 9.95.
    The pad field is 11.2 tall, 0.8 from the bottom edge; the top ~4.6 mm is the antenna,
    which needs a copper-free keep-out on the board below it (or to overhang the board).
    Frame: the module's length along Y, antenna end at +Y."""
    pcb = _box(-6.6, 6.6, -8.3, 8.3, 0, 0.8, FR4_BLUE)
    can = _box(-5.975, 5.975, -7.4, 2.55, 0.8, 2.4, SHIELD)
    ant = _box(-5.5, 5.5, 4.4, 7.9, 0.8, 0.85, GOLD)  # the printed antenna, for orientation
    return Model([pcb, can, ant], {"antenna": (0, 6.0, 0.8)})


def jst_sh6_side() -> Model:
    """JST SM06B-SRSS-TB, 6-pin SH side-entry header for the motor cable (mates SHR-06V-S).
    Body 8.0 wide, 4.25 deep, 2.9 high, 1.0 pitch; the plug goes in along +Y."""
    return Model([_box(-4.0, 4.0, -2.125, 2.125, 0, 2.9, NYLON)], {"entry": (0, 2.125, 1.45)})


# --- boards ---------------------------------------------------------------------------

DEVKITM_PCB_T = 1.6  # UNCONFIRMED: no thickness on Espressif's drawing; 1.6 is typical
# Header pins, pin 1 first (Espressif user guide). Pin 1 is at the antenna end.
DEVKITM_J1 = ["3V3", "RST", "2", "3", "4", "5", "0", "1", "8", "6", "7", "14", "G", "5V", "G"]
DEVKITM_J3 = ["G", "TX", "RX", "23", "22", "21", "20", "19", "18", "15", "9", "G", "13", "12", "G"]


def devkitm_1() -> Model:
    """ESP32-C6-DevKitM-1 (Espressif dimension drawing). Board 25.40 x 48.26, r0.5.

    Frame: origin at the board's bottom face, at the corner by the UART USB port; X across
    the board (J1 at x 1.27, J3 at x 24.13), Y along it towards the antenna, which overhangs
    the far edge by 5.37. Headers hang below the board on 2.5 mm plastic bodies (their pins
    go into the breadboard and are not drawn). Pins are named 'J1-1'..'J3-15' at the bottom
    of the header bodies, plus the GPIO name, e.g. '18'.
    """
    t = DEVKITM_PCB_T
    s = [_box(0, 25.4, 0, 48.26, 0, t, FR4_BLACK)]
    mod = esp32_c6_mini_1()
    s += [_moved(p, Pos(12.7, 53.63 - 8.3, t)) for p in mod.solids]
    for x in (5.5, 19.9):  # USB-C receptacles (16-pin), flush with the near edge: UART, native USB
        s.append(_box(x - 4.47, x + 4.47, -0.2, 7.15, t, t + 3.26, SHIELD))
    for x in (7.7, 17.7):  # RESET, BOOT (TYPICAL 3.9 x 3.0 SMD tact switch)
        s.append(_box(x - 1.95, x + 1.95, 12.77 - 1.5, 12.77 + 1.5, t, t + 1.6, STEEL))
    s.append(_box(5.6, 7.4, 16.3, 19.6, t, t + 0.9, Color(0.95, 0.95, 0.95)))  # RGB LED (GPIO 8)
    pins = {}
    for x, hdr, names in ((1.27, "J1", DEVKITM_J1), (24.13, "J3", DEVKITM_J3)):
        s.append(_box(x - 1.27, x + 1.27, 11.125 - 1.27, 46.685 + 1.27, -HEADER_H, 0, HEADER))
        for k, n in enumerate(names):
            p = (x, 46.685 - k * PITCH, -HEADER_H)
            pins[f"{hdr}-{k + 1}"] = p
            if n.isdigit() or n in ("3V3", "5V"):
                pins.setdefault(n, p)
    return Model(s, pins)


POLOLU_2130_PWR = ["GND", "VIN", "BOUT1", "BOUT2", "AOUT2", "AOUT1", "AISEN", "BISEN"]
POLOLU_2130_CTL = ["GND", "VMM", "BIN1", "BIN2", "AIN2", "AIN1", "nSLEEP", "nFAULT"]


def pololu_2130() -> Model:
    """Pololu #2130 DRV8833 carrier (Pololu dimension drawing). Board 12.7 x 20.3, 1.57 thick,
    16 holes d1.02 in two rows of 8, 10.16 apart, 1.27 from the edges; tallest part 1.8.

    Frame: board's bottom face, origin at the corner by both GND pins; X across (power row at
    x 1.27, control row at x 11.43), Y along. Headers hang below on their plastic bodies.
    The DRV8833 on the carrier is the QFN version (TYPICAL 4 x 4 x 0.9); same die and pins.
    """
    t = 1.57
    s = [_box(0, 12.7, 0, 20.3, 0, t, FR4_BLUE),
         _box(4.35, 8.35, 8.15, 12.15, t, t + 0.9, EPOXY),  # DRV8833 (QFN)
         _box(4.6, 8.1, 3.0, 5.0, t, t + 1.8, Color(0.75, 0.6, 0.35)),  # bulk capacitor, the tallest
         _header_row(1.27, 1.27, 8, -HEADER_H, 0), _header_row(11.43, 1.27, 8, -HEADER_H, 0)]
    pins = {}
    for x, row, tag in ((1.27, POLOLU_2130_PWR, "P"), (11.43, POLOLU_2130_CTL, "C")):
        for k, n in enumerate(row):
            pins[f"{tag}-{n}"] = (x, 1.27 + k * PITCH, -HEADER_H)
            pins.setdefault(n, (x, 1.27 + k * PITCH, -HEADER_H))
    return Model(s, pins)


def sot23_adapter_with_hall() -> Model:
    """SparkFun BOB-00717 SOT-23 adapter carrying a DRV5032FA, worn chip-down.

    Board 10.16 x 7.62 (the Eagle outline; SparkFun's product page says 8.6 x 11.7:
    UNCONFIRMED, measure yours), 1.6 thick, 6 holes in two rows of 3 (2.54 pitch, 7.62 apart).
    Frame: origin at the board centre on its chip-side face, chip hanging down (-Z), the two
    1x3 headers' plastic on the other face (+Z) with their long pins up. Which holes carry
    VCC, OUT and GND depends on how the chip sits on the SOT-23-6 pads: follow the silkscreen.
    """
    s = [_box(-5.08, 5.08, -3.81, 3.81, 0, 1.6, FR4_RED)]
    chip = drv5032_dbz()
    flip = Rot(180, 0, 0)
    s += [_moved(p, flip) for p in chip.solids]
    for x in (-3.81, 3.81):
        s.append(_box(x - 1.27, x + 1.27, -3.81, 3.81, 1.6, 1.6 + HEADER_H, HEADER))
        for k in (-1, 0, 1):
            s.append(_box(x - 0.32, x + 0.32, k * PITCH - 0.32, k * PITCH + 0.32, 1.6 + HEADER_H,
                          1.6 + HEADER_H + 6.0, GOLD))
    pins = {f"{n}": (x, k * PITCH, 1.6 + HEADER_H + 6.0)
            for x, names in ((-3.81, (1, 2, 3)), (3.81, (6, 5, 4))) for n, k in zip(names, (-1, 0, 1))}
    pins["sensor"] = (0, 0, -0.65)
    return Model(s, pins)


# --- the motor --------------------------------------------------------------------------

MOTOR = dict(w=12.0, t=10.0, gearbox=9.0, can=15.0, enc=8.0, enc_w=13.6, enc_t=13.0, side_t=14.0,
             overall=32.5, shaft_d=3.0, shaft_flat=2.5, shaft_l=10.0, boss_d=4.0, boss_h=0.7,
             face_holes=9.0)
MOTOR_PINS = ["enc GND", "enc B", "enc A", "enc VCC", "M2", "M1"]  # connector pin 1..6


def micro_gearmotor() -> Model:
    """Pololu HPCB 6 V micro metal gearmotor with 12 CPR encoder, side connector (#5193
    150:1, #5185 30:1: the gearbox is 9 long for every ratio but 1000:1).

    Pololu drawing: gearbox 12 x 10 x 9; motor 15; encoder + connector 8, 13.6 x 14 across
    (32.5 overall, max); output shaft d3 D (2.5 across the flat), 10 out of a d4 x 0.7 boss
    (the product page says 9: UNCONFIRMED); 2x M1.6 in the face, 9 apart across the 12 mm
    width. Connector: JST SH 6-pin side entry, plug goes in along +Y.
    Frame: origin on the shaft axis at the gearbox face; body up +Z, shaft down -Z; the 12 mm
    width along X.
    """
    m = MOTOR
    w, t = m["w"] / 2, m["t"] / 2
    z1 = m["gearbox"]
    z2 = z1 + m["can"]
    z3 = m["overall"]
    s = [_box(-w, w, -t, t, 0, z1, BRASS)]
    can = Pos(0, 0, (z1 + z2) / 2) * Cylinder(w, m["can"]) & Pos(0, 0, (z1 + z2) / 2) * Box(2 * w, 2 * t, m["can"])
    can.color = MOTOR_CAN
    s.append(can)
    et = m["enc_t"] / 2
    s.append(_box(-m["enc_w"] / 2, m["enc_w"] / 2, -et, et, z2, z3 - 0.5, FR4_BLACK))
    s.append(_box(-4.0, 4.0, et - 2.9, et + 1.0, z2 + 2.0, z2 + 6.25, NYLON))  # side-entry SH header
    s.append(_cyl(m["boss_d"], -m["boss_h"], 0, 0, 0, BRASS))
    shaft = Pos(0, 0, -m["shaft_l"] / 2) * Cylinder(m["shaft_d"] / 2, m["shaft_l"]) \
        - Pos(m["shaft_flat"] - m["shaft_d"] / 2 + 1.0, 0, -m["shaft_l"] / 2) * Box(2.0, 4.0, m["shaft_l"] + 1)
    shaft.color = STEEL
    s.append(shaft)
    return Model(s, {"connector": (0, et + 1.0, z2 + 4.1)})
