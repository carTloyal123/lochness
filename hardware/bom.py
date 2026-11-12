"""Costed bills of materials: one CSV per assembly, every line from the model plus the parts
the model doesn't draw (cables, headers, passives, the door-frame magnet).

    uv run bom.py          # -> bom/<assembly>.csv

Prices are US, quantity 1 (or the per-piece price of the smallest common pack), checked on
the supplier's page on 2026-11-11 unless the "Price confirmed" column says otherwise.
Assumptions:
- Printed parts cost their filament: solid volume x 1.27 g/cm3 (PETG) x $0.02/g. That is an
  upper bound, since prints aren't solid.
- A product PCB, assembled, costs about what the dev boards it replaces cost: DevKitM-1 +
  DRV8833 carrier + three Hall adapters. The chips on it are listed for reference at $0
  ("in the PCB line"), and so are the passives, which aren't designed yet.
"""

from __future__ import annotations

import csv
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from concepts.kit import BOUGHT, CELL, CHIPS, FASTENER, METAL, MOTOR_DESC, PCB, PRINTED, WIRE, Unit

HERE = Path(__file__).resolve().parent
OUT = HERE / "bom"
PETG_G_PER_CM3, USD_PER_G = 1.27, 0.02


@dataclass(frozen=True)
class Buy:
    mfr: str
    mpn: str
    supplier: str
    sku: str
    usd: float  # per piece
    url: str = ""
    note: str = ""
    confirmed: bool = True
    on_pcb: bool = False  # cost is in the PCB line


DK = "https://www.digikey.com/en/products/detail/"
MC = "https://www.mcmaster.com/"


def _mc(pn: str, usd: float, note: str = "") -> Buy:
    return Buy("McMaster-Carr", pn, "McMaster-Carr", pn, usd, MC + pn,
               ("18-8 stainless; pack of 100, per-piece price estimated. " + note).strip(), False)


