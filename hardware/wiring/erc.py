"""Electrical rule check for every stage's net list: catches shorts, fights and over-voltage
before anything is wired.

Each part pin gets an electrical model (`E`) from its datasheet: a source (sets its net's
voltage), a follower (passes another pin's voltage through, like a switched FET), a ground, a
supply input (with the range it needs), a logic input, a GPIO, an open-drain output, a
push-pull output, a resistor terminal, or passive. The checker works out each net's voltage
range, then applies RULES:

| Rule | Catches |
|---|---|
| one-driver | two supplies or push-pull outputs on one net (they fight) |
| short-to-ground | a supply or output on the GND net |
| over-voltage | a net above the maximum of any pin on it (5 V on a 3.3 V GPIO, the pack on logic VCC) |
| under-voltage | a supply pin below its minimum (an A3144 on 3.3 V), or with no source at all |
| pull-up | an open-drain output with nothing pulling its net up (no resistor, no GPIO pull-up) |
| ground | a part with a supply but no ground, or a ground pin on another net |
| strapping | GPIO 8 or 9 pulled low or driven by another part at boot (wrong boot mode) |

Net voltages: sources and followers set a net's range ("strong"); a resistor from a strong net
pulls a net without a source towards that voltage ("weak": pull-ups, pull-downs); a push-pull
output swings between ground and its own supply's voltage. A net the inference can't handle
(a divider) gets a declared range with its reason, in that stage's Config.levels.

The limits below are absolute maxima or recommended ranges from the parts' datasheets (sources
in each model). They describe the parts as specified: if your board's silkscreen differs from
the order a model assumes, this check can't see it.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

from concepts.parts import DEVKITM_J1, DEVKITM_J3

from .netlist import DEVKIT, Pin, Stage, devkit_pin_name

GND_NET = "GND"
STRAPS_HIGH = {8: "GPIO8 must be 1 for download mode", 9: "GPIO9 must be 1 for SPI boot"}


@dataclass(frozen=True)
class E:
    kind: str  # source follow gnd supply in io od out res passive nc
    lo: float = 0.0  # source: the voltage it makes; supply: the minimum it needs
    hi: float = 0.0  # source: the voltage it makes; others: the maximum the pin takes
    of: str = ""  # follow: the pin it passes through; out: the pin whose net is its supply
    gpio: int | None = None


def SRC(lo, hi): return E("source", lo, hi)
def SUP(lo, hi): return E("supply", lo, hi)
def IN(hi): return E("in", hi=hi)
def OD(hi): return E("od", hi=hi)
def OUT(of): return E("out", of=of)
def FOL(of, hi=1e9): return E("follow", hi=hi, of=of)
def IO(gpio, hi=3.6): return E("io", hi=hi, gpio=gpio)


GND, PAS, NC, RES = E("gnd"), E("passive"), E("nc"), E("res")


# --- part models: pin name -> E ---------------------------------------------------------------

def espressif_devkit(j1: list[str], j3: list[str]) -> Callable[[str], E]:
    """An ESP32-C6 DevKit (M-1 or C-1): 3V3 from its LDO; 5V is USB VBUS behind a diode; GPIOs
    take VDD + 0.3 = 3.6 V (ESP32-C6 datasheet, absolute maximum ratings)."""
    def model(pin: str) -> E:
        name = devkit_pin_name(pin, j1, j3)
        return {"3V3": SRC(3.2, 3.4), "5V": SRC(4.7, 5.25), "G": GND, "RST": IN(3.6), "NC": NC,
                "TX": IO(16), "RX": IO(17)}.get(name) or IO(int(name))
    return model


devkitm1 = espressif_devkit(DEVKITM_J1, DEVKITM_J3)


def pololu_2130(pin: str) -> E:
    """Pololu DRV8833 carrier: VIN 2.7-10.8 V (after its reverse-protection FET; VMM is that
    FET's output); logic inputs and nFAULT -0.5 to 7 V (DRV8833 SLVSAR1E abs. max.)."""
    name = pin.split("-", 1)[1]
    if name == "GND":
        return GND
    if name == "VIN":
        return SUP(2.7, 10.8)
    if name == "VMM":
        return FOL("P-VIN")
    if name in ("AOUT1", "AOUT2", "BOUT1", "BOUT2"):
        return OUT("P-VIN")
    if name in ("AISEN", "BISEN"):
        return PAS
    return OD(7.0) if name == "nFAULT" else IN(7.0)


def drv5032_adapter(pin: str) -> E:
    """DRV5032FA on a SOT-23 adapter, holes 1 VCC, 3 OUT, 5 GND (assumed): VCC 1.65-5.5 V,
    push-pull OUT (SLVSDC7H)."""
    return {"1": SUP(1.65, 5.5), "3": OUT("1"), "5": GND}.get(pin, NC)


def pololu_motor(pin: str) -> E:
    """Pololu micro metal gearmotor + encoder, connector pins 1 GND, 2 B, 3 A, 4 VCC
    (2.7-18 V), 5 M2, 6 M1. A and B have 10k pull-ups to VCC on the encoder board."""
    return {"1": GND, "2": OUT("4"), "3": OUT("4"), "4": SUP(2.7, 18.0)}.get(pin, PAS)


def pack_4aa(pin: str) -> E:
    """4x AA lithium (Energizer L91): 1.0-1.8 V per cell."""
    return SRC(4.0, 7.2) if pin == "+" else GND


def tb6612(pin: str) -> E:
    """TB6612FNG (Toshiba datasheet): VM 2.5-13.5 V, VCC 2.7-5.5 V, inputs -0.2 to 6 V
    (abs. max.); AO/BO outputs swing to VM."""
    name = pin.split("-", 1)[1]
    if name.startswith("GND"):
        return GND
    return {"VM": SUP(2.5, 13.5), "VCC": SUP(2.7, 5.5)}.get(name) or \
        (OUT("P-VM") if name[:2] in ("AO", "BO") else IN(6.0))


def a3144(pin: str) -> E:
    """A3144 (Allegro A3144E): VCC 4.5-24 V, open-collector output, off-state up to 24 V."""
    return {"1": SUP(4.5, 24.0), "2": GND, "3": OD(24.0)}[pin]


def mp1584(pin: str) -> E:
    """MP1584EN module: input 4.5-28 V; output set by its trimmer (6.0 V here); grounds common."""
    return {"IN+": SUP(4.5, 28.0), "IN-": GND, "OUT+": SRC(5.9, 6.1), "OUT-": GND}[pin]


def pack_2s(pin: str) -> E:
    """2x 18650 Li-ion in series: 3.0-4.2 V per cell (stop at 3.0)."""
    return SRC(6.0, 8.4) if pin == "+" else GND


def mini1(pin: str) -> E:
    """ESP32-C6-MINI-1 pads: 3V3 3.0-3.6 V; GPIOs and EN take 3.6 V."""
    from .product import MINI1, MINI1_GND, MINI1_NC
    if pin in MINI1_GND:
        return GND
    if pin in MINI1_NC:
        return NC
    name = next(n for n, p in MINI1.items() if p == pin)
    return {"3V3": SUP(3.0, 3.6), "EN": IN(3.6), "TXD0": IO(16), "RXD0": IO(17)}.get(name) or IO(int(name[4:]))


def drv8833(pin: str) -> E:
    """DRV8833PWP: VM 2.7-10.8 V; logic inputs and nFAULT 7 V max; VINT is its own 3.3 V."""
    from .product import DRV
    name = next(n for n, p in DRV.items() if p == pin)
    if name in ("GND", "PAD"):
        return GND
    return {"VM": SUP(2.7, 10.8), "VINT": SRC(3.0, 3.6), "nFAULT": OD(7.0), "VCP": PAS, "AISEN": PAS,
            "BISEN": PAS}.get(name) or (OUT("12") if "OUT" in name else IN(7.0))


def drv5032(pin: str) -> E:
    return {"1": SUP(1.65, 5.5), "2": OUT("1"), "3": GND}[pin]


def tps629206(pin: str) -> E:
    """TPS629206: VIN 3-17 V, EN may tie to VIN; VOS senses the 3.3 V it regulates, so the
    net VOS is on is the regulator's output; SW swings to VIN."""
    return {"6": SUP(3.0, 17.0), "7": IN(17.0), "8": PAS, "2": OD(17.0), "3": SRC(3.25, 3.35),
            "4": OUT("6"), "5": GND, "1": NC}[pin]


def ao3401a(follows: str, out: str) -> Callable[[str], E]:
    """P-FET (AO3401A): VGS +-12 V. Used as a switch, its `out` pin passes the `follows` pin's
    voltage through."""
    return lambda pin: {"1": IN(12.0), out: FOL(follows)}.get(pin, PAS)


def n7002(pin: str) -> E:
    """2N7002: VGS +-20 V, VDS 60 V."""
    return IN(20.0) if pin == "1" else PAS


# --- what each stage's parts are --------------------------------------------------------------

@dataclass
class Config:
    models: dict[str, Callable[[str], E]]
    pullups: set[Pin] = field(default_factory=set)  # GPIO internal pull-ups the firmware must enable
    levels: dict[str, tuple[float, float, str]] = field(default_factory=dict)  # declared net ranges


def _gpio_pin(stage: Stage, gpio: int) -> Pin:
    return stage.blocks[("MCU", f"GPIO{gpio}")]


def configs() -> dict[str, Config]:
    from . import bench
    from .product import SPECS
    halls = ("hall_locked", "hall_unlocked", "hall_door")
    product = {}
    for s in SPECS:
        ref = s.ref
        product[ref] = (
            mini1 if ref == "U1" else drv8833 if ref == "U2" else tps629206 if ref == "U6"
            else drv5032 if s.lib_id == "lochness:DRV5032FA"
            else ao3401a("3", "2") if ref == "Q1"  # reverse protection: source follows the cells
            else ao3401a("2", "3") if ref == "Q2"  # encoder switch: drain follows 3.3 V
            else n7002 if s.lib_id.endswith("2N7002")
            else pololu_motor if ref == "J1"
            else (lambda pin: SRC(4.0, 7.2) if pin == "1" else GND) if ref == "J2"
            else (lambda pin: RES) if s.lib_id == "Device:R"
            else (lambda pin: PAS))
    return {
        "bench": Config(
            models={"devkit": espressif_devkit(bench.DEVKITC_J1, bench.DEVKITC_J3), "tb6612": tb6612, **{h: a3144 for h in halls}, "motor": lambda p: PAS,
                    "buck": mp1584, "battery": pack_2s},
            pullups={_gpio_pin(bench.BENCH, g) for g in (22, 23, 2)}),
        "devkit": Config(
            models={"devkit": devkitm1, "drv8833": pololu_2130, **{h: drv5032_adapter for h in halls},
                    "motor": pololu_motor, "battery": pack_4aa},
            pullups={_gpio_pin(DEVKIT, 3)}),
        "product": Config(
            models=product,
            levels={"VBAT_SENSE": (0.0, 7.2 * 3.3 / (4.7 + 3.3), "R1/R2 divider: 7.2 V x 3.3/8.0")}),
    }


# --- the checker ------------------------------------------------------------------------------

@dataclass(frozen=True)
class Finding:
    rule: str
    net: str
    message: str

    def __str__(self):
        return f"[{self.rule}] {self.net}: {self.message}"


def check(nets: dict[str, list[Pin]], cfg: Config) -> list[Finding]:
    model = {p: cfg.models[p[0]](p[1]) for pins in nets.values() for p in pins}
    net_of = {p: n for n, pins in nets.items() for p in pins}
    out: list[Finding] = []

    def name(p: Pin) -> str:
        return f"{p[0]}.{p[1]}"

    # strong ranges: sources, then followers (which may chain)
    strong: dict[str, tuple[float, float]] = {}
    for n, pins in nets.items():
        src = [model[p] for p in pins if model[p].kind == "source"]
        if src:
            strong[n] = (min(e.lo for e in src), max(e.hi for e in src))
    for _ in range(len(nets)):
        for p, e in model.items():
            if e.kind == "follow" and net_of.get((p[0], e.of)) in strong and net_of[p] not in strong:
                strong[net_of[p]] = strong[net_of[(p[0], e.of)]]
    for n, (lo, hi, _) in cfg.levels.items():
        strong[n] = (lo, hi)
    if GND_NET in nets:
        strong.setdefault(GND_NET, (0.0, 0.0))  # the reference: what pull-downs pull to
    # weak: through resistors, from a strong net to one without
    weak: dict[str, list[tuple[float, float]]] = {}
    for p, e in model.items():
        if e.kind == "res":
            other = next((q for q in model if q[0] == p[0] and q != p), None)
            if other and net_of[other] in strong and net_of[p] not in strong:
                weak.setdefault(net_of[p], []).append(strong[net_of[other]])
    for p in cfg.pullups:
        if p in net_of and net_of[p] not in strong:
            weak.setdefault(net_of[p], []).append((3.3, 3.3))

    def hi_of(n: str) -> float | None:
        vals = [strong[n][1]] if n in strong else [w[1] for w in weak.get(n, [])]
        for p in nets[n]:  # push-pull outputs swing up to their own supply
            e = model[p]
            if e.kind == "out" and net_of.get((p[0], e.of)) in strong:
                vals.append(strong[net_of[(p[0], e.of)]][1])
        return max(vals) if vals else None

    for n, pins in nets.items():
        drivers = [p for p in pins if model[p].kind in ("source", "follow", "out")]
        if len(drivers) > 1:
            out.append(Finding("one-driver", n, "driven by " + ", ".join(map(name, drivers))))
        if n == GND_NET and drivers:
            out.append(Finding("short-to-ground", n, "has " + ", ".join(map(name, drivers)) + " on it"))
        hi = hi_of(n)
        for p in pins:
            e = model[p]
            if hi is not None and e.kind in ("supply", "in", "io", "od") and hi > e.hi + 1e-9:
                out.append(Finding("over-voltage", n, f"up to {hi:g} V, but {name(p)} takes {e.hi:g} V max"))
            if e.kind == "supply":
                if n not in strong:
                    out.append(Finding("under-voltage", n, f"{name(p)} needs power but nothing supplies the net"))
                elif strong[n][0] < e.lo - 1e-9:
                    out.append(Finding("under-voltage", n, f"down to {strong[n][0]:g} V, but {name(p)} needs {e.lo:g} V"))
            if e.kind == "gnd" and n != GND_NET:
                out.append(Finding("ground", n, f"{name(p)} is a ground pin on {n}"))
        if any(model[p].kind == "od" for p in pins) and n not in strong:
            if not any(w[1] > 0 for w in weak.get(n, [])):
                out.append(Finding("pull-up", n, "open-drain output with nothing pulling the net up"))
        for p in pins:
            e = model[p]
            if e.kind == "io" and e.gpio in STRAPS_HIGH:
                others = [q for q in drivers if q[0] != p[0]]
                low = weak.get(n) and all(w[1] == 0 for w in weak[n])
                if others or low or (n in strong and strong[n][0] == 0) or n == GND_NET:
                    out.append(Finding("strapping", n, f"{STRAPS_HIGH[e.gpio]}; "
                                       + ("driven by " + ", ".join(map(name, others)) if others else "pulled low")))
    parts = {p[0] for p in model}
    for ref in sorted(parts):
        pins = [p for p in model if p[0] == ref]
        if any(model[p].kind == "supply" for p in pins) and not any(
                model[p].kind == "gnd" and net_of[p] == GND_NET for p in pins):
            out.append(Finding("ground", ref, "has a supply but no ground"))
    return out


def check_stage(stage: Stage, cfg: Config | None = None) -> list[Finding]:
    return check(stage.nets(), cfg or configs()[stage.key])


def firmware_requirements(stage: Stage) -> list[str]:
    """What the firmware must do for the wiring to be safe and work: the internal pull-ups."""
    cfg = configs()[stage.key]
    gpio = {pin: g for (blk, b), pin in stage.blocks.items() if blk == "MCU" and b.startswith("GPIO")
            for g in [int(b[4:])]}
    return [f"enable the internal pull-up on GPIO {gpio[p]}" for p in sorted(cfg.pullups, key=lambda p: gpio[p])]
