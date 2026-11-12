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
| A-puck | round cup, the front cap is the knob, cells under the cap | 2x CR123A | Pololu #5193, standing | annulus under the bridge (chip-down ESP32-C6) |
| B-slab | vertical, drive at the bottom, cells above behind a door | 4x AA lithium | Pololu #5193, standing | over the drive |
| C-pill | horizontal, cells towards the hinge behind a door | 4x AA lithium | Pololu #5185 lying flat, worm drive | over the drive |

`concepts/kit.py` holds what they share. That's the drive module: a coupler over the tailpiece, an m1 involute output gear with a 132 deg lost-motion pocket, the motor pinion, and bolt sensing. It also holds the fastener library, snaps and helpers. `concepts/parts.py` models the motor and every chip and breakout board from its spec sheet (see below). `concepts/designs.py` builds each concept: steel back plate on the deadbolt's screws, printed chassis (floor, skirt, posts, bosses, slotted battery ribs, snap arms), bridge, PCB stack on spacers, cover with battery door, knob, light pipe.

```sh
uv run build_concepts.py              # per concept: STEP, STLs of printed parts, BOM.md, renders -> out/concepts/<key>/
uv run python -m concepts.check       # assembly problems, listed
uv run pytest tests/test_concepts.py
```

### Sensing and electronics

The lock needs two positions. The **bolt's** position matters because the knob and the key turn it without the motor. The **motor's** position matters because after each move the motor backs the output gear off to the middle of its lost-motion pocket, so the knob and the key turn freely.

- **Bolt:** a printed flag on the coupler's D-shaft carries a 3 x 2 mm magnet at 10.5 mm radius (8.5 in the puck, between its cells). Two DRV5032 Hall switches on the main PCB's underside sit where the magnet stops when the bolt is locked and when it is unlocked; between them, the bolt is moving or jammed. `DriveLayout.hall_xy()` places them. In the puck they sit on a small sensor board, because its main PCB is down by the floor.
- **Motor:** a Pololu micro metal gearmotor with a 12 CPR quadrature encoder on its back end. Through the 150:1 gearbox and the 4:1 output gear that is about 7,200 counts per bolt turn, read by the ESP32-C6's pulse counter. A count that stops while the motor is driven is a stall.
- **Door:** a third DRV5032 by the latch edge, for a magnet on the frame.

The product and the dev kit use the same chips. `concepts/parts.py` models each from its spec sheet (links in `SOURCES`), with its pins:

| Function | Product (main PCB) | Dev kit (breakout) |
|---|---|---|
| MCU, Thread + BLE | ESP32-C6-MINI-1-N4 (13.2 x 16.6 x 2.4) | ESP32-C6-DevKitM-1 (25.4 x 48.26, the same module) |
| Motor driver | DRV8833PWPR, HTSSOP-16 | Pololu #2130 carrier (12.7 x 20.3) |
| Bolt and door switches (3) | DRV5032FADBZR, SOT-23 | SparkFun BOB-00717 SOT-23 adapters, chip-down |
| 3.3 V from the cells | TPS629206DRLR, SOT-583 (3-17 V in, 4 uA) | TPS629206EVM, bench only |
| Motor + encoder | Pololu #5193 (150:1); #5185 (30:1) in the worm drive | the same |
| Motor cable socket | JST SM06B-SRSS-TB (6-pin SH, side entry) | the motor cable's own plug, to the breadboards |

The puck can't fit the MINI-1 module in its ring-shaped PCB, so it uses the bare ESP32-C6 chip: the same silicon, but its own antenna and RF layout.

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
| Bolt flag (magnet carrier) to coupler | keyed on the D-shaft | |
| Battery contacts | drop into slots in the ribs from above; link plates put the cells in series | |

No screw threads into bare plastic. Screws are stock lengths: `fix()` in `concepts/kit.py` picks the shortest one that engages enough, and sinks the insert deeper when a stock length would bottom out. Insert sizes are CNC Kitchen / Ruthex standard (M2: 3.2 mm hole, 3 mm long; M3: 4.0 mm hole, 5.7 mm long). Check the hole size against the inserts you buy.

The motor's face holes (2x M1.6, 9 mm apart) are from Pololu's drawing; how deep they are threaded is not, so `fix()` assumes 1.2-2.0 mm (UNMEASURED).

### Checks

