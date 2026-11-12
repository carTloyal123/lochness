"""Minimal S-expression reader and writer for KiCad files.

Atoms come back as `Sym` (bare words) or `str` (quoted strings), so a file read and written
again keeps its quoting. Numbers stay as `Sym` text.
"""

from __future__ import annotations

import re


class Sym(str):
    """A bare word: written without quotes."""


_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


def loads(text: str) -> list:
    stack: list[list] = [[]]
    pos = 0
    while True:
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        if m.group(1):
            stack.append([])
        elif m.group(2):
            done = stack.pop()
            stack[-1].append(done)
        elif m.group(3) is not None:
            stack[-1].append(m.group(3).replace('\\"', '"').replace("\\\\", "\\"))
        else:
            stack[-1].append(Sym(m.group(4)))
    if len(stack) != 1:
        raise ValueError("unbalanced parentheses")
    return stack[0][0]


def _atom(a) -> str:
    if isinstance(a, Sym):
        return str(a)
    if isinstance(a, bool):
        return "yes" if a else "no"
    if isinstance(a, (int, float)):
        return fmt(a)
    s = str(a).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return f'"{s}"'


def fmt(v: float) -> str:
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def dumps(e, indent: int = 0) -> str:
    """A list's head is its keyword, always written bare."""
    if not isinstance(e, list):
        return _atom(e)
    if e and isinstance(e[0], str) and not isinstance(e[0], Sym):
        e = [Sym(e[0]), *e[1:]]
    if not any(isinstance(c, list) for c in e):
        return "(" + " ".join(_atom(c) for c in e) + ")"
    pad = "\t" * (indent + 1)
    head = [c for c in e if not isinstance(c, list)]
    out = "(" + " ".join(_atom(c) for c in head)
    for c in e[len(head):]:
        out += "\n" + pad + (dumps(c, indent + 1) if isinstance(c, list) else _atom(c))
    return out + "\n" + "\t" * indent + ")"


def find(e: list, key: str) -> list | None:
    return next((c for c in e if isinstance(c, list) and c and c[0] == key), None)


def find_all(e: list, key: str) -> list[list]:
    return [c for c in e if isinstance(c, list) and c and c[0] == key]
