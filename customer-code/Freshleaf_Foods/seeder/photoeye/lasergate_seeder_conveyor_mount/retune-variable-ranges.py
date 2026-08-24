#!/usr/bin/env python3
"""
retune-variable-ranges.py — update variable_ranges in the live settings file,
without losing the presets already stored in it.

    scp retune-variable-ranges.py rooted@<host>:/tmp/
    ssh -t rooted@<host> 'sudo systemctl stop seeder_poll.service seeder_tcp_server.service; \
        python3 /tmp/retune-variable-ranges.py; \
        sudo systemctl start seeder_poll.service seeder_tcp_server.service'

WHY THIS EXISTS

Re-seeding from TE_Variable_Values.seed.json would set the ranges correctly and
throw away every preset on the machine along with them. This edits the ranges in
place instead, then clamps any stored value that the new, narrower range no
longer permits.

Clamping rather than reporting-and-leaving is deliberate. A stored value outside
the declared range makes audit_stored_presets flag that preset on every single
startup, and save_variety_data refuse to save it, until somebody notices and
re-enters it on the HMI. Since the firmware would clamp the value anyway, the
file may as well say what the machine is actually going to do.

RUN WITH THE SERVICES STOPPED. This rewrites the whole file; poll or the TCP
server writing underneath it would either lose this edit or have theirs lost.
The advisory lock is still taken, as belt and braces.
"""

import json
import os
import sys
import fcntl
import tempfile

JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# Must match DEFAULT_VARIABLE_RANGES in tabletop_seeder_poll.py AND the widget
# limits in the GUIDE project. One unit of the six offsets is 100 ms on the
# ClearCore, so +-20 is +-2 seconds.
RANGES = {
    "roller_speed":        {"min": 0,   "max": 250},
    "belt_speed":          {"min": 0,   "max": 10},
    "irrigation_delay":    {"min": -20, "max": 20},
    "irrigation_duration": {"min": -20, "max": 20},
    "misting_delay":       {"min": -20, "max": 20},
    "misting_duration":    {"min": -20, "max": 20},
    "roller_delay":        {"min": -20, "max": 20},
    "roller_duration":     {"min": -20, "max": 20},
}


def atomic_write(path, data):
    """temp file -> fsync -> rename -> fsync the directory. Never open(path,'w')."""
    dir_name = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(data, f, indent=4)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        dfd = os.open(dir_name, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def main():
    with open(LOCK_FILE_PATH, "a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)

        with open(JSON_FILE_PATH) as f:
            data = json.load(f)

        before = data.get("variable_ranges", {})
        data["variable_ranges"] = RANGES

        changes = []
        for key in sorted((k for k in data if k.isdigit()), key=int):
            preset = data[key]
            if not isinstance(preset, dict):
                continue
            for field, bounds in RANGES.items():
                if field not in preset:
                    continue
                v = preset[field]
                if isinstance(v, bool) or not isinstance(v, (int, float)):
                    continue
                clamped = max(bounds["min"], min(bounds["max"], v))
                if clamped != v:
                    preset[field] = clamped
                    changes.append((key, field, v, clamped))

        atomic_write(JSON_FILE_PATH, data)

    print(f"variable_ranges updated in {JSON_FILE_PATH}")
    for field in sorted(RANGES):
        old = before.get(field)
        new = RANGES[field]
        marker = "" if old == new else "   <- changed"
        print(f"  {field:<20} {new['min']:>4} .. {new['max']:<4}{marker}")

    if changes:
        print(f"\n{len(changes)} stored value(s) clamped into the new range:")
        for key, field, was, now in changes:
            print(f"  variety {key:>2}  {field:<20} {was:>5} -> {now}")
        print("\nThese presets now say what the machine will actually do. Re-tune")
        print("them on the HMI if the clamped value is not what you want.")
    else:
        print("\nNo stored value needed clamping.")

    print("\nThe .bak still holds the pre-edit ranges; poll refreshes it on the")
    print("next preset save.")


if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError:
        sys.exit(f"ERROR: {JSON_FILE_PATH} not found — is this the right machine?")
    except json.JSONDecodeError as e:
        sys.exit(f"ERROR: {JSON_FILE_PATH} is not valid JSON ({e}). Refusing to "
                 f"rewrite it; restore from the .bak first.")
