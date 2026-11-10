#!/usr/bin/env python3
"""List the boards plugged in and say which app each one runs.

    boards.py            # every USB serial port
    boards.py --no-probe # ports only, don't talk to the boards

Each port is asked for its console `help` (opened without touching DTR/RTS,
so no board is reset) and the app is told apart by a command only it has.
A port another program holds (idf.py monitor, say) shows as "busy".
"""
import argparse
import re
import sys
import time

import serial
from serial.tools import list_ports

# A console command only that app registers -> app name.
SIGNATURES = (("matter", "hub"), ("keys", "keypad"), ("remote", "lock_unit"))
COMMAND = re.compile(r"^([a-z_]+)\b", re.MULTILINE)


def probe(device: str, wait: float) -> str:
    try:
        with serial.Serial(device, 115200, timeout=0.05, exclusive=True) as port:
            port.reset_input_buffer()
            port.write(b"help\n")
            out, end = "", time.monotonic() + wait
            while time.monotonic() < end:
                out += port.read(4096).decode(errors="replace")
    except (serial.SerialException, OSError):
        return "busy"
    commands = set(COMMAND.findall(out))
    for command, app in SIGNATURES:
        if command in commands:
            return app
    return "no console answer" if not out.strip() else "unknown"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-probe", action="store_true", help="list ports without talking to the boards")
    ap.add_argument("--wait", type=float, default=1.0, help="seconds to wait for each board to answer")
    args = ap.parse_args()

    ports = sorted((p for p in list_ports.comports() if p.vid is not None), key=lambda p: p.device)
    if not ports:
        print("no USB serial devices (check cable, hub port power, and the board's UART connector)")
        return 1
    for p in ports:
        app = "" if args.no_probe else probe(p.device, args.wait)
        print(f"{p.device:28} {app:18} {p.description} [{p.serial_number or '-'}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
