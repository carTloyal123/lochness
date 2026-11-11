# hardware

Mechanical CAD for the lock unit, written as code in [build123d](https://github.com/gumyr/build123d) (Apache-2.0, a library only; no third-party CAD files are vendored). Everything here is MIT like the rest of the repo.

```sh
cd hardware
uv run build.py                 # STEP, STL, interface.json and PNG renders -> out/generic/
uv run build.py --preset generic-thin-door
uv run pytest                   # geometry checks for every preset, locked and unlocked
```

Renders need [f3d](https://f3d.app) (`brew install f3d`); `--no-render` skips them.

### Viewing in 3D (VS Code)

Opening the repo in VS Code, locally or over Remote-SSH, prompts you to install the recommended extensions: Python and [OCP CAD Viewer](https://marketplace.visualstudio.com/items?itemName=bernhard-42.ocp-cad-viewer). Over Remote-SSH they install on the remote host. The `hardware: setup` task runs `uv sync` when the folder opens; VS Code asks once whether to allow automatic tasks. Then:

1. Open `hardware/view.py`. An "OCP CAD Viewer" tab opens showing the OCP logo. It stays empty until a script sends it something.
2. Leave that tab open; closing it stops the viewer. Drag it into a split if it covers your code.
3. Press Cmd+Shift+B (the `hardware: view` task) and pick `all` or one concept. The models appear after a few seconds.

If no viewer tab is open, `view.py` says so instead of sending. Needs [uv](https://docs.astral.sh/uv/) on the machine that holds the repo.

## Deadbolt fixture (`deadbolt/fixture.py`)

A door with a US single-cylinder deadbolt, **with the interior thumb-turn already removed**, because the lock unit replaces it. Enclosures are designed against this fixture: `interface(params)` gives the numbers they need (tailpiece axis, size and turn angles, screw-post positions, bore, door edge), and `out/<preset>/hardware-*.step` is the hardware alone for fit checks.

Frame: origin on the bore axis at the interior door face; +Z into the room; +X towards the latch edge (at `x = backset`); +Y up; millimetres.

| Output | What it is |
|---|---|
| `fixture-{locked,unlocked}.step` | door + latch + bolt + exterior cylinder + tailpiece, coloured and named |
| `hardware-{locked,unlocked}.step` | the same without the door |
| `section-*.step`, `*-section.png` | cut through the bore axis |
| `*.stl` | each part, locked state |
| `interface.json` | interface numbers, all parameters, and the list of unmeasured ones |

### Where the numbers come from

**Standard US door prep (published):** 2-1/8" cross bore, 1" edge bore, 2-3/8" or 2-3/4" backset, 1-3/8" to 1-3/4" doors, 1" bolt throw, 1" × 2-1/4" faceplate. These come from manufacturer spec sheets (e.g. Baldwin, Taymor) for ANSI/BHMA A156.40 residential deadbolts.

**Illustrative only:** the exterior rose, latch internals and bolt size. They stay on the door unchanged and the unit never touches them.

**UNMEASURED placeholders:** the tailpiece and the screw posts. They differ by brand and are exactly what the unit couples and bolts to, so `build.py` warns about them on every run. Replace them with measurements from real hardware and add a preset per brand (`kwikset`, `schlage`, ...).

### Measurement checklist (per brand, with the interior thumb-turn removed)

Tailpiece (what the motor turns):
1. Width and thickness of the flat bar.
2. How far it sticks out past the interior door face.
3. Its angle when locked and when unlocked, and which way it turns to unlock (this depends on which side the door is hinged).

Screw holes (what the unit bolts to):
4. Centre-to-centre spacing of the two holes. They sit left and right of the tailpiece and pass through the latch.
5. Screw thread (read it off the original screws, e.g. #10-24), and the original screw length.
6. Where the posts end relative to the interior door face: flush, recessed, or sticking out (and by how much).

The door around it (how big the unit can be):
7. Backset (door edge to bore centre) and door thickness.
8. Distance from the deadbolt centre to the handle or knob centre below it.
9. Anything else nearby on the interior side: trim, molding, a storm door, how close the jamb or wall comes when the door is open.

For motor sizing (not CAD):
10. Torque to turn the thumb-turn with the door closed, including when the bolt rubs the strike.

## Lock-unit concepts (`concepts/`)

Three layouts built from real parts on the fixture, so they can be compared and printed:

| Concept | Layout | Battery | Motor | Main PCB |
|---|---|---|---|---|
| A-puck | round cup, the front cap is the knob, cells under the cap | 2x CR123A | N20, standing | annulus under the bridge (chip-down ESP32-C6) |
| B-slab | vertical, drive at the bottom, cells above behind a door | 4x AA lithium | 16 mm planetary, standing | over the drive |
| C-pill | horizontal, cells towards the hinge behind a door | 4x AA lithium | N20 lying flat, worm drive | over the drive |

`concepts/kit.py` holds what they share. That's the drive module: a coupler over the tailpiece, an m1 involute output gear with a 132 deg lost-motion pocket, the motor pinion, and a 1:1 encoder gear pair turning a diametric magnet under an AS5600. It also holds the fastener library, snaps, bought-part envelopes and helpers. `concepts/designs.py` builds each concept: steel back plate on the deadbolt's screws, printed chassis (floor, skirt, posts, bosses, slotted battery ribs, snap arms), bridge, PCB stack on spacers, cover with battery door, knob, light pipe.

```sh
uv run build_concepts.py              # per concept: STEP, STLs of printed parts, BOM.md, renders -> out/concepts/<key>/
uv run python -m concepts.check       # assembly problems, listed
uv run pytest tests/test_concepts.py
```

### How the parts fasten

| Joint | Fastener | Threads into |
|---|---|---|
| Back plate to door | the deadbolt's own 2 flat-head through-screws | the exterior half's posts |
| Chassis to back plate | 2x M3 x 5 button head | tapped holes in the 3 mm steel plate |
| Bridge and PCB stack to chassis | M2 button head through PCB, spacer tube and bridge | M2 heat-set inserts in the chassis posts |
| Motor to bridge (A, B) / bulkhead (C) | 2 screws into the gearbox face, heads sunk into the 3 mm bridge | the motor's own face holes |
| Cover to chassis (B, C) | 2x M3 x 6 countersunk at the bottom, 2 snap arms at the top (B) or ends (C) | M3 inserts set into the chassis wall from outside |
| Knob / cap to coupler | M3 button head, knob keyed on the coupler's D-shaft | an M3 insert in the coupler's top |
| Battery door | 2 hooks under the rim, a snap latch at the other end | a keeper under the cover's rim |
| Encoder gear to coupler | keyed on the D-shaft | |
| Battery contacts | drop into slots in the ribs from above; link plates put the cells in series | |

No screw threads into bare plastic. Screws are stock lengths: `fix()` in `concepts/kit.py` picks the shortest one that engages enough, and sinks the insert deeper when a stock length would bottom out. Insert sizes are CNC Kitchen / Ruthex standard (M2: 3.2 mm hole, 3 mm long; M3: 4.0 mm hole, 5.7 mm long). Check the hole size against the inserts you buy.

UNMEASURED: the motors' face-screw patterns (N20: 2x M1.6 at 9 mm; 16 mm planetary: 2x M2 at 12 mm) are typical datasheet values. Check them against the motors you buy before printing the bridge.

### Checks

The checks (and tests) run for every fixture preset, locked and unlocked:
- no two parts collide, except meshing gear teeth
- every declared joint actually touches (or is within its running clearance)
- every part connects through joints back to the door
- the unit stays clear of the latch edge and the handle (the handle clearance is a placeholder until measured)
- every screw is a stock length, with enough thread engaged and without bottoming out
- at least 1.2 mm of material (three perimeters) round every insert and tapped hole
- no snap arm strains past 2.5% when it engages (PETG/ASA)

Approximations: gear teeth are true involutes; the worm is drawn as rings, not a helix. Bought parts are datasheet-typical envelopes. Battery life comes from the energy model at the bottom of `designs.py`.

## License

Copyright (c) 2026 Carson Loyal.

The hardware designs in this directory (the models, the scripts that generate them, and the STEP, STL and render outputs) are licensed under the CERN Open Hardware Licence Version 2 - Strongly Reciprocal (CERN-OHL-S-2.0); see [LICENSE](LICENSE). You may print, build, modify and sell them. If you distribute a modified design, or products made from one, you must make its source available under the same licence.

Source location: https://github.com/carTloyal123/lochness/tree/main/hardware

This licence covers the designs, not the name. It does not grant use of the project's name or any marks.