# What to buy for each model description (Item.desc).
CATALOG: dict[str, Buy] = {
    # Motors
    MOTOR_DESC["n20"]: Buy("Pololu", "5193", "Pololu", "5193", 32.45, "https://www.pololu.com/product/5193",
                           "Cable not included. $29.85 each at 5+."),
    MOTOR_DESC["n20-worm"]: Buy("Pololu", "5185", "Pololu", "5185", 32.45, "https://www.pololu.com/product/5185",
                                "Cable not included. $29.85 each at 5+."),
    # Chips on the product PCB
    CHIPS["mcu"][1]: Buy("Espressif", "ESP32-C6-MINI-1-N4", "DigiKey", "1965-ESP32-C6-MINI-1-N4CT-ND", 4.53,
                         DK + "espressif-systems/ESP32-C6-MINI-1-N4/17885796", on_pcb=True),
    CHIPS["driver"][1]: Buy("Texas Instruments", "DRV8833PWPR", "DigiKey", "296-29434-1-ND", 2.68,
                            DK + "texas-instruments/DRV8833PWPR/2743167", on_pcb=True),
    CHIPS["regulator"][1]: Buy("Texas Instruments", "TPS629206DRLR", "DigiKey", "296-TPS629206DRLRCT-ND", 1.08,
                               DK + "texas-instruments/TPS629206DRLR/16516686", on_pcb=True),
    CHIPS["motor_conn"][1]: Buy("JST", "SM06B-SRSS-TB", "DigiKey", "455-SM06B-SRSS-TBCT-ND", 0.67,
                                DK + "jst-sales-america-inc/SM06B-SRSS-TB/926712", on_pcb=True),
    **{CHIPS[h][1]: Buy("Texas Instruments", "DRV5032FADBZR", "DigiKey", "296-47765-1-ND", 0.33,
                        DK + "texas-instruments/DRV5032FADBZR/7400094", on_pcb=True)
       for h in ("hall_locked", "hall_unlocked", "hall_door")},
    "ESP32-C6 QFN40 5 x 5 (chip-down: the same chip as the MINI-1 module)":
        Buy("Espressif", "ESP32-C6FH4", "DigiKey", "1904-ESP32-C6FH4CT-ND", 2.06,
            DK + "espressif-systems/ESP32-C6FH4/18587545", "4 MB flash in the package.", on_pcb=True),
    "2.4 GHz chip antenna": Buy("Johanson Technology", "2450AT18A0100001E", "DigiKey",
                                "712-2450AT18A0100001ECT-ND", 0.51,
                                DK + "johanson-technology-inc/2450AT18A100E/1560676",
                                "Formerly 2450AT18A100E.", on_pcb=True),
    "Status RGB LED": Buy("Kingbright", "APTF1616SEEZGKQBKC", "DigiKey", "754-1977-1-ND", 0.92,
                          DK + "kingbright/APTF1616SEEZGKQBKC/5803669", "1.6 x 1.6 mm.", on_pcb=True),
    "Pairing / reset button": Buy("C&K", "PTS810SJM250SMTR LFS", "DigiKey", "CKN10502CT-ND", 0.53,
                                  DK + "c-k/PTS810SJM250SMTR-LFS/4176610", "Top-actuated, 4.2 x 3.2 mm.",
                                  on_pcb=True),
    "Battery connector": Buy("JST", "S2B-PH-K-S", "DigiKey", "455-1719-ND", 0.10,
                             DK + "jst-sales-america-inc/S2B-PH-K-S/926626", "2-pin PH, right angle.",
                             on_pcb=True),
    # Dev boards
    "Espressif ESP32-C6-DevKitM-1 (the product's ESP32-C6-MINI-1 module), on its pin headers":
        Buy("Espressif", "ESP32-C6-DEVKITM-1-N4", "DigiKey", "1965-ESP32-C6-DEVKITM-1-N4-ND", 8.00,
            DK + "espressif-systems/ESP32-C6-DEVKITM-1-N4/18667011",
            "Headers appear fitted in photos; the user guide doesn't say."),
    "Pololu #2130 DRV8833 dual motor driver carrier, on 2x 1x8 pin headers":
        Buy("Pololu", "2130", "Pololu", "2130", 10.95, "https://www.pololu.com/product/2130",
            "Headers included, unsoldered."),
    "SparkFun BOB-00717 SOT-23 adapter with a DRV5032FADBZR and 2x 1x3 header, worn chip-down":
        Buy("SparkFun", "BOB-00717", "SparkFun", "BOB-00717", 1.25, "https://www.sparkfun.com/products/717",
            "Adapter only: the chip and headers are separate lines. Page says 8.6 x 11.7 mm."),
    "Mini breadboard, 170 points (47 x 35 mm), adhesive back":
        Buy("SparkFun", "PRT-12043", "SparkFun", "PRT-12043", 5.25, "https://www.sparkfun.com/products/12043"),
    # Magnets and cells
    "NdFeB magnet 3 x 2 mm, axial (bolt position)":
        Buy("Apex Magnets", "M3x2MMDI", "Apex Magnets", "M3x2MMDI", 0.18,
            "https://www.apexmagnets.com/3mm-x-2mm-disc-neodymium-magnet", "N48. Pack of 50 for $8.99."),
    "AA 1.5 V lithium cell (Energizer L91)":
        Buy("Energizer", "L91", "Ace Hardware", "L91SBP-8", 3.37, "",
            "8-pack about $27; US range $19-40.", False),
    "CR123A 3 V lithium cell": Buy("Energizer", "EL123AP", "Home Depot", "EL123APB2", 4.80, "",
                                   "2-pack about $9.59.", False),
    # Metal
    "Back plate, 3 mm steel, laser cut, 2x M3 tapped":
        Buy("SendCutSend", "custom (0.119\" mild steel, 2x M3 tap)", "SendCutSend", "", 14.00,
            "https://sendcutsend.com", "Estimate: $12-16 per part; order minimum about $29.", False),
    "Battery contact, + flat with solder tab":
        Buy("Keystone", "5223", "DigiKey", "36-5223-ND", 0.17, DK + "keystone-electronics/5223/316374",
            "Fits AA and CR123A."),
    "Battery contact, - spring with solder tab":
        Buy("Keystone", "5201", "DigiKey", "36-5201-ND", 0.25, DK + "keystone-electronics/5201/316365",
            "Fits AA and CR123A."),
    "Battery link contact, + flat / - spring, nickel-plated steel":
        Buy("Keystone", "5212", "DigiKey", "36-5212-ND", 0.51, DK + "keystone-electronics/5212/316371",
            "AA dual contact for cells in series."),
    # Fasteners
    "Deadbolt through-screw, flat head (thread per brand, e.g. #8-32 x 1\")":
        Buy("(deadbolt maker)", "", "reused", "", 0.0, "", "Comes with the deadbolt; longer ones per brand if needed."),
    "Heat-set insert M2 x 3 (hole 3.2 mm)":
        Buy("CNC Kitchen", "Threaded Inserts M2 Standard", "3DJake", "", 0.111,
            "https://www.3djake.com/cnc-kitchen/threaded-inserts-m2-standard", "Pack of 100 for $11.12, ships from the EU."),
    "Heat-set insert M3 x 5.7 (hole 4 mm)":
        Buy("CNC Kitchen", "Heat Set Inserts M3 standard", "Prusa", "", 0.110,
            "https://www.prusa3d.com/en/product/heat-set-inserts-m3-standard-100-pcs/", "Pack of 100 for $10.99."),
    "M1.6 x 3 pan head (ISO 7045)": Buy("Pololu", "1954", "Pololu", "1954", 0.62,
                                        "https://www.pololu.com/product/1954", "Pack of 6 for $3.71."),
    "M1.6 x 4 pan head (ISO 7045)": _mc("92000A002", 0.07, "Phillips pan head."),
    "M2 x 6 button head hex socket": _mc("92095A453", 0.10, "ISO 7380."),
    "M2 x 8 button head hex socket": _mc("92095A454", 0.10, "ISO 7380."),
    "M2 x 16 button head hex socket": _mc("92095A111", 0.11, "ISO 7380."),
    "M2 x 20 button head hex socket": _mc("92095A106", 0.12, "ISO 7380."),
    "M3 x 5 button head hex socket (ISO 7380)": _mc("92095A177", 0.07),
    "M3 x 6 button head hex socket (ISO 7380)": _mc("92095A179", 0.07, "Part number confirmed."),
    "M3 x 8 button head hex socket (ISO 7380)": _mc("92095A181", 0.08),
    "M3 x 6 countersunk hex socket (ISO 10642)": _mc("92125A126", 0.08),
}

