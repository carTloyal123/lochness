#!/usr/bin/env python3
"""Type console commands into a running board and print what it answers.

    send.py PORT [--wait S] CMD [CMD ...]

The port is opened without touching DTR/RTS so the board is NOT reset
(pyserial's default open leaves both asserted, which the DevKit's auto-reset
circuit ignores; dropping only one of them pulls EN low and reboots it).
"""
import argparse
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--wait", type=float, default=1.5, help="seconds to keep reading after each command")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=0.05) as port:
        port.reset_input_buffer()
        for command in args.commands:
            port.write(command.encode() + b"\n")
            end = time.monotonic() + args.wait
            pending = ""
            while time.monotonic() < end:
                pending += port.read(4096).decode(errors="replace")
                *lines, pending = pending.split("\n")
                for line in lines:
                    line = ANSI.sub("", line).replace("\x1b[5n", "").rstrip()
                    if line:
                        print(line, flush=True)
            if pending.strip():
                print(ANSI.sub("", pending).rstrip(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
