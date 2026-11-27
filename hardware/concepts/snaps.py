"""Printed snap features, so parts fasten without screws.

Each function returns the geometry to add to or cut from the parts involved, registers the
snap's strain (Unit.snaps, checked by concepts/check.py) and its barb (Unit.barbs: the
assembly sweep in concepts/assembly.py lets a barb pass, because its arm flexes).

Where it can, an arm flexes in the plane it was printed in ("along" the layers), which is
the strong way for a printed part. Arms that stand up from the bed bend "across" the
layers and get a lower strain limit (concepts/kit.py, SNAP_STRAIN_LIMIT_ACROSS).

| Feature | Flexes | Used for |
|---|---|---|
| edge_tab | in the part's plane | a flat part (bridge, deck, carrier) clicking into windows in the chassis walls |
| wall_latch | across the layers | the cover: arms in the chassis wall, their barbs show in windows in the cover |
| hook_arm | across the layers | holding a part down by its top edge (breadboards, the motor) |
| keyhole | doesn't | the chassis hanging on the back plate |
"""

from __future__ import annotations

from build123d import Cone, Location, Part, Plane, Polygon, Pos, extrude

from concepts.kit import Snap, Unit, box, zcyl

SLIDE_FIT = 0.2  # play per side where a printed part slides on or into another
WINDOW_PLAY = 0.3  # a barb's window is this much bigger than the barb all round


def add_barb(u: Unit, owner: str, barb: Part) -> None:
    u.barbs[owner] = u.barbs[owner] + barb if owner in u.barbs else barb


def _xbox(s: int, a: float, b: float, y0: float, y1: float, z0: float, z1: float) -> Part:
    """A box from |x| = a to b on side s (+1: +X, -1: -X)."""
    return box(min(s * a, s * b), max(s * a, s * b), min(y0, y1), max(y0, y1), z0, z1)


def edge_tab(u: Unit, name: str, owner: str, side: int, x_edge: float, y_root: float, y_tip: float,
             z0: float, z1: float, wall_x: float, wall_t: float = 1.6, beam: float = 1.6,
             relief: float = 1.3, engage: float = 0.9, barb_l: float = 3.0) -> tuple[Part, Part, Part]:
    """A spring tab on the edge of a flat part, clicking into a window in the wall beside it.

    The part lies in z0..z1 with its edge at |x| = x_edge on `side`; the wall's inner face is
    at |x| = wall_x. A slot cut parallel to the edge leaves a beam from y_root to y_tip; the
    barb on its free end sticks out past the wall's inner face. Pushing the part down, the
    ramp under the barb bends the beam in (in its own plane, along the layers) until the
    barb reaches the window and springs out; its flat top then holds the part down. Squeeze
    the tab's tip inwards to lift the part out.

    Returns (cut from the part, barb to add to it, window to cut from the wall).
    """
    d = 1.0 if y_tip > y_root else -1.0
    gap = wall_x - x_edge
    out = gap + engage  # how far the barb stands proud of the edge
    deflect = out - gap
    assert relief >= deflect + 0.3, f"{name}: the slot is too narrow for the tab to flex"
    inner = x_edge - beam
    cut = _xbox(side, inner - relief, inner, y_root, y_tip + d * relief, z0 - 1, z1 + 1)
    cut += _xbox(side, inner - relief, x_edge + 1, y_tip, y_tip + d * relief, z0 - 1, z1 + 1)
    ramp = min(z1 - z0 - 0.6, 1.2 * out)
    prof = Polygon((side * x_edge, z0), (side * (x_edge + out), z0 + ramp), (side * (x_edge + out), z1),
                   (side * x_edge, z1), align=None)
    y_lo, y_hi = sorted((y_tip, y_tip - d * barb_l))
    barb = extrude(Plane.XZ * prof, amount=barb_l)  # which way it goes depends on the profile's winding
    barb = Pos(0, y_lo - barb.bounding_box().min.Y, 0) * barb
    window = _xbox(side, wall_x - 0.1, wall_x + wall_t + 0.1, y_lo - WINDOW_PLAY, y_hi + WINDOW_PLAY,
                   z0 - WINDOW_PLAY, z1 + 0.2)
    u.snaps.append(Snap(name, beam, abs(y_tip - y_root) - barb_l / 2, deflect, "along"))
    add_barb(u, owner, barb)
    return cut, barb, window


