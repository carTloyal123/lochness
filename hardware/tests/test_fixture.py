"""Geometry checks on the deadbolt fixture, for every preset and both bolt states."""

from itertools import combinations

import pytest

from deadbolt.fixture import INCH, PRESETS, build, interface

TOL = 1e-3  # mm^3: shared faces must not count as overlap
CASES = [(name, locked) for name in PRESETS for locked in (True, False)]


@pytest.fixture(scope="module", params=CASES, ids=lambda c: f"{c[0]}-{'locked' if c[1] else 'unlocked'}")
def case(request):
    name, locked = request.param
    return PRESETS[name], locked, build(PRESETS[name], locked)


def test_every_part_is_a_valid_solid(case):
    _, _, parts = case
    for name, part in parts.items():
        assert part.is_valid, name
        assert part.volume > 0, name


def test_no_parts_overlap(case):
    _, _, parts = case
    for (a, pa), (b, pb) in combinations(parts.items(), 2):
        overlap = pa & pb
        vol = overlap.volume if overlap else 0.0
        assert vol < TOL, f"{a} and {b} overlap by {vol:.3f} mm^3"


def test_bolt_throw(case):
    p, locked, parts = case
    tip_x = parts["bolt"].bounding_box().max.X
    expected = p.backset + (p.bolt_throw if locked else 0.0)
    assert tip_x == pytest.approx(expected, abs=1e-3)


def test_only_the_tailpiece_reaches_the_room(case):
    p, _, parts = case
    for name, part in parts.items():
        top = part.bounding_box().max.Z
        if name == "tailpiece":
            assert top == pytest.approx(p.tailpiece_protrusion, abs=1e-3)
        else:
            assert top <= 1e-6, f"{name} sticks {top:.2f} mm out of the interior face"


def test_tailpiece_turns_on_the_bore_axis(case):
    p, locked, parts = case
    c = parts["tailpiece"].center()
    assert (c.X, c.Y) == pytest.approx((0, 0), abs=1e-6)


def test_standard_door_prep():
    p = PRESETS["generic"]
    assert p.cross_bore_d == pytest.approx(2.125 * INCH)
    assert p.edge_bore_d == pytest.approx(1.0 * INCH)
    assert p.backset in (pytest.approx(2.375 * INCH), pytest.approx(2.75 * INCH))
    assert p.housing_d < p.edge_bore_d


def test_interface_lists_unmeasured_values():
    info = interface(PRESETS["generic"])
    assert "tailpiece_w" in info["unmeasured"]
    assert info["tailpiece"]["unlocked_deg"] - info["tailpiece"]["locked_deg"] == pytest.approx(90)


def test_screw_posts_flank_the_tailpiece_through_the_latch(case):
    p, _, parts = case
    posts = interface(p)["screw_posts"]
    assert sorted(post["center"][0] for post in posts) == pytest.approx(
        [-p.screw_spacing / 2, p.screw_spacing / 2])
    assert all(post["center"][1] == 0 for post in posts)
    # every post fits in the bore and lies inside the latch frame's span
    for post in posts:
        assert abs(post["center"][0]) + p.post_d / 2 < p.cross_bore_d / 2
        assert abs(post["center"][0]) + p.post_d / 2 < p.drive_box_x / 2
    # the retracted bolt stays clear of the latch-edge post
    assert p.backset - p.bolt_len > p.screw_spacing / 2 + p.post_d / 2