The checks (and tests) run for every fixture preset, locked and unlocked:
- no two parts collide, except meshing gear teeth
- every declared joint actually touches (or is within its running clearance)
- every part connects through joints back to the door
- the unit stays clear of the latch edge and the handle (the handle clearance is a placeholder until measured)
- every screw is a stock length, with enough thread engaged and without bottoming out
- at least 1.2 mm of material (three perimeters) round every insert and tapped hole
- no snap arm strains past 2.5% when it engages (PETG/ASA)

Approximations: gear teeth are true involutes; the worm is drawn as rings, not a helix. The motor and electronics are modelled from their drawings (`concepts/parts.py`); cells, magnets and screws are datasheet envelopes; the status LED, button and battery connector are placeholders until they are chosen. Battery life comes from the energy model at the bottom of `designs.py`.

## Dev kit (`devkit/`)

The tall slab (B), opened up for firmware work on a real door. It mounts the same way and has the same drive, motor, battery bay and chips (each on a breakout), so what you learn on it carries over. The differences:

- **Deck instead of the main PCB:** a removable printed deck carries two 170-point mini breadboards. The upper one holds the ESP32-C6-DevKitM-1 (USB end at the cover's side slot, antenna overhanging the far clamp bar) and the lower one the Pololu DRV8833 carrier. The deck is 4.5 mm thick so it can hold M2 inserts. It sits on the slab's PCB spacers at one edge and on two tall posts beyond the cells at the other.
- **Breadboard mounting:** each breadboard is held down by two printed clamp bars, with a lip over each end's margin, plus its adhesive back. Each bar has two M2 screws. Most go into inserts in the deck; the upper breadboard's top two go through the deck into the tall chassis posts, so they hold the deck down too.
- **Hall switches:** the three DRV5032s are on SOT-23 adapters, glued chip-down into a printed carrier on three spacers. The chips' faces are where the product PCB's underside is, so they see the bolt flag's magnet exactly as on the product. Their header pins come up through the carrier for jumper wires.
- **Open cover:** the slab's shape with a big front window, so the wiring stays in view, and a side slot for the DevKit's USB cable. It's 17 mm taller than the slab, to fit the second breadboard. Clear PETG works well for it.
- **Changing cells:** there's no battery door. Remove the cover (two screws, two snaps) and the deck (four screws).
- **Wiring:** all 24 jumper wires are modelled, coloured by signal, including the motor's 6-wire cable. `WIRING` in `devkit/rig.py` is the proposed pin map: motor PWM on GPIO 18/19, driver enable 20, driver fault 3, motor encoder A/B on 21/4, bolt locked/unlocked switches on 22/23, door switch on 2. It avoids the strapping, USB and UART pins. 3V3 and GND are shared along two spare rows of the driver's breadboard. The 4xAA pack feeds the carrier's VIN and the DevKit runs from USB. A test checks that no breadboard hole takes two wires.
- **Regulator:** the TPS629206 is the one product chip not on the deck. Its EVM is for bench battery-life runs from the cells, with the DevKit off USB (feeding the DevKit's 3V3 pin while USB is in would fight its own regulator).

```sh
uv run view.py devkit                 # in the viewer, on the door (or Cmd+Shift+B -> devkit)
uv run build_devkit.py                # STEP, printed-part STLs, BOM.md, WIRING.md, renders -> out/devkit/
uv run python -m devkit.check         # the same checks as the concepts
```

UNMEASURED or UNCONFIRMED: mini breadboard size (47 x 35 mm here; they vary); the DevKitM-1's board thickness (1.6 assumed; Espressif's drawing gives the outline, headers, module and USB positions); the SOT-23 adapter's size (SparkFun's Eagle file says 10.16 x 7.62, its product page 8.6 x 11.7) and which holes carry VCC, OUT and GND; the motor's output shaft length (10 mm on Pololu's drawing, 9 on its product page); the TPS629206EVM's outline (no drawing; about 54 x 39).

## License

Copyright (c) 2026 Carson Loyal.

The hardware designs in this directory (the models, the scripts that generate them, and the STEP, STL and render outputs) are licensed under the CERN Open Hardware Licence Version 2 - Strongly Reciprocal (CERN-OHL-S-2.0); see [LICENSE](LICENSE). You may print, build, modify and sell them. If you distribute a modified design, or products made from one, you must make its source available under the same licence.

Source location: https://github.com/carTloyal123/lochness/tree/main/hardware

This licence covers the designs, not the name. It does not grant use of the project's name or any marks.
