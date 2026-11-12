"""Stage 3: the product's main PCB (concept B, the slab), as a Stage of the net list.

The function blocks (MCU, driver, motor, Hall switches, battery) are the same circuit as the
dev kit's (wiring/netlist.py CORE); this adds what the breakouts used to provide (the
regulator, decoupling, pull-ups, the DRV8833's charge pump), and what only the product needs
(reverse-battery protection, a switch for the encoder's supply, a battery-voltage divider,
the status LED, the pairing button, the 32 kHz crystal and a programming header).

Each support part says why it is there and where its value comes from. Datasheet sections
are from: TPS629206 SLVSGE1, DRV8833 SLVSAR1E, DRV5032 SLVSDC7H, ESP32-C6-MINI-1 datasheet
v1.5 (Fig. 9-1) and the ESP32-C6 hardware design guidelines (HDG).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .netlist import HALL_BLOCKS, PIN_MAP, Part, Stage

# GPIOs only the product uses. The dev kit has its own BOOT button on GPIO 9 and an RGB LED
# on GPIO 8; it doesn't switch the encoder or measure the cells.
PRODUCT_PINS: dict[str, tuple[int, str]] = {
    "XTAL_32K_P": (0, "32.768 kHz crystal (sleep clock for Thread)"),
    "XTAL_32K_N": (1, "32.768 kHz crystal"),
    "ENC_nEN": (5, "Encoder power, active low (strapping pin MTDI: only read in SDIO boot)"),
    "VBAT_SENSE": (6, "Battery voltage, ADC1 channel 6, through a 4.7M/3.3M divider"),
    "LED_R": (7, "Status LED red (drives an N-FET)"),
    "LED_G": (14, "Status LED green"),
    "LED_B": (15, "Status LED blue (strapping pin: ignored unless the JTAG-select eFuse is burned)"),
    "BOOT_BTN": (9, "Pairing / reset button, and BOOT (download mode if held at reset)"),
    "IO8_PU": (8, "Strapping pin: pulled up, so download mode works"),
    "USB_DM": (12, "USB D- (programming header)"),
    "USB_DP": (13, "USB D+ (programming header)"),
}

# ESP32-C6-MINI-1 pad numbers (KiCad RF_Module:ESP32-C6-MINI-1, from the module datasheet).
MINI1 = {"3V3": "3", "EN": "8", "TXD0": "31", "RXD0": "30"} | {f"GPIO{g}": p for g, p in {
    0: "12", 1: "13", 2: "5", 3: "6", 4: "9", 5: "10", 6: "15", 7: "16", 8: "22", 9: "23", 12: "17",
    13: "18", 14: "19", 15: "20", 18: "24", 19: "25", 20: "26", 21: "27", 22: "28", 23: "29"}.items()}
MINI1_GND = ["1", "2", "11", "14"] + [str(n) for n in range(36, 54)]
MINI1_NC = ["4", "7", "21", "32", "33", "34", "35"]
# DRV8833PWP (HTSSOP-16 + pad 17).
DRV = {"nSLEEP": "1", "AOUT1": "2", "AISEN": "3", "AOUT2": "4", "BOUT2": "5", "BISEN": "6", "BOUT1": "7",
       "nFAULT": "8", "BIN1": "9", "BIN2": "10", "VCP": "11", "VM": "12", "GND": "13", "VINT": "14",
       "AIN2": "15", "AIN1": "16", "PAD": "17"}
# TPS629206 DRL (SOT-583), datasheet Table 6-1.
TPS = {"FB/VSET": "1", "PG": "2", "VOS": "3", "SW": "4", "GND": "5", "VIN": "6", "EN": "7", "MODE/S-CONF": "8"}
# DRV5032 DBZ (SOT-23), datasheet Table 5-1.
HALL = {"VCC": "1", "OUT": "2", "GND": "3"}

FP = {
    "0402R": "Resistor_SMD:R_0402_1005Metric", "0402": "Capacitor_SMD:C_0402_1005Metric",
    "0805": "Capacitor_SMD:C_0805_2012Metric", "1206": "Capacitor_SMD:C_1206_3216Metric",
    "SOT-23": "Package_TO_SOT_SMD:SOT-23",
}


@dataclass
class Spec:
    ref: str
    lib_id: str
    value: str
    footprint: str
    x: float
    y: float
    nets: dict[str, str] = field(default_factory=dict)  # this part's own connections: pin -> net
    why: str = ""
    mpn: str = ""
    rot: int = 0
    pins: list[str] = field(default_factory=list)  # every pin, when not just nets' keys


def R(ref, value, a, b, x, y, why, mpn="", fp="0402R"):
    return Spec(ref, "Device:R", value, FP[fp], x, y, {"1": a, "2": b}, why, mpn)


def C(ref, value, a, b, x, y, why, mpn="", fp="0402"):
    return Spec(ref, "Device:C", value, FP[fp], x, y, {"1": a, "2": b}, why, mpn)


# Sheet columns (A2, mm). Every coordinate is on the 1.27 mm grid.
def g(v: float) -> float:
    return round(v / 1.27) * 1.27


SPECS: list[Spec] = [
    # --- battery in: reverse protection, bulk, voltage sense ---
    Spec("J2", "Connector_Generic:Conn_01x02", "Battery", "Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal",
         g(40), g(60), {"1": "VBAT_IN"}, "4xAA lead (JST PH). UNCONFIRMED: check the lead's red wire is on pin 1",
         "S2B-PH-K-S", pins=["1", "2"]),
    Spec("Q1", "Transistor_FET:AO3401A", "AO3401A", FP["SOT-23"], g(70), g(60), {"1": "GND", "3": "VBAT_IN"},
         "Reverse-battery protection: P-FET, drain to the cells, gate to GND. On (47 mOhm) when the "
         "cells are the right way round; VGS -7.2 V max vs its +-12 V rating", "AO3401A", pins=["1", "2", "3"]),
    C("C1", "100u 16V", "VBAT", "GND", g(85), g(60), "Bulk for motor start and stall (0.75 A) on aging cells "
      "(DRV8833 9.1: system-level; size on the dev kit)", "EEE-FT1C101AR", fp="1206"),
    R("R1", "4.7M 1%", "VBAT", "VBAT_SENSE", g(100), g(55), "Battery divider top: 7.2 V -> 2.97 V at the ADC, 0.9 uA"),
    R("R2", "3.3M 1%", "VBAT_SENSE", "GND", g(110), g(55), "Battery divider bottom"),
    C("C2", "100n", "VBAT_SENSE", "GND", g(120), g(55), "ADC filter cap (HDG: 0.1 uF on ADC pins); holds the "
      "divider node up while the ADC samples"),

    # --- 3.3 V buck ---
    Spec("U6", "lochness:TPS629206", "TPS629206DRLR", "Package_TO_SOT_SMD:SOT-583-8", g(60), g(120),
         {TPS["VIN"]: "VBAT", TPS["EN"]: "VBAT", TPS["MODE/S-CONF"]: "TPS_MODE",
          TPS["SW"]: "TPS_SW", TPS["VOS"]: "+3V3", TPS["GND"]: "GND"},
         "3.3 V buck, 3-17 V in, 4 uA. FB/VSET open = 3.3 V (Table 8-2); EN tied to VIN at C3 (8.3.4); "
         "PG unused, left open (8.3.5)", "TPS629206DRLR", pins=list(TPS.values())),
    C("C3", "4.7u 25V", "VBAT", "GND", g(30), g(145), "Buck input (7.3: 3 uF effective min; TI's C3216X7R1E475K160AC)",
      "C3216X7R1E475K160AC", fp="1206"),
    R("R3", "27.4k 1%", "TPS_MODE", "GND", g(50), g(145), "MODE/S-CONF: VSET mode, auto PFM/PWM with AEE, "
      "output discharge on (Table 8-1); read once at start-up"),
    Spec("L1", "Device:L", "2.2u", "Inductor_SMD:L_1008_2520Metric", g(90), g(115), {"1": "TPS_SW", "2": "+3V3"},
         "Buck inductor 2.2 uH (9.2.2.3.2). Murata DFE252012PD-2R2M (2.5 x 2.0): CHECK the land pattern "
         "against Murata's; Coilcraft XGL3530-222ME is the other option", "DFE252012PD-2R2M#"),
    C("C4", "22u 6.3V", "+3V3", "GND", g(105), g(125), "Buck output (7.3: 10-100 uF effective; GCM21BD70J226ME36L)",
      "GCM21BD70J226ME36L", fp="0805"),

    # --- ESP32-C6-MINI-1 ---
    Spec("U1", "RF_Module:ESP32-C6-MINI-1", "ESP32-C6-MINI-1-N4", "RF_Module:ESP32-C6-MINI-1", g(230), g(110),
         {**{p: "GND" for p in MINI1_GND}, MINI1["TXD0"]: "UART_TX", MINI1["RXD0"]: "UART_RX",
          MINI1["EN"]: "CHIP_EN"}, "Thread + BLE module. Antenna end over the board edge or a copper keep-out",
         "ESP32-C6-MINI-1-N4", pins=[str(n) for n in range(1, 54)]),
    C("C5", "22u 6.3V", "+3V3", "GND", g(180), g(60), "Module 3V3 bulk (module Fig. 9-1: 22 uF + 0.1 uF)",
      "GCM21BD70J226ME36L", fp="0805"),
    C("C6", "100n", "+3V3", "GND", g(190), g(60), "Module 3V3 bypass, at pin 3"),
    R("R4", "10k", "+3V3", "CHIP_EN", g(160), g(60), "EN pull-up (module Fig. 9-1: 10k + 1 uF RC delay)"),
    C("C7", "1u", "CHIP_EN", "GND", g(170), g(60), "EN delay cap"),
    R("R5", "10k", "+3V3", "IO8_PU", g(150), g(160), "GPIO8 strap pull-up: download mode needs GPIO8 = 1 "
      "(module Fig. 9-1 R8; HDG Table 4)"),
    R("R6", "10k", "+3V3", "BOOT_BTN", g(160), g(160), "GPIO9 pull-up (internal is weak): SPI boot unless the button is held"),
    Spec("SW1", "Switch:SW_Push", "Pair", "Button_Switch_SMD:SW_SPST_PTS810", g(175), g(165),
         {"1": "BOOT_BTN", "2": "GND"}, "Pairing / factory-reset button; held at reset = download mode",
         "PTS810SJM250SMTR LFS"),
    Spec("Y1", "Device:Crystal", "32.768k", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", g(155), g(85),
         {"1": "XTAL_32K_P", "2": "XTAL_32K_N"}, "Sleep clock: better timing, lower average current in "
         "sleep (HDG 1.3.5). ESR <= 70k; CL 7 pF part", "ABS07-32.768KHZ-7-T"),
    C("C8", "12p", "XTAL_32K_P", "GND", g(140), g(100), "Crystal load (module Fig. 9-1: 12 pF); tune to the "
      "crystal's CL and the board's stray capacitance"),
    C("C9", "12p", "XTAL_32K_N", "GND", g(170), g(100), "Crystal load"),
    Spec("J3", "Connector_Generic:Conn_01x08", "Prog", "Connector_PinHeader_1.27mm:PinHeader_1x08_P1.27mm_Vertical",
         g(300), g(60), {"1": "+3V3", "2": "GND", "3": "USB_DM", "4": "USB_DP", "5": "CHIP_EN",
                         "6": "BOOT_BTN", "7": "UART_TX", "8": "UART_RX"},
         "Programming / debug: USB Serial-JTAG on D-/D+, EN, BOOT, UART0. Pads for pogo pins or a 1.27 header"),

    # --- motor driver, motor connector, encoder supply switch ---
    Spec("U2", "Driver_Motor:DRV8833PWP", "DRV8833PWPR", "Package_SO:HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3.4x5mm_Mask2.46x2.31mm_ThermalVias",
         g(370), g(110), {DRV["VCP"]: "DRV_VCP", DRV["VINT"]: "DRV_VINT", DRV["AISEN"]: "GND",
                           DRV["BISEN"]: "GND", DRV["BIN1"]: "GND", DRV["BIN2"]: "GND", DRV["PAD"]: "GND"},
         "Bridge A drives the motor; bridge B unused (inputs to GND, outputs open). xISEN to GND: no "
         "current limit (7.3.3); stalls are seen by the encoder", "DRV8833PWPR", pins=list(DRV.values())),
    C("C10", "10u 25V", "VBAT", "GND", g(345), g(60), "VM bypass, at pin 12 (10 uF ceramic, 10.1)", fp="1206"),
    C("C11", "10n 16V", "DRV_VCP", "VBAT", g(360), g(60), "Charge pump: 0.01 uF X7R 16 V, VCP to VM (pin table)"),
    C("C12", "2.2u 6.3V", "DRV_VINT", "GND", g(375), g(60), "Internal regulator: 2.2 uF 6.3 V to GND (pin table)"),
    R("R7", "10k", "+3V3", "DRV_nFAULT", g(390), g(60), "nFAULT is open drain (6.5)"),
    Spec("J1", "Connector_Generic:Conn_01x06", "Motor", "Connector_JST:JST_SH_SM06B-SRSS-TB_1x06-1MP_P1.00mm_Horizontal",
         g(450), g(110), {}, "Pololu #5193 cable: 1 enc GND, 2 B, 3 A, 4 VCC, 5 M2, 6 M1. CHECK the "
         "Pololu SH-style plug in a genuine SM06B-SRSS-TB", "SM06B-SRSS-TB", pins=list("123456")),
    Spec("Q2", "Transistor_FET:AO3401A", "AO3401A", FP["SOT-23"], g(430), g(160),
         {"1": "ENC_nEN", "2": "+3V3", "3": "ENC_VCC"}, "Encoder supply switch: the encoder only needs "
         "power while the motor turns. Its outputs have 10k pull-ups to its VCC, so 3.3 V keeps them "
         "safe for the ESP32", "AO3401A", pins=["1", "2", "3"]),
    R("R8", "100k", "+3V3", "ENC_nEN", g(405), g(160), "Holds the encoder off through reset and sleep"),
    C("C13", "100n", "ENC_VCC", "GND", g(460), g(160), "Encoder supply bypass, at J1"),

    # --- Hall switches (bolt locked and unlocked under the board, door by the latch edge) ---
    *[s for k, b in enumerate(HALL_BLOCKS) for s in (
        Spec(f"U{3 + k}", "lochness:DRV5032FA", "DRV5032FADBZR", FP["SOT-23"], g(70 + 60 * k), g(230), {},
             f"{b.replace('HALL_', '').title()} switch: omnipolar, push-pull, 20 Hz, 1.6 uA", "DRV5032FADBZR",
             pins=list(HALL.values())),
        C(f"C{14 + k}", "100n", "+3V3", "GND", g(95 + 60 * k), g(230), "Hall switch bypass (>= 0.1 uF, 8.4)"))],

    # --- status LED: common anode on the cells, cathodes switched by N-FETs ---
    Spec("D1", "Device:LED_ARGB", "APTF1616SEEZGKQBKC", "", g(290), g(220),
         {"1": "VBAT", "2": "LED_RK", "3": "LED_GK", "4": "LED_BK"},
         "RGB, common anode. Green and blue need 3.3 V (4.1 max) so the anode is on the cells, not "
         "3.3 V. NO FOOTPRINT YET: draw Kingbright's 1.6 x 1.6 land pattern", "APTF1616SEEZGKQBKC"),
    *[s for k, (c, gpio, r, v) in enumerate((("R", 7, "820", "2.0"), ("G", 14, "560", "3.3"), ("B", 15, "560", "3.3")))
      for s in (
        R(f"R{9 + k}", r, f"LED_{c}K", f"LED_{c}_D", g(320 + 30 * k), g(205),
          f"{c} current: (6 V - {v} V) / {r} ~ {round((6 - float(v)) / int(r) * 1000, 1)} mA at a fresh-ish 6 V"),
        Spec(f"Q{3 + k}", "Transistor_FET:2N7002", "2N7002", FP["SOT-23"], g(325 + 30 * k), g(235),
             {"1": f"LED_{c}", "2": "GND", "3": f"LED_{c}_D"}, f"Switches the {c} cathode", "2N7002",
             pins=["1", "2", "3"]),
        R(f"R{12 + k}", "100k", f"LED_{c}", "GND", g(310 + 30 * k), g(250),
          f"Holds the {c} FET off through reset"))],
]

PARTS = {s.ref: Part(s.ref, s.value, s.pins or list(s.nets)) for s in SPECS}

PRODUCT = Stage(
    key="product", title="Stage 3: product main PCB",
    parts=PARTS,
    blocks={
        ("MCU", "3V3"): ("U1", MINI1["3V3"]),
        ("MCU", "GND"): ("U1", "1"),
        **{("MCU", f"GPIO{g_}"): ("U1", MINI1[f"GPIO{g_}"]) for g_, _, _ in PIN_MAP.values()},
        **{("DRIVER", n): ("U2", DRV[n]) for n in ("VM", "GND", "AIN1", "AIN2", "nSLEEP", "nFAULT", "AOUT1", "AOUT2")},
        **{("MOTOR", str(k)): ("J1", str(k)) for k in range(1, 7)},
        **{(b, sig): (f"U{3 + k}", HALL[sig]) for k, b in enumerate(HALL_BLOCKS) for sig in HALL},
        # The battery as the circuit sees it: + after the reverse-protection FET (its source).
        ("BATTERY", "+"): ("Q1", "2"),
        ("BATTERY", "-"): ("J2", "2"),
    },
)
for s in SPECS:
    for pin, net in s.nets.items():
        PRODUCT.extra.setdefault(net, []).append((s.ref, pin))
for net, (gpio, _) in PRODUCT_PINS.items():
    pad = MINI1[f"GPIO{gpio}"]
    PRODUCT.extra.setdefault(net, []).append(("U1", pad))

# Nets with a power symbol on the sheet (the rest get labels), and the ones needing a
# PWR_FLAG because nothing on them is a power output as KiCad sees it.
POWER_SYMBOLS = {"GND": "power:GND", "+3V3": "power:+3V3", "VBAT": "power:+BATT"}
FLAGGED = ["GND", "+3V3", "VBAT", "DRV_VINT"]
