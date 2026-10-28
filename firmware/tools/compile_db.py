#!/usr/bin/env python3
"""Merge the firmware's compile databases into firmware/compile_commands.json.

clangd and clang-tidy need one database covering the lock app, the keypad
app and the host tests. ESP-IDF's databases name riscv32-esp-elf-gcc, pass
GCC-only flags and get libc headers from a GCC specs file, none of which clang
understands. This script rewrites each entry so any recent clang can parse it:

  ESP entries:  --target=riscv32-esp-elf, -nostdlibinc plus the libc include
                dirs GCC reports, GCC-only flags dropped.
  Host entries: -arch X becomes --target=X-apple-macosx with the SDK sysroot,
                so a cross clang (esp-clang) still parses them as host code.

Only files in this repo are kept (not ESP-IDF's own sources), so clangd indexes
our code and reads IDF through its headers. When a file appears in several
builds, the first build listed wins: lock_unit, then keypad, then host tests.

  firmware/tools/compile_db.py              # merge existing build dirs
  firmware/tools/compile_db.py --configure  # (re)configure the builds first
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parent.parent
IDF_PATH = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf-v6.0.2"))

# Priority order: the first build that compiles a file supplies its command.
ESP_APPS = ["apps/lock_unit", "apps/keypad"]
HOST_BUILD = "test/host/build"

# GCC flags clang rejects or warns about. Matched exactly or, when ending in
# '=', by prefix.
GCC_ONLY = {
    "-fno-shrink-wrap",
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-fzero-init-padding-bits=",
    "-fno-malloc-dce",
    "-specs=",
    "-fdiagnostics-color=",
    "-mlongcalls",
    "-mdisable-hardware-atomics",
    "-Wno-old-style-declaration",
}
# Builds enforce -Werror; in the editor warnings should stay warnings.
DROP = {"-Werror"}


def drop_flag(arg: str) -> bool:
    if arg in DROP:
        return True
    return any(arg.startswith(f) if f.endswith("=") else arg == f for f in GCC_ONLY)


def argv_of(entry: dict) -> list[str]:
    argv = list(entry["arguments"]) if "arguments" in entry else shlex.split(entry["command"])
    # ESP-IDF moves -march/-specs into @response files; inline them so they
    # can be inspected and filtered like any other flag.
    out = []
    for a in argv:
        if a.startswith("@"):
            rsp = Path(entry["directory"], a[1:])
            out += shlex.split(rsp.read_text()) if rsp.is_file() else [a]
        else:
            out.append(a)
    return out


def run(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL)


def configure() -> None:
    env_sh = IDF_PATH / "export.sh"
    for app in ESP_APPS:
        # export.sh sets up PATH and the Python env; reconfigure is enough to
        # emit compile_commands.json without compiling anything.
        script = f'. "{env_sh}" >/dev/null 2>&1 && idf.py reconfigure >/dev/null'
        print(f"+ idf.py reconfigure ({app})", file=sys.stderr)
        subprocess.run(["bash", "-c", script], cwd=FIRMWARE / app, check=True)
    run(["cmake", "-S", ".", "-B", "build", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], FIRMWARE / "test/host")


_gcc_includes: dict[tuple[str, ...], list[str]] = {}


def gcc_system_includes(driver: str, probe_flags: list[str]) -> list[str]:
    """Ask the GCC driver which system include dirs it would use."""
    key = (driver, *probe_flags)
    if key not in _gcc_includes:
        out = subprocess.run(
            [driver, *probe_flags, "-E", "-x", "c", "-", "-v"],
            input="", capture_output=True, text=True, check=True,
        ).stderr
        dirs, inside = [], False
        for line in out.splitlines():
            if line.startswith("#include <...>"):
                inside = True
            elif line.startswith("End of search list"):
                break
            elif inside:
                dirs.append(os.path.normpath(line.strip()))
        _gcc_includes[key] = dirs
    return _gcc_includes[key]


def rewrite_esp(argv: list[str]) -> list[str]:
    driver = argv[0]
    target = Path(driver).name.removesuffix("-gcc").removesuffix("-g++")
    # Flags that change GCC's include search; probe with exactly these.
    probe = [a for a in argv if a.startswith(("-specs=", "-march=", "-mabi="))]
    sysdirs = [
        d for d in gcc_system_includes(driver, probe)
        # GCC's private headers (stddef.h, stdatomic.h...) clash with clang's
        # own; clang supplies those from its resource dir.
        if "/lib/gcc/" not in d
    ]
    out = ["clang", f"--target={target}", "-nostdlibinc"]
    for d in sysdirs:
        out += ["-isystem", d]
    out += [a for a in argv[1:] if not drop_flag(a)]
    return out


_sdk: str | None = None


def rewrite_host(argv: list[str]) -> list[str]:
    global _sdk
    out, i = [argv[0]], 1
    while i < len(argv):
        if argv[i] == "-arch" and i + 1 < len(argv):
            out.append(f"--target={argv[i + 1]}-apple-macosx")
            i += 2
            continue
        if not drop_flag(argv[i]):
            out.append(argv[i])
        i += 1
    if sys.platform == "darwin" and "-isysroot" not in out:
        if _sdk is None:
            _sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                                  text=True, check=True).stdout.strip()
        out[1:1] = ["-isysroot", _sdk]
    return out


def in_repo(path: Path) -> bool:
    try:
        rel = path.relative_to(FIRMWARE)
    except ValueError:
        return False
    return not any(p == "build" or p.startswith("build-") for p in rel.parts)


def load(db: Path, rewrite) -> list[dict]:
    if not db.is_file():
        print(f"warning: {db.relative_to(FIRMWARE)} missing; run with --configure", file=sys.stderr)
        return []
    entries = []
    for e in json.loads(db.read_text()):
        file = Path(e["directory"], e["file"]).resolve()
        if not in_repo(file):
            continue
        entries.append({
            "directory": e["directory"],
            "file": str(file),
            "arguments": rewrite(argv_of(e)),
        })
    return entries


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--configure", action="store_true",
                    help="run idf.py reconfigure for each app and configure the host tests first")
    args = ap.parse_args()
    if args.configure:
        configure()

    merged: dict[str, dict] = {}
    sources = [(FIRMWARE / app / "build/compile_commands.json", rewrite_esp) for app in ESP_APPS]
    sources.append((FIRMWARE / HOST_BUILD / "compile_commands.json", rewrite_host))
    for db, rewrite in sources:
        for e in load(db, rewrite):
            merged.setdefault(e["file"], e)

    if not merged:
        print("error: no compile commands found", file=sys.stderr)
        return 1
    out = FIRMWARE / "compile_commands.json"
    out.write_text(json.dumps(sorted(merged.values(), key=lambda e: e["file"]), indent=1) + "\n")
    print(f"wrote {out.relative_to(FIRMWARE.parent)} ({len(merged)} files)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