# Product PCB = the dev boards it replaces (see the module docstring).
_HALL_SET = 1.25 + 0.33  # adapter + chip
_HEADERS = 1.95  # one 40-pin strip covers the adapters
DEVBOARDS_USD = 8.00 + 10.95 + 3 * _HALL_SET + _HEADERS
_PCB_NOTE = ("Assembled. Priced as the dev boards it replaces (DevKitM-1 + Pololu 2130 + 3 Hall "
             "adapters with chips and headers) = ${:.2f}.")
PCBS: dict[str, Buy] = {
    "Main PCB, 1.6 mm FR4, bolt Hall switches on the underside":
        Buy("custom", "", "PCB fab + assembly", "", round(DEVBOARDS_USD, 2), "",
            _PCB_NOTE.format(DEVBOARDS_USD), False),
    "Main PCB, annular, 1.6 mm FR4 (chip-down ESP32-C6)":
        Buy("custom", "", "PCB fab + assembly", "", round(DEVBOARDS_USD - 2 * _HALL_SET, 2), "",
            _PCB_NOTE.format(DEVBOARDS_USD) + " Less the two bolt Hall sets, which are on the sensor board.",
            False),
    "Bolt-sensor board, 2x Hall switch, wired to the main PCB":
        Buy("custom", "", "PCB fab + assembly", "", round(2 * _HALL_SET, 2), "",
            "Priced as two Hall adapters with chips.", False),
}

