"""Show the lock-unit concepts on the door in OCP CAD Viewer (VS Code).

Open this file in VS Code; the viewer panel opens by itself. Then run it
(Run Python File, or the "hardware: view" task). Edit CONCEPT, PRESET or LOCKED
below and run again.

    uv run view.py                  # the concepts, side by side on separate doors
    uv run view.py B-slab           # one concept
    uv run view.py fixture          # just the door and deadbolt
    uv run view.py devkit           # the dev kit on its own

In the viewer: the tree on the left toggles each part (hide the shell to see
inside), the clipping tab cuts sections, and the measure tools check clearances.
"""

from __future__ import annotations

import sys
import warnings

from build123d import Compound, Pos
from ocp_vscode import Camera, find_and_set_port, get_port, show

from concepts.designs import CONCEPTS
from deadbolt.fixture import PRESETS, build
from devkit.rig import rig

CONCEPT = "all"  # "all", "fixture", "devkit", or a key from concepts.designs.CONCEPTS
PRESET = "generic"
LOCKED = True
SPACING = 260.0  # mm between doors when showing every concept


def scene(concept: str, preset: str = PRESET, locked: bool = LOCKED) -> list[Compound]:
    p = PRESETS[preset]
    if concept == "devkit":
        u, parts = rig(p, locked)
        name = "dev kit"
    else:
        parts = build(p, locked)
        if concept == "fixture":
            return [Compound(label="door+deadbolt", children=list(parts.values()))]
        u, name = CONCEPTS[concept].unit(p, locked), CONCEPTS[concept].name
    fixture = Compound(label="door+deadbolt", children=list(parts.values()))
    groups: dict[str, list] = {}
    for item in u.items.values():  # one tree node per kind, to hide e.g. all wires or fasteners
        groups.setdefault(item.kind, []).append(item.part)
    unit = Compound(label=name, children=[Compound(label=k, children=v) for k, v in groups.items()])
    return [Compound(label=concept, children=[unit, fixture])]


NO_VIEWER = """
No OCP CAD Viewer is open, so there is nothing to show the models in.

  1. In VS Code, open hardware/view.py. The "OCP CAD Viewer" tab opens with the
     OCP logo (or run "OCP CAD Viewer: Open viewer" from the command palette).
  2. Leave that tab open. Closing it shuts the viewer down. Drag it to the side
     if it covers your code.
  3. Run this again: Cmd+Shift+B, or `uv run view.py` in the VS Code terminal.
"""

# ocp_vscode 4.2 warns about its own collapse setting on every show; harmless.
warnings.filterwarnings("ignore", message="Unknown collapse value from viewer")


def viewer_open() -> bool:
    find_and_set_port()  # look again: the viewer may have opened or closed since import
    return get_port() is not None


def main() -> int:
    if not viewer_open():
        print(NO_VIEWER, file=sys.stderr)
        return 1
    choice = sys.argv[1] if len(sys.argv) > 1 else CONCEPT
    if choice == "all":
        objs = [Pos(i * SPACING, 0, 0) * scene(key)[0] for i, key in enumerate(CONCEPTS)]
        for obj, key in zip(objs, CONCEPTS):
            obj.label = key
    else:
        objs = scene(choice)
    if not viewer_open():  # building takes a few seconds; check the viewer is still there
        print(NO_VIEWER, file=sys.stderr)
        return 1
    show(*objs, names=[o.label for o in objs], reset_camera=Camera.RESET)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
