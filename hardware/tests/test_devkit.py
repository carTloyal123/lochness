"""The dev kit must assemble, fit the door and wire up, like a concept does; and, since it is
built from printed snap-fit parts, go together step by step with no screws but the deadbolt's."""

import pytest

from concepts import assembly
from concepts.check import collisions, fasteners, fit, joint_gaps, snaps, unattached
from concepts.kit import FASTENER, PRINTED
from deadbolt.fixture import PRESETS
from concepts.parts import DEVKITM_J1, DEVKITM_J3
from devkit.rig import BB_TOP, HALLS, PRINT, Hole, devkit_pin, rig, wire_plan
from wiring.netlist import PIN_MAP, RESERVED_GPIO


@pytest.fixture(scope="module", params=[(k, locked) for k in PRESETS for locked in (True, False)],
                ids=lambda c: f"{c[0]}-{'locked' if c[1] else 'unlocked'}")
def built(request):
    key, locked = request.param
    return (*rig(PRESETS[key], locked), PRESETS[key])


def test_parts_are_valid(built):
    u, _, _ = built
    assert [n for n, i in u.items.items() if not i.part.is_valid or i.part.volume <= 0] == []


def test_no_collisions(built):
    u, fixture, _ = built
    assert collisions(u, fixture) == []


def test_every_joint_touches(built):
    u, fixture, _ = built
    assert joint_gaps(u, fixture) == []


def test_every_part_is_attached(built):
    u, _, _ = built
    assert unattached(u) == []


def test_fasteners_are_sound(built):
    u, _, _ = built
    assert fasteners(u) == []


def test_fits_the_door(built):
    u, _, p = built
    assert fit(u, p) == []


def test_snaps_within_strain(built):
    u, _, _ = built
    assert snaps(u) == []


@pytest.fixture(scope="module")
def assembled(built):
    u, fixture, _ = built
    return assembly.check(u, fixture)


def test_assembly_sequence_covers_every_part(assembled):
    assert assembled["sequence"] == []


def test_every_step_can_be_done(assembled):
    assert assembled["insertion"] == []


def test_no_printed_fit_is_line_to_line(assembled):
    assert assembled["clearance"] == []


def test_only_the_deadbolts_screws(built):
    u, _, _ = built
    assert sorted(n for n, i in u.items.items() if i.kind == FASTENER) == ["screw_door_l", "screw_door_r"]


def test_every_snap_states_its_print_direction(built):
    u, _, _ = built
    assert u.snaps and all(s.layers in ("along", "across") for s in u.snaps)


def test_every_printed_part_has_a_print_pose():
    u, _ = rig()
    assert sorted(n for n, i in u.items.items() if i.kind == PRINTED) == sorted(PRINT)


def test_pin_map_avoids_reserved_pins():
    used = [g for g, _, _ in PIN_MAP.values()]
    assert len(used) == len(set(used)), "a GPIO is used twice"
    assert set(used).isdisjoint(RESERVED_GPIO)
    assert {int(g) for g in HALLS.values()} <= set(used)
    for gpio in used:
        assert str(gpio) in DEVKITM_J1 + DEVKITM_J3
        devkit_pin(str(gpio))


def test_every_wire_follows_the_plan():
    u, _ = rig()
    wires = [i for i in u.items.values() if i.kind == "wire"]
    assert [w.plan for w in wires] == wire_plan()
    for w in wires:
        for end, e in zip(w.ends, (w.plan.a, w.plan.b)):
            if isinstance(e, Hole):
                assert end == e.xyz()


def test_no_breadboard_hole_takes_two_wires():
    u, _ = rig()
    holes = [tuple(round(v, 2) for v in end) for i in u.items.values() if i.kind == "wire"
             for end in i.ends if abs(end[2] - BB_TOP) < 1e-6]
    assert len(holes) == len(set(holes))
    assert len(holes) > 20