# Parts the model doesn't draw: (qty, category, desc, Buy, required).
_PASSIVES = ("Passives: decoupling, buck inductor and caps, pull-ups, ESD (per schematic, not yet designed)",
             Buy("various", "", "", "", 0.0, "", "In the PCB line.", on_pcb=True))
_DOOR_MAGNET = ("Door-frame magnet for the door-position switch, NdFeB disc about 10 x 3 mm, axial, adhesive-backed",
                Buy("Apex Magnets", "", "Apex Magnets", "", 0.50, "",
                    "Not chosen yet: size it for the door gap against the DRV5032FA's 3.9 mT. Price estimated.",
                    False))
_PRODUCT_CABLE = ("Motor cable, 6-pin JST SH-style both ends, 10 cm",
                  Buy("Pololu", "4765", "Pololu", "4765", 3.32, "https://www.pololu.com/product/4765",
                      "Check the fit on a genuine JST SM06B-SRSS-TB. 16 cm is #4766 ($3.61)."))
_BATT_LEAD = ("Battery lead, JST PH 2-pin female, 100 mm (soldered to the end contacts)",
              Buy("Adafruit", "261", "Adafruit", "261", 0.75, "https://www.adafruit.com/product/261"))
_HOOKUP = ("Hookup wire, 26 AWG silicone, 2 m",
           Buy("Adafruit", "1881", "Adafruit", "1881", 0.95, "https://www.adafruit.com/product/1881"))
_PRODUCT_EXTRAS = [(1, BOUGHT, *_PRODUCT_CABLE, True), (1, BOUGHT, *_DOOR_MAGNET, True),
                   (1, BOUGHT, *_PASSIVES, True)]
EXTRAS: dict[str, list[tuple]] = {
    "A-puck": _PRODUCT_EXTRAS + [
        (1, BOUGHT, "40 MHz crystal for the chip-down ESP32-C6 (per schematic)",
         Buy("various", "", "", "", 0.0, "", "In the PCB line.", on_pcb=True), True),
        (2, WIRE, *_BATT_LEAD, True),
        (1, WIRE, "Hookup wire, 26 AWG silicone, 2 m (sensor board to main PCB)", _HOOKUP[1], True)],
    "B-slab": _PRODUCT_EXTRAS + [(1, WIRE, *_BATT_LEAD, True)],
    "C-pill": _PRODUCT_EXTRAS + [(1, WIRE, *_BATT_LEAD, True)],
    "devkit": [
        (3, BOUGHT, "DRV5032FADBZR Hall switch, SOT-23 (soldered to the adapters)",
         Buy("Texas Instruments", "DRV5032FADBZR", "DigiKey", "296-47765-1-ND", 0.33,
             DK + "texas-instruments/DRV5032FADBZR/7400094"), True),
        (1, BOUGHT, "Break-away male headers, 40-pin, 0.1\" (2x 1x3 per adapter)",
         Buy("SparkFun", "PRT-00116", "SparkFun", "PRT-00116", 1.95, "https://www.sparkfun.com/products/116"),
         True),
        (1, BOUGHT, "Motor cable, 6-pin JST SH-style to bare wires, 12 cm (tinned ends into the breadboard)",
         Buy("Pololu", "4762", "Pololu", "4762", 2.10, "https://www.pololu.com/product/4762",
             "Or a #4765 cable and a #4772 breakout ($1.49) for 0.1\" pins."), True),
        (1, BOUGHT, *_DOOR_MAGNET, True),
        (1, WIRE, "Jumper wire kit, 22 AWG solid, pre-bent, 140 pieces (covers the jumper wires)",
         Buy("SparkFun", "PRT-00124", "SparkFun", "PRT-00124", 8.95, "https://www.sparkfun.com/products/124"),
         True),
        (1, WIRE, "Hookup wire, 26 AWG silicone, 2 m (battery contacts to the breadboard)", _HOOKUP[1], True),
        (1, BOUGHT, "USB-C cable, data (powers and programs the DevKit)",
         Buy("any", "", "", "", 5.00, "", "Price estimated.", False), True),
    ],
}

COLUMNS = ["Category", "Qty", "Part", "Manufacturer", "MPN", "Supplier", "Supplier SKU", "Unit USD",
           "Extended USD", "Required", "In model", "Price confirmed", "URL", "Notes"]
