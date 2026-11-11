"""Every concept must assemble: no collisions, every joint touching, every part
attached back to the door, and the whole unit clear of the door edge and handle."""

import pytest

from concepts.check import collisions, fasteners, fit, joint_gaps, snaps, unattached
from concepts.designs import CONCEPTS, life_years
from concepts.kit import PRINTED
from deadbolt.fixture import PRESETS, build

CASES = [(k, preset, locked) for k in CONCEPTS for preset in PRESETS for locked in (True, False)]


@pytest.fixture(scope="module", params=CASES, ids=lambda c: f"{c[0]}-{c[1]}-{'locked' if c[2] else 'unlocked'}")
def built(request):
    key, preset, locked = request.param
    p = PRESETS[preset]
    return CONCEPTS[key].unit(p, locked), build(p, locked), p


def test_parts_are_valid_solids(built):
    u, _, _ = built
    bad = [n for n, i in u.items.items() if not i.part.is_valid or i.part.volume <= 0]
    assert not bad


def test_no_collisions(built):
    u, fixture, _ = built
    assert collisions(u, fixture) == []


def test_every_joint_touches(built):
    u, fixture, _ = built
    assert joint_gaps(u, fixture) == []


def test_every_part_is_attached_to_the_door(built):
    u, _, _ = built
    assert unattached(u) == []


def test_fits_the_door(built):
    u, _, p = built
    assert fit(u, p) == []


def test_fasteners_are_sound(built):
    u, _, _ = built
    assert fasteners(u) == []


def test_no_screw_threads_into_bare_plastic(built):
    u, _, _ = built
    assert all(f.thread in ("insert", "tapped", "bought") for f in u.fixings)
    assert all(f.thread == "insert" or u.items[f.into].kind != PRINTED for f in u.fixings)


def test_snaps_within_strain(built):
    u, _, _ = built
    assert snaps(u) == []


@pytest.mark.parametrize("key", sorted(CONCEPTS))
def test_concept_has_printable_parts_and_battery_life(key):
    c = CONCEPTS[key]
    u = c.unit(PRESETS["generic"])
    assert any(i.kind == PRINTED for i in u.items.values())
    assert life_years(c) > 0.5
