"""The lock's net list: the one definition that the dev kit model (devkit/rig.py), the
breadboard drawings and the product's KiCad schematic all read, so they can't disagree.

Three layers:

- PIN_MAP: which GPIO does what. The firmware's pin table should match it.
- CORE: the circuit as function blocks (MCU, motor driver, motor, the three Hall switches,
  battery) and the nets between them. Every stage builds this same circuit.
- One Stage per build: the parts standing in for each block, and which pin of which part
  carries each block pin. A stage may add parts and nets of its own (the product's
  regulator and passives), merge nets it doesn't keep apart (the dev kit doesn't switch the
  encoder's supply), and declare pins its boards already join on their own copper.

A stage may also leave block pins out (the bench motor has no encoder) or move one to
another net (the bench's A3144 Hall switches need 5 V), each with its reason.

BENCH (wiring/bench.py) is stage 1, loose breadboards with the parts on hand. DEVKIT (below)
is stage 2, the printed dev kit. PRODUCT (wiring/product.py) is stage 3, the main PCB.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from concepts.parts import DEVKITM_J1, DEVKITM_J3, MOTOR_PINS, POLOLU_2130_CTL, POLOLU_2130_PWR

Pin = tuple[str, str]  # (part or block, pin)

# --- the pin map ---------------------------------------------------------------------------
# net: (GPIO, the block pin at the other end, what it is). Avoids the strapping pins
# (8, 9, 15), USB (12, 13) and UART0 (16, 17). The encoder is read by the PCNT unit: any GPIO.
PIN_MAP: dict[str, tuple[int, Pin, str]] = {
    "MOT_IN1": (18, ("DRIVER", "AIN1"), "Motor A in 1 (PWM)"),
    "MOT_IN2": (19, ("DRIVER", "AIN2"), "Motor A in 2 (PWM)"),
    "DRV_nSLEEP": (20, ("DRIVER", "nSLEEP"), "Driver enable"),
    "DRV_nFAULT": (3, ("DRIVER", "nFAULT"), "Driver fault (open drain)"),
    "ENC_A": (21, ("MOTOR", "3"), "Motor encoder A"),
    "ENC_B": (4, ("MOTOR", "2"), "Motor encoder B"),
    "HALL_LOCKED": (22, ("HALL_LOCKED", "OUT"), "Bolt locked (DRV5032 OUT)"),
    "HALL_UNLOCKED": (23, ("HALL_UNLOCKED", "OUT"), "Bolt unlocked (DRV5032 OUT)"),
    "HALL_DOOR": (2, ("HALL_DOOR", "OUT"), "Door (DRV5032 OUT)"),
}
RESERVED_GPIO = {8: "strapping", 9: "strapping (BOOT)", 15: "strapping", 12: "USB D-",
                 13: "USB D+", 16: "UART0 TX", 17: "UART0 RX"}
HALL_BLOCKS = ("HALL_LOCKED", "HALL_UNLOCKED", "HALL_DOOR")

# --- the circuit, as function blocks ---------------------------------------------------------
# Motor block pins are the Pololu connector's: 1 encoder GND, 2 B, 3 A, 4 VCC, 5 M2, 6 M1.
assert MOTOR_PINS == ["enc GND", "enc B", "enc A", "enc VCC", "M2", "M1"]
CORE: dict[str, list[Pin]] = {
    "VBAT": [("BATTERY", "+"), ("DRIVER", "VM")],
    "+3V3": [("MCU", "3V3")] + [(h, "VCC") for h in HALL_BLOCKS],
    "ENC_VCC": [("MOTOR", "4")],
    "GND": [("BATTERY", "-"), ("MCU", "GND"), ("DRIVER", "GND"), ("MOTOR", "1")]
           + [(h, "GND") for h in HALL_BLOCKS],
    "MOT_M1": [("DRIVER", "AOUT1"), ("MOTOR", "6")],
    "MOT_M2": [("DRIVER", "AOUT2"), ("MOTOR", "5")],
} | {net: [("MCU", f"GPIO{gpio}"), other] for net, (gpio, other, _) in PIN_MAP.items()}

NET_TITLES = {"VBAT": "Battery + (4xAA, 6 V)", "+3V3": "3.3 V", "ENC_VCC": "Motor encoder power",
              "GND": "Ground", "MOT_M1": "Motor M1", "MOT_M2": "Motor M2"} \
    | {net: desc for net, (_, _, desc) in PIN_MAP.items()}


@dataclass
class Part:
    ref: str
    name: str  # what it is, in a few words
    pins: list[str]


@dataclass
class Stage:
    key: str
    title: str
    parts: dict[str, Part]
    blocks: dict[Pin, Pin]  # block pin -> part pin
    extra: dict[str, list[Pin]] = field(default_factory=dict)  # this stage's own connections
    aliases: dict[str, str] = field(default_factory=dict)  # nets this stage merges into another
    ties: list[tuple[Pin, ...]] = field(default_factory=list)  # pins a board joins itself
    omit: dict[Pin, str] = field(default_factory=dict)  # block pins this stage doesn't build, and why
    moves: dict[Pin, str] = field(default_factory=dict)  # block pins this stage puts on another net

    def net_of(self, net: str) -> str:
        while net in self.aliases:
            net = self.aliases[net]
        return net

    def nets(self) -> dict[str, list[Pin]]:
        """Net name -> part pins, in order, each pin once."""
        out: dict[str, list[Pin]] = {}
        for net, pins in CORE.items():
            for b in pins:
                if b in self.omit:
                    continue
                if b not in self.blocks:
                    raise KeyError(f"{self.key}: no part pin for block pin {b}")
                out.setdefault(self.net_of(self.moves.get(b, net)), []).append(self.blocks[b])
        for net, pins in self.extra.items():
            out.setdefault(self.net_of(net), []).extend(pins)
        for net, pins in out.items():
            for ref, pin in pins:
                if ref not in self.parts or pin not in self.parts[ref].pins:
                    raise KeyError(f"{self.key}: net {net} names {ref} pin {pin}, which doesn't exist")
            out[net] = list(dict.fromkeys(pins))
        return out

    def connectivity(self) -> set[frozenset[Pin]]:
        """The nets as sets of part pins, with the boards' own ties merged in: what a
        continuity tester would find."""
        return merge([set(p) for p in self.nets().values()] + [set(t) for t in self.ties])

    def check(self) -> list[str]:
        """Each part pin on one net at most, and every net joins two pins or more."""
        problems, seen = [], {}
        for net, pins in self.nets().items():
            if len(pins) < 2:
                problems.append(f"{self.key}: net {net} has only {pins}")
            for p in pins:
                if p in seen:
                    problems.append(f"{self.key}: {p} is on {seen[p]} and {net}")
                seen[p] = net
        return problems


def merge(groups: list[set]) -> set[frozenset]:
    """Union of overlapping sets."""
    out: list[set] = []
    for g in groups:
        g = set(g)
        for o in [o for o in out if o & g]:
            g |= o
            out.remove(o)
        out.append(g)
    return {frozenset(g) for g in out if len(g) > 1}


# --- stage 2: the printed dev kit ----------------------------------------------------------
# Part refs are the dev kit model's names (devkit/rig.py). DevKit pins are header positions
# ('J1-1' .. 'J3-15'); carrier pins are 'P-<name>' (power row) and 'C-<name>' (control row);
# adapter pins are its six holes; the motor's are its cable's pins.

def _devkit_pin(name: str) -> str:
    for hdr, pins in (("J1", DEVKITM_J1), ("J3", DEVKITM_J3)):
        if name in pins:
            return f"{hdr}-{pins.index(name) + 1}"
    raise KeyError(name)


# Which adapter holes carry the DRV5032's pins depends on how it sits on the SOT-23-6 pads;
# these assume chip pin 1 on pad 1, pin 2 on pad 3, pin 3 on pad 5. UNCONFIRMED: check the
# silkscreen of the adapters you have.
HALL_HOLE = {"VCC": "1", "OUT": "3", "GND": "5"}
HALL_PARTS = {"HALL_LOCKED": "hall_locked", "HALL_UNLOCKED": "hall_unlocked", "HALL_DOOR": "hall_door"}

DEVKIT = Stage(
    key="devkit", title="Stage 2: printed dev kit",
    parts={
        "devkit": Part("devkit", "ESP32-C6-DevKitM-1",
                       [f"J1-{k + 1}" for k in range(15)] + [f"J3-{k + 1}" for k in range(15)]),
        "drv8833": Part("drv8833", "Pololu #2130 DRV8833 carrier",
                        [f"P-{n}" for n in POLOLU_2130_PWR] + [f"C-{n}" for n in POLOLU_2130_CTL]),
        **{r: Part(r, f"DRV5032FA on SparkFun BOB-00717 ({r.split('_')[1]})", list("123456"))
           for r in HALL_PARTS.values()},
        "motor": Part("motor", "Pololu #5193 motor cable (JST SH 6-pin)", list("123456")),
        "battery": Part("battery", "4xAA lithium, end contacts", ["+", "-"]),
    },
    blocks={
        ("MCU", "3V3"): ("devkit", _devkit_pin("3V3")),
        ("MCU", "GND"): ("devkit", "J3-1"),
        **{("MCU", f"GPIO{g}"): ("devkit", _devkit_pin(str(g))) for g, _, _ in PIN_MAP.values()},
        ("DRIVER", "VM"): ("drv8833", "P-VIN"),
        ("DRIVER", "GND"): ("drv8833", "C-GND"),
        **{("DRIVER", n): ("drv8833", f"C-{n}") for n in ("AIN1", "AIN2", "nSLEEP", "nFAULT")},
        **{("DRIVER", n): ("drv8833", f"P-{n}") for n in ("AOUT1", "AOUT2")},
        **{("MOTOR", str(k)): ("motor", str(k)) for k in range(1, 7)},
        **{(b, sig): (r, HALL_HOLE[sig]) for b, r in HALL_PARTS.items() for sig in HALL_HOLE},
        ("BATTERY", "+"): ("battery", "+"),
        ("BATTERY", "-"): ("battery", "-"),
    },
    # The battery's - and the door switch's ground land in the carrier's power-row GND.
    extra={"GND": [("drv8833", "P-GND")]},
    # The DevKit runs from USB and its 3.3 V is always on, so the encoder is too.
    aliases={"ENC_VCC": "+3V3"},
    ties=[(("drv8833", "P-GND"), ("drv8833", "C-GND")),
          tuple(("devkit", f"{hdr}-{k + 1}") for hdr, pins in (("J1", DEVKITM_J1), ("J3", DEVKITM_J3))
                for k, n in enumerate(pins) if n == "G")],
)
