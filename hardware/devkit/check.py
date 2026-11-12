"""Assembly checks for the dev kit: the same as the concepts'.

    uv run python -m devkit.check
"""

from __future__ import annotations

from concepts.check import collisions, fasteners, fit, joint_gaps, snaps, unattached
from deadbolt.fixture import GENERIC_US, DeadboltParams
from devkit.rig import rig


def check(p: DeadboltParams = GENERIC_US, locked: bool = True) -> dict[str, list[str]]:
    u, fixture = rig(p, locked)
    return {"collisions": collisions(u, fixture), "joints": joint_gaps(u, fixture),
            "unattached": unattached(u), "fit": fit(u, p), "fasteners": fasteners(u), "snaps": snaps(u)}


def main() -> int:
    bad = 0
    for locked in (True, False):
        for kind, problems in check(locked=locked).items():
            for msg in problems:
                bad += 1
                print(f"{'locked' if locked else 'unlocked'} {kind}: {msg}")
    print("ok" if not bad else f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
