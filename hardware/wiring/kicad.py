"""Write a Stage's net list as a KiCad schematic, and read KiCad's net list back.

The schematic is label-connected: each part pin gets a short wire stub ending in its net's
label (or power symbol), so parts can sit anywhere and the drawing stays readable. Symbols
come from KiCad's stock libraries, plus the project library for the two chips it lacks.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import uuid as _uuid
from dataclasses import dataclass
from pathlib import Path

from .sexpr import Sym, dumps, find, find_all, fmt, loads

GRID = 1.27
STUB = 2.54


def kicad_cli() -> str:
    found = shutil.which("kicad-cli")
    for p in (found, "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"):
        if p and Path(p).exists():
            return p
    raise FileNotFoundError("kicad-cli not found: install KiCad 10")


def stock_symbols_dir() -> Path:
    env = os.environ.get("KICAD10_SYMBOL_DIR") or os.environ.get("KICAD_SYMBOL_DIR")
    for p in (env, "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols",
              "/usr/share/kicad/symbols", "/usr/local/share/kicad/symbols"):
        if p and Path(p).is_dir():
            return Path(p)
    raise FileNotFoundError("KiCad's symbol libraries not found: set KICAD10_SYMBOL_DIR")


_LIBS: dict[Path, list] = {}


def _lib(path: Path) -> list:
    if path not in _LIBS:
        _LIBS[path] = loads(path.read_text())
    return _LIBS[path]


def lib_symbol(lib_id: str, project_lib: Path) -> list:
    """The symbol as a schematic embeds it: named 'Lib:Name', derived symbols flattened."""
    lib, name = lib_id.split(":")
    path = project_lib if lib == project_lib.stem else stock_symbols_dir() / f"{lib}.kicad_sym"
    syms = {s[1]: s for s in find_all(_lib(path), "symbol")}
    sym = syms[name]
    ext = find(sym, "extends")
    if ext:
        parent = syms[ext[1]]
        props = {p[1]: p for p in find_all(sym, "property")}
        body = []
        for c in parent[2:]:
            if isinstance(c, list) and c[0] == "property":
                body.append(props.pop(c[1], c))
            elif isinstance(c, list) and c[0] == "symbol":
                body.append(["symbol", c[1].replace(parent[1], name, 1), *c[2:]])
            else:
                body.append(c)
        body += list(props.values())
        sym = ["symbol", name, *body]
    return ["symbol", f"{lib}:{name}", *sym[2:]]


@dataclass
class PinGeo:
    number: str
    name: str
    etype: str
    x: float  # in the symbol's frame, y up
    y: float
    angle: int


def pins_of(sym: list) -> list[PinGeo]:
    out = []
    for unit in find_all(sym, "symbol"):
        _, u, style = unit[1].rsplit("_", 2)
        if u not in ("0", "1") or style not in ("0", "1"):
            continue
        for p in find_all(unit, "pin"):
            at = find(p, "at")
            out.append(PinGeo(find(p, "number")[1], find(p, "name")[1], str(p[1]),
                              float(at[1]), float(at[2]), int(float(at[3])) if len(at) > 3 else 0))
    return out


def place(x: float, y: float, rot: int, px: float, py: float) -> tuple[float, float]:
    """A symbol-frame point (y up) on the sheet (y down), for a symbol at (x, y) turned rot
    degrees counter-clockwise."""
    sx, sy = px, -py
    for _ in range((rot // 90) % 4):
        sx, sy = sy, -sx
    return round(x + sx, 4), round(y + sy, 4)


def outward(angle: int, rot: int) -> tuple[float, float]:
    """Unit vector on the sheet pointing away from the body at a pin."""
    lx, ly = {0: (-1, 0), 90: (0, -1), 180: (1, 0), 270: (0, 1)}[angle % 360]
    a = place(0, 0, rot, lx, ly)
    return a[0], a[1]


def _uid(*parts: str) -> str:
    """Stable UUIDs, so regenerating an unchanged schematic changes nothing."""
    return str(_uuid.uuid5(_uuid.NAMESPACE_URL, "lochness/" + "/".join(parts)))


def _effects(size=1.27, hide=False, justify=None):
    e = ["effects", ["font", ["size", size, size]]]
    if justify:
        e.append(["justify", Sym(justify)])
    if hide:
        e.append(["hide", Sym("yes")])
    return e


def _prop(name, value, x, y, hide=False, angle=0, justify=None):
    return ["property", name, value, ["at", x, y, angle], _effects(hide=hide, justify=justify)]


@dataclass
class Placed:
    ref: str
    lib_id: str
    value: str
    footprint: str
    x: float
    y: float
    rot: int = 0
    fields: dict | None = None
    power: bool = False


class Sheet:
    def __init__(self, project: str, title: str, project_lib: Path, paper: str = "A2"):
        self.project, self.title, self.project_lib, self.paper = project, title, project_lib, paper
        self.root = _uid(project, "root")
        self.items: list = []
        self.lib: dict[str, list] = {}
        self.pwr_n = 0

    def _symbol(self, p: Placed) -> list[PinGeo]:
        if p.lib_id not in self.lib:
            self.lib[p.lib_id] = lib_symbol(p.lib_id, self.project_lib)
        sym = self.lib[p.lib_id]
        pins = pins_of(sym)
        hide_ref = p.power
        ref_xy = (p.x, p.y - 6.35) if not p.power else (p.x, p.y)
        e = ["symbol", ["lib_id", p.lib_id], ["at", p.x, p.y, p.rot], ["unit", 1],
             ["exclude_from_sim", Sym("no")], ["in_bom", Sym("no" if p.power else "yes")],
             ["on_board", Sym("no" if p.power else "yes")], ["dnp", Sym("no")],
             ["uuid", _uid(self.project, "sym", p.ref)],
             _prop("Reference", p.ref, *ref_xy, hide=hide_ref),
             _prop("Value", p.value, p.x, p.y + (3.81 if p.power and p.rot == 0 else 6.35)),
             _prop("Footprint", p.footprint, p.x, p.y, hide=True),
             _prop("Datasheet", "", p.x, p.y, hide=True)]
        for k, v in (p.fields or {}).items():
            e.append(_prop(k, v, p.x, p.y, hide=True))
        for pin in pins:
            e.append(["pin", pin.number, ["uuid", _uid(self.project, "pin", p.ref, pin.number)]])
        e.append(["instances", ["project", self.project, ["path", f"/{self.root}", ["reference", p.ref], ["unit", 1]]]])
        self.items.append(e)
        return pins

    def part(self, p: Placed, pin_nets: dict[str, str], power_nets: dict[str, str], label_size=1.27):
        """Place a part, and a stub plus a label or power symbol on each pin with a net; a
        no-connect flag on each pin without one. Stacked pins (a module's many GNDs) share
        one stub, so they must share a net."""
        done: dict[tuple[float, float], str | None] = {}
        for pin in self._symbol(p):
            px, py = place(p.x, p.y, p.rot, pin.x, pin.y)
            net = pin_nets.get(pin.number)
            if (px, py) in done:
                if done[(px, py)] != net:
                    raise ValueError(f"{p.ref}: stacked pins at {px, py} on {done[(px, py)]} and {net}")
                continue
            done[(px, py)] = net
            if net is None:
                if pin.etype != "no_connect":
                    self.items.append(["no_connect", ["at", px, py], ["uuid", _uid(self.project, "nc", p.ref, pin.number)]])
                continue
            dx, dy = outward(pin.angle, p.rot)
            ex, ey = round(px + dx * STUB, 4), round(py + dy * STUB, 4)
            self.items.append(["wire", ["pts", ["xy", px, py], ["xy", ex, ey]],
                               ["stroke", ["width", 0], ["type", Sym("default")]],
                               ["uuid", _uid(self.project, "w", p.ref, pin.number)]])
            self.terminal(net, ex, ey, (dx, dy), power_nets, f"{p.ref}.{pin.number}", label_size)

    def terminal(self, net, x, y, d, power_nets, key, size=1.27):
        d = (round(d[0]), round(d[1]))
        lib_id = power_nets.get(net, "")
        # Power symbols only where they read naturally: ground hanging down, supplies pointing
        # up. Anywhere else the net gets a label of the same name, which KiCad joins to it.
        if lib_id and d == ((0, 1) if lib_id.endswith("GND") else (0, -1)):
            self.pwr_n += 1
            self.power(lib_id, net, x, y, 0)
        else:
            angle = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[d]
            just = "left" if angle in (0, 90) else "right"
            self.items.append(["label", net, ["at", x, y, angle],
                               _effects(size, justify=f"{just} bottom"),
                               ["uuid", _uid(self.project, "lbl", key)]])

    def power(self, lib_id, net, x, y, rot=0):
        self._symbol(Placed(f"#PWR{self.pwr_n:03d}", lib_id, net, "", x, y, rot, power=True))

    def flag(self, net: str, x: float, y: float, power_nets: dict[str, str]):
        """A PWR_FLAG on a net (its pin at x, y), joined to the net by a label or power symbol."""
        self.pwr_n += 1
        self._symbol(Placed(f"#FLG{self.pwr_n:03d}", "power:PWR_FLAG", "PWR_FLAG", "", x, y, 0, power=True))
        self.items.append(["wire", ["pts", ["xy", x, y], ["xy", x, y + STUB]],
                           ["stroke", ["width", 0], ["type", Sym("default")]], ["uuid", _uid(self.project, "flagw", net)]])
        self.terminal(net, x, y + STUB, (0, 1), power_nets, f"flag.{net}")

    def text(self, s: str, x: float, y: float, size: float = 1.27, key: str = ""):
        self.items.append(["text", s, ["exclude_from_sim", Sym("no")], ["at", x, y, 0],
                           _effects(size, justify="left top"), ["uuid", _uid(self.project, "txt", key or s[:40])]])

    def box(self, x0, y0, x1, y1, key):
        self.items.append(["rectangle", ["start", x0, y0], ["end", x1, y1],
                           ["stroke", ["width", 0], ["type", Sym("dash")]], ["fill", ["type", Sym("none")]],
                           ["uuid", _uid(self.project, "box", key)]])

    def write(self, path: Path, rev: str = "", date: str = "", comments: list[str] = ()):
        tb = ["title_block", ["title", self.title]]
        if date:
            tb.append(["date", date])
        if rev:
            tb.append(["rev", rev])
        for i, c in enumerate(comments, 1):
            tb.append(["comment", i, c])
        doc = ["kicad_sch", ["version", 20250114], ["generator", "lochness_wiring"], ["generator_version", "10.0"],
               ["uuid", self.root], ["paper", self.paper], tb,
               ["lib_symbols", *self.lib.values()], *self.items,
               ["sheet_instances", ["path", "/", ["page", "1"]]], ["embedded_fonts", Sym("no")]]
        path.write_text(dumps(_numbers(doc)) + "\n")


def _numbers(e):
    """Floats -> KiCad number text."""
    if isinstance(e, list):
        return [_numbers(c) for c in e]
    if isinstance(e, float) or (isinstance(e, int) and not isinstance(e, bool)):
        return Sym(fmt(e))
    return e


def write_symbol_lib(path: Path, symbols: list[list]):
    doc = ["kicad_symbol_lib", ["version", 20251024], ["generator", "lochness_wiring"],
           ["generator_version", "10.0"], *symbols]
    path.write_text(dumps(_numbers(doc)) + "\n")


def ic_symbol(name: str, ref: str, desc: str, footprint: str, datasheet: str,
              left: list[tuple[str, str, str]], right: list[tuple[str, str, str]], width: float = 15.24) -> list:
    """A box symbol: pins (number, name, electrical type) down each side, 2.54 apart."""
    n = max(len(left), len(right))
    h = (n + 1) * 2.54
    top = round((n - 1) / 2 * 2.54 / GRID) * GRID + 2.54
    hw = width / 2
    pins = []
    for side, rows, x, ang in (("l", left, -hw - 2.54, 0), ("r", right, hw + 2.54, 180)):
        for k, (num, pname, etype) in enumerate(rows):
            pins.append(["pin", Sym(etype), Sym("line"), ["at", x, top - 2.54 - k * 2.54, ang], ["length", 2.54],
                         ["name", pname, _effects()], ["number", num, _effects()]])
    props = [_prop("Reference", ref, 0, top + 1.27), _prop("Value", name, 0, top - h - 1.27),
             _prop("Footprint", footprint, 0, 0, hide=True), _prop("Datasheet", datasheet, 0, 0, hide=True),
             _prop("Description", desc, 0, 0, hide=True)]
    body = ["symbol", f"{name}_0_1", ["rectangle", ["start", -hw, top], ["end", hw, top - h],
                                      ["stroke", ["width", 0.254], ["type", Sym("default")]],
                                      ["fill", ["type", Sym("background")]]]]
    return ["symbol", name, ["exclude_from_sim", Sym("no")], ["in_bom", Sym("yes")], ["on_board", Sym("yes")],
            *props, body, ["symbol", f"{name}_1_1", *pins], ["embedded_fonts", Sym("no")]]


def export_netlist(sch: Path) -> dict[str, set[tuple[str, str]]]:
    """KiCad's own net list for the schematic: net -> {(ref, pin)}, power symbols left out."""
    out = sch.with_suffix(".net")
    subprocess.run([kicad_cli(), "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out), str(sch)],
                   check=True, capture_output=True)
    doc = loads(out.read_text())
    out.unlink()
    nets = {}
    for n in find_all(find(doc, "nets"), "net"):
        name = find(n, "name")[1].lstrip("/")
        pins = {(find(nd, "ref")[1], find(nd, "pin")[1]) for nd in find_all(n, "node")
                if not find(nd, "ref")[1].startswith("#")}
        if pins and not name.startswith("unconnected-"):  # KiCad's one-pin nets for no-connects
            nets[name] = pins
    return nets


def erc(sch: Path, report: Path) -> tuple[int, str]:
    r = subprocess.run([kicad_cli(), "sch", "erc", "--severity-error", "--exit-code-violations", "-o", str(report), str(sch)],
                       capture_output=True, text=True)
    return r.returncode, report.read_text() if report.exists() else r.stdout + r.stderr
