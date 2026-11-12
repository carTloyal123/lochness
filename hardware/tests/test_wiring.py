"""One net list: the dev kit model, the breadboard drawing and the KiCad schematic agree."""

import shutil
from pathlib import Path

import pytest

from devkit.rig import Hole, plan_connectivity, wire_plan
from wiring import bench
from wiring import kicad as K
from build_wiring import DRAWN
from concepts.parts import DEVKITM_J1, DEVKITM_J3
from wiring.netlist import CORE, DEVKIT, PIN_MAP, RESERVED_GPIO, devkit_pin_name
from wiring.product import MINI1, PRODUCT, PRODUCT_PINS

HERE = Path(__file__).resolve().parent.parent
SCH = HERE / "pcb" / "main" / "lochness-main.kicad_sch"
needs_kicad = pytest.mark.skipif(not (shutil.which("kicad-cli") or Path(
    "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli").exists()), reason="needs KiCad 10")


@pytest.mark.parametrize("stage", [bench.BENCH, DEVKIT, PRODUCT], ids=lambda s: s.key)
def test_stage_is_consistent(stage):
    assert stage.check() == []


@pytest.mark.parametrize("stage", [bench.BENCH, DEVKIT, PRODUCT], ids=lambda s: s.key)
def test_stage_builds_the_whole_circuit(stage):
    """Every block pin of the shared circuit has a part pin in each stage, or a stated reason
    why not (the bench's motor has no encoder)."""
    assert [b for pins in CORE.values() for b in pins if b not in stage.blocks and b not in stage.omit] == []
    assert all(stage.omit.values())


def test_only_the_bench_leaves_parts_of_the_circuit_out():
    assert DEVKIT.omit == {} and PRODUCT.omit == {} and DEVKIT.moves == {} and PRODUCT.moves == {}


def test_bench_wires_join_exactly_the_net_list():
    assert bench.plan_connectivity() == bench.BENCH.connectivity()


def test_bench_uses_the_dev_kit_pin_map():
    """Same firmware: every GPIO the bench wires is the dev kit's GPIO for that signal, though
    the bench's DevKitC-1 has it on another header pin than the dev kit's DevKitM-1."""
    def gpios(stage, j1, j3, net):
        return [devkit_pin_name(p, j1, j3) for r, p in stage.nets().get(net, []) if r == "devkit"]
    for net, (g, _, _) in PIN_MAP.items():
        assert gpios(DEVKIT, DEVKITM_J1, DEVKITM_J3, net) == [str(g)]
        if ("MCU", f"GPIO{g}") not in bench.BENCH.omit:
            assert gpios(bench.BENCH, bench.DEVKITC_J1, bench.DEVKITC_J3, net) == [str(g)]


def test_no_bench_hole_takes_two_wires():
    holes = [e for w in bench.wire_plan() for e in (w.a, w.b) if isinstance(e, bench.Hole)]
    assert len(holes) == len(set(holes))


def test_gpios_are_used_once_and_reserved_ones_on_purpose():
    shared = {g for g, _, _ in PIN_MAP.values()}
    extra = [g for g, _ in PRODUCT_PINS.values()]
    assert len(extra) == len(set(extra)) and shared.isdisjoint(extra)
    assert shared.isdisjoint(RESERVED_GPIO)
    # the product uses some reserved pins for what they are: straps, USB, BOOT button
    assert {g for g in extra if g in RESERVED_GPIO} == {8, 9, 12, 13, 15}
    assert all(f"GPIO{g}" in MINI1 for g in shared | set(extra))


def test_devkit_wires_join_exactly_the_net_list():
    assert plan_connectivity() == DEVKIT.connectivity()


def test_no_breadboard_hole_takes_two_wires_in_the_plan():
    holes = [e for w in wire_plan() for e in (w.a, w.b) if isinstance(e, Hole)]
    assert len(holes) == len(set(holes))


@pytest.mark.parametrize("path", list(DRAWN), ids=lambda p: p.name)
def test_committed_drawing_is_current(path):
    """The drawings and SETUP.md, as build_wiring.py would write them now."""
    assert path.read_text() == DRAWN[path](), "run: uv run build_wiring.py"


def test_setup_guide_images_exist():
    import re
    for img in re.findall(r"!\[[^]]*\]\(([^)]+)\)", (HERE / "SETUP.md").read_text()):
        assert (HERE / img).exists(), img


@needs_kicad
def test_committed_schematic_matches_the_net_list():
    """KiCad's own reading of the schematic, whether regenerated or edited by hand in KiCad."""
    assert K.export_netlist(SCH) == {n: set(p) for n, p in PRODUCT.nets().items()}, "run: uv run build_wiring.py"


@needs_kicad
def test_schematic_passes_erc(tmp_path):
    code, report = K.erc(SCH, tmp_path / "erc.rpt")
    assert code == 0, report