CATEGORY = {PCB: "PCB", BOUGHT: "Bought", CELL: "Cell", METAL: "Metal", FASTENER: "Fastener",
            PRINTED: "Printed", WIRE: "Wire"}


def missing(u: Unit) -> list[str]:
    """Model descriptions with no catalog entry (printed parts and wires need none)."""
    return sorted({i.desc for i in u.items.values()
                   if i.kind not in (PRINTED, WIRE) and i.desc not in CATALOG and i.desc not in PCBS})


def rows(key: str, u: Unit) -> list[dict]:
    if gaps := missing(u):
        raise KeyError(f"{key}: no catalog entry for {gaps}")
    groups: dict[tuple[str, str], list] = defaultdict(list)
    for item in u.items.values():
        groups[(item.kind, item.desc)].append(item)
    out = []

    def line(cat, qty, desc, b: Buy | None, required=True, in_model=True, unit=None, note=""):
        unit = (0.0 if b.on_pcb else b.usd) if unit is None else unit
        if b and b.on_pcb:
            note = f"On the PCB; cost in the PCB line (${b.usd:.2f} each loose). {b.note}".strip()
        out.append({"Category": CATEGORY[cat], "Qty": qty, "Part": desc,
                    "Manufacturer": b.mfr if b else "printed", "MPN": b.mpn if b else "",
                    "Supplier": b.supplier if b else "", "Supplier SKU": b.sku if b else "",
                    "Unit USD": f"{unit:.2f}", "Extended USD": f"{unit * qty:.2f}",
                    "Required": "yes" if required else "optional", "In model": "yes" if in_model else "no",
                    "Price confirmed": "yes" if (b is None or b.confirmed) else "estimate",
                    "URL": b.url if b else "", "Notes": note or (b.note if b else "")})

    order = [PCB, BOUGHT, CELL, METAL, FASTENER, PRINTED, WIRE]
    for kind in order:
        for (k, desc), items in sorted(groups.items()):
            if k != kind:
                continue
            n = len(items)
            if kind == PRINTED:
                cm3 = sum(i.part.volume for i in items) / 1000 / n
                line(kind, n, desc, None, unit=cm3 * PETG_G_PER_CM3 * USD_PER_G,
                     note=f"Filament, {cm3:.1f} cm3 solid each (PETG 1.27 g/cm3 at $20/kg).")
            elif kind == WIRE:
                line(kind, n, desc, None, unit=0.0, note="From the jumper wire kit.")
            else:
                line(kind, n, desc, PCBS.get(desc) or CATALOG[desc])
        for qty, k, desc, b, req in EXTRAS[key]:
            if k == kind:
                line(kind, qty, desc, b, required=req, in_model=False)
    req = sum(float(r["Extended USD"]) for r in out if r["Required"] == "yes")
    opt = sum(float(r["Extended USD"]) for r in out if r["Required"] != "yes")
    blank = dict.fromkeys(COLUMNS, "")
    out.append({**blank, "Category": "Total", "Part": "Required parts", "Extended USD": f"{req:.2f}"})
    if opt:
        out.append({**blank, "Category": "Total", "Part": "Optional parts", "Extended USD": f"{opt:.2f}"})
    return out


def units() -> dict[str, Unit]:
    from concepts.designs import CONCEPTS
    from deadbolt.fixture import GENERIC_US
    from devkit.rig import rig
    return {**{k: c.unit(GENERIC_US) for k, c in CONCEPTS.items()}, "devkit": rig()[0]}


def write(key: str, u: Unit) -> Path:
    OUT.mkdir(exist_ok=True)
    path = OUT / f"{key}.csv"
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, COLUMNS)
        w.writeheader()
        w.writerows(rows(key, u))
    return path


def main() -> int:
    for key, u in units().items():
        path = write(key, u)
        total = next(r for r in csv.DictReader(path.open()) if r["Part"] == "Required parts")
        print(f"{path.relative_to(HERE)}: ${total['Extended USD']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
