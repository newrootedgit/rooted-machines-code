#!/usr/bin/env python3
import os
import json
import socket
import fcntl
import time
from typing import Dict

# ========================
# Configuration
# ========================
HOST = "192.168.10.1"   # As confirmed
PORT = 8888
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# Tick rate at which we re-read the shared JSON and check whether the rendered
# payload has changed. This is the upper bound on how stale ready_to_run /
# variety params can be on the motor side when something *does* change.
POLL_INTERVAL_S = 0.5

# Send a refresh even if the payload hasn't changed, at least this often. Gives
# the ClearCore (or a fresh reconnect) a recent snapshot without relying on
# the next user action, and lets us layer on a deadman timeout later if we
# want fail-safe behavior on the motor side.
HEARTBEAT_INTERVAL_S = 10.0

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

def load_state() -> Dict:
    """
    Read the shared JSON written by the poll script and normalize it into
    a flat dict of numeric values suitable for CSV output.

    Expected JSON schema:
      {
        "ready_to_run": bool,
        "active_variety": int or null,
        "1": {
          "roller_speed": int,
          "belt_speed": int,
          "irrigation_delay": int,
          "irrigation_duration": int,
          "misting_delay": int,
          "misting_duration": int,
          "roller_delay": int,
          "roller_duration": int
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
    variety_names = data.get("variety_names", {})

    # Default values if nothing is set yet
    variety_values = {
        "roller_speed": 0,
        "belt_speed": 0,
        "irrigation_delay": 0,
        "irrigation_duration": 0,
        "misting_delay": 0,
        "misting_duration": 0,
        "roller_delay": 0,
        "roller_duration": 0,
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
        # Look up the human-readable name from variety_names map.
        if isinstance(variety_names, dict):
            variety_name = str(variety_names.get(key, ""))
    else:
        active_variety = -1  # sentinel for "no active variety"

    # Sanitize for the downstream CSV/UDP pipeline:
    # - strip commas/newlines so they can't fragment the ClearCore parser
    #   (which uses strtok(",")) or the telemetry UDP packet
    # - cap at 32 chars to match the ClearCore-side buffer
    variety_name = (
        variety_name.replace(",", "_").replace("\n", "_").replace("\r", "_")[:32]
    )

    return {
        "ready_to_run": ready_to_run,
        "active_variety": int(active_variety),
        "roller_speed": variety_values["roller_speed"],
        "belt_speed": variety_values["belt_speed"],
        "irrigation_delay": variety_values["irrigation_delay"],
        "irrigation_duration": variety_values["irrigation_duration"],
        "misting_delay": variety_values["misting_delay"],
        "misting_duration": variety_values["misting_duration"],
        "roller_delay": variety_values["roller_delay"],
        "roller_duration": variety_values["roller_duration"],
        "variety_name": variety_name,
    }

def build_payload() -> str:
    """Render the current shared state as a single CSV line (no trailing \\n)."""
    state = load_state()
    # Field order is the contract with the ClearCore parser. variety_name is
    # last so any future overflow truncation chops the name, not the structured
    # numeric tail.
    payload_fields = [
        state["ready_to_run"],
        state["active_variety"],
        state["roller_speed"],
        state["belt_speed"],
        state["irrigation_delay"],
        state["irrigation_duration"],
        state["misting_delay"],
        state["misting_duration"],
        state["roller_delay"],
        state["roller_duration"],
        state["variety_name"],
    ]
    return ",".join(str(x) for x in payload_fields)


def serve_client(conn: socket.socket, addr) -> None:
    """
    Hold the connection open and push a newline-terminated CSV snapshot
    whenever the rendered payload changes, plus a heartbeat refresh every
    HEARTBEAT_INTERVAL_S. Returns on disconnect.
    """
    # Detect a dead peer reasonably quickly without blocking on send.
    conn.settimeout(5.0)
    # Disable Nagle so sub-100-byte updates ship immediately.
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass

    print(f"tcp: [persistent-v2] client connected {addr}")
    last_sent: str | None = None
    last_sent_at = 0.0
    try:
        while True:
            try:
                payload = build_payload()
            except Exception as e:
                # Transient JSON read race — log and retry on the next tick.
                print(f"tcp: payload error for {addr}: {e}")
                time.sleep(POLL_INTERVAL_S)
                continue

            now = time.monotonic()
            changed   = (payload != last_sent)
            heartbeat = (now - last_sent_at) >= HEARTBEAT_INTERVAL_S
            if changed or heartbeat:
                try:
                    conn.sendall((payload + "\n").encode("utf-8"))
                except (BrokenPipeError, ConnectionResetError, socket.timeout, OSError) as e:
                    print(f"tcp: client {addr} disconnected: {e}")
                    return
                if changed:
                    print(f"tcp: {addr} <- '{payload}'")
                last_sent = payload
                last_sent_at = now
            time.sleep(POLL_INTERVAL_S)
    finally:
        try:
            conn.close()
        except Exception:
            pass


def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # Allow quick restart after crash
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"tcp: [persistent-v2] serving on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            # One client at a time matches the ClearCore deployment (single
            # motor controller). If a second client connects, finish serving
            # the current one first.
            serve_client(conn, addr)

if __name__ == "__main__":
    serve()