def wall_latch(u: Unit, name: str, wall_owner: str, loc: Location, z_root: float, z_top: float, zb: float,
               wall_t: float = 1.6, width: float = 10.0, gap: float = 0.25, engage: float = 1.0,
               bump_h: float = 2.0, cover_t: float = 2.0) -> tuple[Part, Part, Part]:
    """An arm cut in a wall (two slits from z_root up through its free top edge z_top), with a
    barb on its outer face whose flat underside is at zb. A cover sliding on over the wall
    (gap away from it) rides up the barb's ramp, pushing the arm in, until the barb snaps
    out into a window in the cover. Press the barb through the window to take the cover off.

    loc puts the wall's outer face on local y = 0, +y outwards, x along the wall.
    Returns (slits to cut from the wall, barb to add to it, window to cut from the cover).
    """
    slits = Part()
    for sx in (-1, 1):
        x = sx * (width / 2 + 0.3)
        slits += box(x - 0.3, x + 0.3, -wall_t - 0.1, 0.1, z_root, z_top + 1)
    out = gap + engage
    prof = Polygon((0, zb), (out, zb), (out, zb + 0.4), (0, zb + bump_h), align=None)
    barb = Pos(-(width / 2 - 0.5), 0, 0) * extrude(Plane.YZ * prof, amount=width - 1.0)
    window = box(-(width / 2 - 0.5) - WINDOW_PLAY, width / 2 - 0.5 + WINDOW_PLAY, gap - 0.1, gap + cover_t + 0.1,
                 zb - WINDOW_PLAY, zb + bump_h + WINDOW_PLAY)
    u.snaps.append(Snap(name, wall_t, zb - z_root, engage, "across"))
    add_barb(u, wall_owner, loc * barb)
    return loc * slits, loc * barb, loc * window


def hook_arm(u: Unit, name: str, owner: str, loc: Location, z_root: float, z_catch: float, t: float = 1.2,
             width: float = 6.0, gap: float = 0.3, catch: float = 0.9, barb_h: float = 1.5,
             sink_from: float | None = None) -> tuple[Part, Part]:
    """An arm standing up beside a part, its barb hooked over the part's top edge (at z_catch).

    loc: local y = 0 on the edge it holds, +y away from the part, x along the edge. The arm
    rises from z_root, `gap` clear of the part; the barb reaches `catch` over its top, with a
    ramp on top so the part pushes the arm aside on its way in. With sink_from (the top of
    the owner it rises from) the arm's root is sunk into a pocket in the owner, which
    lengthens the arm and lowers its strain.
    Returns (arm and barb to add to the owner, pocket to cut from the owner).
    """
    zc = z_catch + 0.1
    arm = box(-width / 2, width / 2, gap, gap + t, z_root, zc + barb_h)
    prof = Polygon((gap, zc), (-catch, zc), (-catch, zc + 0.4), (gap, zc + barb_h), align=None)
    barb = Pos(-width / 2, 0, 0) * extrude(Plane.YZ * prof, amount=width)
    pocket = Part()
    if sink_from is not None:
        pocket = box(-width / 2 - 0.6, width / 2 + 0.6, gap - 0.6, gap + t + catch + 0.6, z_root, sink_from + 0.01)
        pocket = loc * pocket
    u.snaps.append(Snap(name, t, z_catch - z_root, catch, "across"))
    add_barb(u, owner, loc * barb)
    return loc * (arm + barb), pocket


# --- keyhole: the chassis hangs on two pegs on the back plate ---
KEY_STEM, KEY_HEAD = 3.5, 6.9  # peg stem and head diameters
KEY_PASS = KEY_HEAD + 0.4  # the round hole the head drops through


def keyhole(x: float, y: float, slide: float, plate_top: float, floor_top: float) -> tuple[Part, Part]:
    """A peg on the plate at (x, y) and the keyhole in the chassis floor above it.

    The chassis goes on `slide` mm higher (+Y), the peg's head through the round hole, then
    slides down until the stem stops at the end of the narrow slot. The head is a 45 deg cone
    (it prints without support; its top is only 0.3 above the floor) sitting in a matching chamfer along the slot, SLIDE_FIT
    clear, so the chassis can't come away from the door. The floor runs plate_top..floor_top.
    Returns (peg to add to the plate, cut from the chassis floor).
    """
    r_slot = KEY_STEM / 2 + SLIDE_FIT
    ch0 = plate_top + 0.5  # the chamfer starts here and widens at 45 deg to the floor's top
    r_top = r_slot + (floor_top - ch0)
    off = SLIDE_FIT * 1.414  # radial play that gives SLIDE_FIT square to the 45 deg faces
    c0 = ch0 + off - (r_slot - KEY_STEM / 2)  # where the head's cone leaves the stem
    r_head = KEY_HEAD / 2
    peg = zcyl(KEY_STEM, plate_top, c0 + 0.01, x, y)
    peg += Pos(x, y, c0 + (r_head - KEY_STEM / 2) / 2) * Cone(KEY_STEM / 2, r_head, r_head - KEY_STEM / 2)

    trap = Polygon((-r_slot, ch0), (r_slot, ch0), (r_top + 0.01, floor_top + 0.01), (-r_top - 0.01, floor_top + 0.01),
                   align=None)
    cut = Pos(x, y, 0) * extrude(Plane.XZ * trap, amount=slide)  # from y down to y - slide
    cut += box(x - r_slot, x + r_slot, y - slide, y, plate_top - 0.1, ch0 + 0.01)
    cut += zcyl(2 * r_slot, plate_top - 0.1, ch0 + 0.01, x, y)
    cut += Pos(x, y, (ch0 + floor_top + 0.01) / 2) * Cone(r_slot, r_top + 0.01, floor_top + 0.01 - ch0)
    cut += zcyl(KEY_PASS, plate_top - 0.1, floor_top + 0.1, x, y - slide)
    return peg, cut
