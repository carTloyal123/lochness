"""Electrical rule check: every stage is clean, and each rule catches a seeded mistake."""

from dataclasses import replace

import pytest

from wiring import bench, erc
from wiring.netlist import DEVKIT
from wiring.product import PRODUCT

STAGES = {s.key: s for s in (bench.BENCH, DEVKIT, PRODUCT)}


@pytest.mark.parametrize("key", list(STAGES))
def test_stage_is_electrically_clean(key):
    assert [str(f) for f in erc.check_stage(STAGES[key])] == []


def move(nets, pin, to=None):
    """A copy of the nets with one pin taken off its net and, if `to`, put on another."""
    out = {n: [p for p in ps if p != pin] for n, ps in nets.items()}
    if to:
        out.setdefault(to, []).append(pin)
    return {n: ps for n, ps in out.items() if ps}


def gpio(stage, g):
    return stage.blocks[("MCU", f"GPIO{g}")]


B, D, P = bench.BENCH, DEVKIT, PRODUCT
FAULTS = [  # (what's wrong, stage, mutation, rule, net it must be reported on)
    ("GPIO 22 wired to the 5 V rail", B, lambda n: move(n, gpio(B, 22), "+5V"), "over-voltage", "+5V"),
    ("18650 pack on the TB6612's logic VCC", B, lambda n: move(n, ("tb6612", "P-VCC"), "VBAT_2S"),
     "over-voltage", "VBAT_2S"),
    ("A3144 on 3.3 V instead of 5 V", B, lambda n: move(n, ("hall_door", "1"), "+3V3"), "under-voltage", "+3V3"),
    ("5 V rail not fed from the DevKit", B, lambda n: move(n, ("devkit", "J1-14")), "under-voltage", "+5V"),
    ("A3144 ground pin left out", B, lambda n: move(n, ("hall_locked", "2")), "ground", "hall_locked"),
    ("DevKit 3V3 shorted to GND", D, lambda n: move(n, ("devkit", "J1-1"), "GND"), "short-to-ground", "GND"),
    ("4xAA + on the 3.3 V row", D, lambda n: move(n, ("battery", "+"), "+3V3"), "one-driver", "+3V3"),
    ("Hall OUT wired to the motor's M1", D, lambda n: move(n, ("hall_door", "3"), "MOT_M1"), "one-driver", "MOT_M1"),
    ("nFAULT pull-up resistor missing", P, lambda n: move(n, ("R7", "2")), "pull-up", "DRV_nFAULT"),
    ("Hall switch ground on 3.3 V", P, lambda n: move(n, ("U3", "3"), "+3V3"), "ground", "+3V3"),
    ("GPIO8 strap pulled down", P, lambda n: move(n, ("R5", "1"), "GND"), "strapping", "IO8_PU"),
    ("two Hall outputs on one GPIO", P, lambda n: move(n, ("U3", "2"), "HALL_UNLOCKED"), "one-driver", "HALL_UNLOCKED"),
    ("battery straight into the ADC pin", P, lambda n: move(n, ("U1", "15"), "VBAT"), "over-voltage", "VBAT"),
]


@pytest.mark.parametrize("what,stage,mutate,rule,net", FAULTS, ids=[f[0] for f in FAULTS])
def test_rule_catches_seeded_fault(what, stage, mutate, rule, net):
    found = erc.check(mutate(stage.nets()), erc.configs()[stage.key])
    assert any(f.rule == rule and f.net == net for f in found), [str(f) for f in found]


def test_missing_gpio_pullup_is_caught():
    cfg = replace(erc.configs()["devkit"], pullups=set())
    assert any(f.rule == "pull-up" and f.net == "DRV_nFAULT" for f in erc.check(DEVKIT.nets(), cfg))


def test_divider_level_is_what_keeps_the_adc_safe():
    """Without its declared range, the battery-sense net reads as the full pack on GPIO 6."""
    cfg = replace(erc.configs()["product"], levels={})
    assert any(f.rule == "over-voltage" and f.net == "VBAT_SENSE" for f in erc.check(PRODUCT.nets(), cfg))


def test_every_part_has_a_model():
    for key, stage in STAGES.items():
        assert set(stage.parts) <= set(erc.configs()[key].models), key
