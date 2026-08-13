#!/usr/bin/env python3
import os
import json
import socket
import fcntl
from typing import Dict

# ========================
# Configuration
# ========================
HOST = "192.168.10.1"
PORT = 8888
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# ========================
# Lock helpers (advisory)
# ========================
class FileLock:
    def __init__(self, lock_path: str, shared: bool):
        self.lock_path = lock_path
        self.shared = shared
        self._fh = None

    def __enter__(self):
        self._fh = open(self.lock_path, "a+")
        fcntl.flock(self._fh, fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX)
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            fcntl.flock(self._fh, fcntl.LOCK_UN)
        finally:
            self._fh.close()
            self._fh = None

# Applied when the settings file has no fixed_timings block. Poll seeds one at
# startup (ensure_fixed_timings), so this is only a fallback for the window
# before it does.
FALLBACK_FIXED_TIMINGS = {
    "irrigation_delay": 0,
    "irrigation_duration": 0,
    "misting_delay": 0,
    "misting_duration": 0,
}

def load_state() -> Dict:
    """
    Read the shared JSON written by the poll script and normalize it into
    a flat dict of values suitable for CSV output.

    Expected JSON schema:
      {
        "ready_to_run": bool,
        "active_variety": int or null,
        "fixed_timings": {          # NOT operator-adjustable; no encoder screen
          "irrigation_delay": int,
          "irrigation_duration": int,
          "misting_delay": int,
          "misting_duration": int
        },
        "variety_names": { "1": "...", ... },
        "1": {
          "roller_speed": int (0-250),
          "belt_speed": int (0-20),
          "roller_start_delay": int (-100..+100),
          "roller_stop_delay": int (-100..+100)
        },
        "2": { ... },
        ...
      }
    """
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(JSON_FILE_PATH, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            data = {}

    ready_to_run = int(bool(data.get("ready_to_run", False)))
    active_variety = data.get("active_variety", None)

    # Default values if nothing is set yet
    variety_values = {
        "roller_speed": 0,
        "belt_speed": 0,
        "roller_start_delay": 0,
        "roller_stop_delay": 0,
    }

    variety_name = ""

    if active_variety is not None:
        key = str(active_variety)
        v = data.get(key, {})
        if isinstance(v, dict):
            for k in variety_values.keys():
                try:
                    variety_values[k] = int(v.get(k, 0))
                except (TypeError, ValueError):
                    variety_values[k] = 0
        names = data.get("variety_names", {})
        if isinstance(names, dict):
            variety_name = str(names.get(key, key))
    else:
        active_variety = -1  # sentinel for "no active variety"

    # A comma or newline in the name would shift every field after it, or split
    # the packet. The sketch scrubs these too, but the side that builds the
    # payload is the right place to guarantee it.
    for ch in (",", "\n", "\r"):
        variety_name = variety_name.replace(ch, "_")
    variety_name = variety_name[:32]

    fixed = dict(FALLBACK_FIXED_TIMINGS)
    stored_fixed = data.get("fixed_timings")
    if isinstance(stored_fixed, dict):
        for k in fixed:
            try:
                fixed[k] = int(stored_fixed.get(k, fixed[k]))
            except (TypeError, ValueError):
                pass

    return {
        "ready_to_run": ready_to_run,
        "active_variety": int(active_variety),
        "roller_speed": variety_values["roller_speed"],
        "belt_speed": variety_values["belt_speed"],
        "irrigation_delay": fixed["irrigation_delay"],
        "irrigation_duration": fixed["irrigation_duration"],
        "misting_delay": fixed["misting_delay"],
        "misting_duration": fixed["misting_duration"],
        "roller_start_delay": variety_values["roller_start_delay"],
        "roller_stop_delay": variety_values["roller_stop_delay"],
        "variety_name": variety_name,
    }

def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # Allow quick restart after crash
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"tcp: serving on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    state = load_state()

                    # CSV payload in a fixed order. This IS the contract with
                    # tabletop_seeder_photoeye.ino, which parses by position —
                    # see the switch on fieldIndex in parseReceivedMessage().
                    # The sketch needs no changes to run this machine, and that
                    # only holds while these eleven fields stay in this order.
                    #
                    # Fields 8 and 9 land on the sketch's
                    # user_roller_start_mod_value / user_roller_end_mod_value,
                    # which are OFFSETS — hence the -100..+100 range and hence
                    # roller_start_delay / roller_stop_delay mapping cleanly
                    # onto what the sketch already called roller_delay and
                    # roller_duration.
                    #
                    # Fields 4-7 are fixed timings with no encoder screen; see
                    # ensure_fixed_timings() in the poll script.
                    #
                    # Format: ready_to_run,active_variety,roller_speed,belt_speed,
                    #         irrigation_delay,irrigation_duration,
                    #         misting_delay,misting_duration,
                    #         roller_start_delay,roller_stop_delay,variety_name
                    payload_fields = [
                        state["ready_to_run"],
                        state["active_variety"],
                        state["roller_speed"],
                        state["belt_speed"],
                        state["irrigation_delay"],
                        state["irrigation_duration"],
                        state["misting_delay"],
                        state["misting_duration"],
                        state["roller_start_delay"],
                        state["roller_stop_delay"],
                        state["variety_name"],
                    ]
                    payload = ",".join(str(x) for x in payload_fields)

                    conn.sendall(payload.encode("utf-8"))

                    print(f"tcp: {addr} -> '{payload}'")

                except Exception as e:
                    try:
                        conn.sendall(b"ERR")
                    except Exception:
                        pass
                    print(f"tcp: error serving {addr}: {e}")

if __name__ == "__main__":
    serve()
