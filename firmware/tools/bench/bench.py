#!/usr/bin/env python3
"""Two-board bench tests: lock_unit and keypad on real hardware over BLE.

Drives both serial consoles, times keypad requests end to end, and checks the
security behaviour that only shows up on real radios and real reboots.

    python3 firmware/tools/bench/bench.py --lock /dev/cu.usbserial-210 \\
        --keypad /dev/cu.usbserial-110 [--cycles 10] [--only latency,lockout]

Needs pyserial (in the ESP-IDF Python environment: run it via ./esp.sh's
environment or `pip install pyserial`). By default it factory-resets the lock
and re-pairs the keypad first; --keep skips that and uses the existing setup
(user 1 with PIN 1234 and a paired keypad).

Timing: "host" is measured from writing the command to the keypad's serial
port until the keypad logs its feedback, so it includes about 1-2 ms of UART
time. "link" and "result" come from the keypad's own clock: BLE connect plus
GATT setup, and connect until the lock's answer.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field

import serial

PIN = "1234"
WRONG_PIN = "9999"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
FEEDBACK = re.compile(
    r"feedback: (UNLOCKED|LOCKED|denied|locked out|refused[^\r\n]*|lock unreachable|PAIRED|pairing failed|not paired)"
)
TIMING = re.compile(r"timing: link (-?\d+) ms, result (\d+) ms after connect")
LINGER_S = 3.6  # keypad keeps the link 3 s after a result (KP_LINGER_MS)


class Board:
    """One serial console with a timestamped line log."""

    def __init__(self, name: str, port: str, ready: str):
        self.name = name
        self.ready = ready
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.lines: list[tuple[float, str]] = []
        self.cv = threading.Condition()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        buf = b""
        while True:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = ANSI.sub("", raw.decode(errors="replace")).rstrip("\r")
                with self.cv:
                    self.lines.append((time.monotonic(), text))
                    self.cv.notify_all()

    def mark(self) -> int:
        with self.cv:
            return len(self.lines)

    def send(self, cmd: str) -> float:
        t = time.monotonic()
        self.ser.write(cmd.encode() + b"\r\n")
        self.ser.flush()
        return t

    def wait(self, pattern: str | re.Pattern, timeout: float, since: int) -> tuple[float, re.Match] | None:
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        i = since
        with self.cv:
            while True:
                while i < len(self.lines):
                    t, text = self.lines[i]
                    i += 1
                    m = rx.search(text)
                    if m:
                        return t, m
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def cmd(self, cmd: str, settle: float = 0.6) -> list[str]:
        """Run a console command and return the lines it printed."""
        start = self.mark()
        self.send(cmd)
        time.sleep(settle)
        with self.cv:
            return [text for _, text in self.lines[start:]]

    def reset(self, timeout: float = 8) -> bool:
        """Hardware reset through the UART bridge (EN), wait for the app."""
        start = self.mark()
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False
        ok = self.wait(self.ready, timeout, start) is not None
        time.sleep(0.8)  # console prompt
        return ok


@dataclass
class Result:
    name: str
    ok: bool
    detail: str = ""
    data: dict = field(default_factory=dict)


def summarize(samples: list[float]) -> dict:
    if not samples:
        return {}
    s = sorted(samples)
    p95 = s[min(len(s) - 1, round(0.95 * (len(s) - 1)))]
    return {
        "n": len(s),
        "min": round(s[0], 1),
        "median": round(statistics.median(s), 1),
        "p95": round(p95, 1),
        "max": round(s[-1], 1),
    }


def fmt(stats: dict) -> str:
    if not stats:
        return "no samples"
    return f"n={stats['n']} min {stats['min']} / median {stats['median']} / p95 {stats['p95']} / max {stats['max']} ms"


class Bench:
    def __init__(self, lock: Board, keypad: Board, cycles: int):
        self.lock = lock
        self.kp = keypad
        self.cycles = cycles
        self.results: list[Result] = []

    # ---- helpers ------------------------------------------------------------

    def keypad_request(self, keys: str, timeout: float = 12) -> dict:
        """Send `keys` to the keypad; return feedback, host ms, and keypad timing."""
        start = self.kp.mark()
        t0 = self.kp.send(f"keys {keys}")
        got = self.kp.wait(FEEDBACK, timeout, start)
        if not got:
            return {"feedback": None}
        t1, m = got
        out = {"feedback": m.group(1), "host_ms": (t1 - t0) * 1000}
        timing = self.kp.wait(TIMING, 0.3, start)
        if timing:
            out["link_ms"] = int(timing[1].group(1))
            out["result_ms"] = int(timing[1].group(2))
        return out

    def lock_status(self) -> dict:
        info = {}
        for line in self.lock.cmd("status"):
            m = re.match(r"\s*(?:lochness> )?(bolt|door|keypad|time|boot)\s+(.*)", line)
            if m:
                info[m.group(1)] = m.group(2).strip()
        return info

    def ensure_bolt(self, want: str) -> None:
        if self.lock_status().get("bolt") != want:
            self.lock.cmd("app " + ("lock" if want == "locked" else "unlock"), settle=1.2)

    def record(self, r: Result) -> None:
        self.results.append(r)
        mark = "PASS" if r.ok else "FAIL"
        print(f"{mark} {r.name}: {r.detail}", flush=True)

    # ---- setup ----------------------------------------------------------------

    def setup_fresh(self) -> bool:
        print("setup: factory-reset lock, re-pair keypad", flush=True)
        start = self.lock.mark()
        self.lock.send("factory-reset confirm")
        if not self.lock.wait(self.lock.ready, 10, start):
            return False
        time.sleep(0.8)
        self.kp.cmd("forget confirm")
        self.lock.cmd("user add Alice")
        self.lock.cmd(f"code add 1 {PIN}")
        self.lock.cmd("pair keypad")
        start = self.kp.mark()
        self.kp.send("pair")
        got = self.kp.wait(FEEDBACK, 40, start)
        ok = bool(got) and got[1].group(1) == "PAIRED"
        self.record(Result("setup", ok, "paired" if ok else f"pairing: {got[1].group(1) if got else 'timeout'}"))
        time.sleep(1)
        return ok

    # ---- tests ------------------------------------------------------------------

    def test_latency_cold(self) -> None:
        """Every request starts with no link: the one-touch-lock worst case."""
        host, link, result, bad = [], [], [], []
        for _ in range(self.cycles):
            for keys, want in ((f"{PIN}#", "UNLOCKED"), ("L", "LOCKED")):
                time.sleep(LINGER_S)  # previous link must be gone
                r = self.keypad_request(keys)
                if r.get("feedback") != want:
                    bad.append(f"{keys}->{r.get('feedback')}")
                    continue
                host.append(r["host_ms"])
                if "link_ms" in r:
                    link.append(r["link_ms"])
                    result.append(r["result_ms"])
        stats = {"host": summarize(host), "link": summarize(link), "result": summarize(result)}
        ok = not bad
        self.record(
            Result(
                "latency_cold",
                ok,
                f"host {fmt(stats['host'])}; link {fmt(stats['link'])}" + (f"; failures {bad}" if bad else ""),
                stats,
            )
        )

    def test_latency_typed(self) -> None:
        """Digits 300 ms apart, as a person types: the link comes up during entry."""
        host, bad = [], []
        for _ in range(max(3, self.cycles // 2)):
            time.sleep(LINGER_S)
            self.ensure_bolt("locked")
            time.sleep(LINGER_S)
            for digit in PIN:
                self.kp.send(f"keys {digit}")
                time.sleep(0.3)
            r = self.keypad_request("#")
            if r.get("feedback") != "UNLOCKED":
                bad.append(r.get("feedback"))
                continue
            host.append(r["host_ms"])
        stats = summarize(host)
        self.record(
            Result("latency_typed", not bad, f"enter-to-result {fmt(stats)}" + (f"; failures {bad}" if bad else ""), {"host": stats})
        )
        self.ensure_bolt("locked")

    def test_latency_warm(self) -> None:
        """Second request while the link from the first is still up."""
        host, bad = [], []
        for _ in range(max(3, self.cycles // 2)):
            time.sleep(LINGER_S)
            first = self.keypad_request(f"{PIN}#")
            time.sleep(1.0)  # simulated motor runs 800 ms; the lock answers busy until then
            second = self.keypad_request("L")
            if first.get("feedback") != "UNLOCKED" or second.get("feedback") != "LOCKED":
                bad.append(f"{first.get('feedback')}/{second.get('feedback')}")
                continue
            host.append(second["host_ms"])
        stats = summarize(host)
        self.record(
            Result("latency_warm", not bad, f"request on open link {fmt(stats)}" + (f"; failures {bad}" if bad else ""), {"host": stats})
        )

    def test_lockout(self) -> None:
        """Wrong PINs lock the keypad out, a reboot does not clear it, it expires."""
        lockout_s = 8
        self.lock.cmd(f"config lockout {lockout_s}")
        self.ensure_bolt("locked")
        problems = []
        for i in range(5):
            time.sleep(0.2)
            r = self.keypad_request(f"{WRONG_PIN}#")
            if r.get("feedback") != "denied":
                problems.append(f"wrong #{i + 1} -> {r.get('feedback')}")
        r = self.keypad_request(f"{PIN}#")
        if r.get("feedback") != "locked out":
            problems.append(f"correct PIN during lockout -> {r.get('feedback')}")
        if not self.lock.reset():
            problems.append("lock did not reboot")
        st = self.lock_status()
        if "LOCKED OUT" not in st.get("keypad", ""):
            problems.append(f"after reboot keypad state '{st.get('keypad')}'")
        time.sleep(0.5)
        r = self.keypad_request(f"{PIN}#")
        if r.get("feedback") != "locked out":
            problems.append(f"after reboot correct PIN -> {r.get('feedback')}")
        time.sleep(lockout_s + 1.5)
        r = self.keypad_request(f"{PIN}#")
        if r.get("feedback") != "UNLOCKED":
            problems.append(f"after lockout expired -> {r.get('feedback')}")
        self.lock.cmd("config lockout 60")
        self.ensure_bolt("locked")
        self.record(Result("lockout", not problems, "; ".join(problems) or "5 wrong -> locked out, survives reboot, expires"))

    def test_lock_reboot_recovery(self) -> None:
        """Lock reboots mid-session: keypad must recover on its next request."""
        time.sleep(LINGER_S)
        r1 = self.keypad_request(f"{PIN}#")
        start = self.lock.mark()
        self.lock.ser.dtr = False
        self.lock.ser.rts = True
        time.sleep(0.1)
        self.lock.ser.rts = False
        booted = self.lock.wait(self.lock.ready, 8, start)
        t_boot = booted[0] if booted else None
        r2 = self.keypad_request("L", timeout=15)
        t_done = time.monotonic()
        ok = r1.get("feedback") == "UNLOCKED" and r2.get("feedback") == "LOCKED"
        detail = f"before {r1.get('feedback')}, after reboot {r2.get('feedback')}"
        if ok and t_boot:
            detail += f", locked {round((t_done - t_boot) * 1000)} ms after lock booted"
        self.record(Result("lock_reboot_recovery", ok, detail))

    def test_keypad_reboot_mid_session(self) -> None:
        """Keypad dies with the link up: the lock must free the connection."""
        time.sleep(LINGER_S)
        self.kp.send("keys 12")  # opens a link, then the keypad vanishes
        time.sleep(1.0)
        self.kp.reset()
        r = self.keypad_request(f"{PIN}#")
        self.ensure_bolt("locked")
        ok = r.get("feedback") == "UNLOCKED"
        self.record(Result("keypad_reboot_mid_session", ok, f"next request after keypad reboot: {r.get('feedback')}"))

    def test_persistence(self) -> None:
        """Both boards power-cycle; pairing, users and PINs survive."""
        lock_ok = self.lock.reset()
        kp_ok = self.kp.reset()
        peers = " ".join(self.lock.cmd("peers"))
        kp_status = " ".join(self.kp.cmd("status"))
        time.sleep(LINGER_S)
        r = self.keypad_request(f"{PIN}#")
        self.ensure_bolt("locked")
        ok = lock_ok and kp_ok and "keypad" in peers and "not paired" not in kp_status and r.get("feedback") == "UNLOCKED"
        self.record(Result("persistence", ok, f"lock peers {'ok' if 'keypad' in peers else 'MISSING'}, keypad unlock -> {r.get('feedback')}"))

    def test_battery_and_clock(self) -> None:
        """Battery is reported as unknown (no divider on a DevKit); log entries carry boot numbers."""
        peers = " ".join(self.lock.cmd("peers"))
        log = self.lock.cmd("log 3")
        battery_ok = "battery unknown" in peers
        boot_ok = any(re.search(r"boot \d+\s+\+\d+s", line) for line in log)
        self.record(
            Result(
                "battery_and_clock",
                battery_ok and boot_ok,
                f"peers shows battery {'unknown' if battery_ok else 'WRONG'}; log entries {'carry boot numbers' if boot_ok else 'MISSING boot numbers'}",
            )
        )

    def test_unpaired_window_refused(self) -> None:
        """Keypad pairing scan with no window open must fail (30 s scan)."""
        start = self.kp.mark()
        self.kp.send("pair")
        got = self.kp.wait(FEEDBACK, 40, start)
        fb = got[1].group(1) if got else None
        # The keypad cleared nothing: it still holds the old pairing.
        status = " ".join(self.kp.cmd("status"))
        r = self.keypad_request(f"{PIN}#")
        self.ensure_bolt("locked")
        ok = fb == "pairing failed" and "not paired" not in status and r.get("feedback") == "UNLOCKED"
        self.record(Result("unpaired_window_refused", ok, f"scan -> {fb}; old pairing still works -> {r.get('feedback')}"))


TESTS = [
    "latency_cold",
    "latency_typed",
    "latency_warm",
    "lockout",
    "lock_reboot_recovery",
    "keypad_reboot_mid_session",
    "persistence",
    "battery_and_clock",
    "unpaired_window_refused",
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lock", required=True, help="lock_unit serial port")
    ap.add_argument("--keypad", required=True, help="keypad serial port")
    ap.add_argument("--cycles", type=int, default=10, help="lock/unlock cycles for latency_cold")
    ap.add_argument("--only", help="comma-separated tests: " + ", ".join(TESTS))
    ap.add_argument("--keep", action="store_true", help="skip factory reset and re-pairing")
    ap.add_argument("--json", help="write results to this file")
    args = ap.parse_args()

    lock = Board("lock", args.lock, r"lochness lock unit ready")
    keypad = Board("keypad", args.keypad, r"lochness keypad ready")
    bench = Bench(lock, keypad, args.cycles)
    lock.reset()
    keypad.reset()
    if not args.keep and not bench.setup_fresh():
        print("setup failed", file=sys.stderr)
        return 2
    bench.ensure_bolt("locked")

    selected = args.only.split(",") if args.only else TESTS
    for name in selected:
        if name not in TESTS:
            print(f"unknown test {name}", file=sys.stderr)
            return 2
        getattr(bench, f"test_{name}")()

    failed = [r for r in bench.results if not r.ok]
    print(f"\n{len(bench.results) - len(failed)}/{len(bench.results)} passed")
    if args.json:
        with open(args.json, "w") as f:
            json.dump([r.__dict__ for r in bench.results], f, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
