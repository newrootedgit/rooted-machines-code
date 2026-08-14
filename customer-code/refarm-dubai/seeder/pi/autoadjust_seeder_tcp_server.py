#!/usr/bin/env python3
import os
import json
import time
import socket
import fcntl
import threading
from typing import Dict

# ========================
# Configuration
# ========================
HOST = "192.168.10.1"
PORT = 8888
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# How often the shared JSON is re-read to see whether anything changed.
POLL_INTERVAL_S = 0.5

# Resend an unchanged snapshot this often, so a ClearCore that reconnected or
# missed an update still converges without waiting for the next operator action.
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
          "roller_start_delay": int (-20..+20),
          "roller_stop_delay": int (-20..+20)
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

def build_payload() -> str:
    """
    Render the current shared state as one CSV line (no trailing newline).

    Field order IS the contract with tabletop_seeder_photoeye.ino, which parses
    by position — see the switch on fieldIndex in parseReceivedMessage(). The
    sketch needs no changes to run this machine, and that only holds while these
    eleven fields stay in this order.

    Fields 8 and 9 land on the sketch's user_roller_start_mod_value /
    user_roller_end_mod_value, which are OFFSETS — hence the +-20 range (one
    unit = 100 ms) and hence roller_start_delay / roller_stop_delay mapping
    cleanly onto what the sketch calls roller_delay and roller_duration.

    Fields 4-7 are fixed timings with no encoder screen; see
    ensure_fixed_timings() in the poll script.

    Format: ready_to_run,active_variety,roller_speed,belt_speed,
            irrigation_delay,irrigation_duration,
            misting_delay,misting_duration,
            roller_start_delay,roller_stop_delay,variety_name
    """
    state = load_state()
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
    return ",".join(str(x) for x in payload_fields)


def enable_keepalive(conn: socket.socket) -> None:
    """
    Turn on TCP keepalive so a silently-dead peer — a ClearCore power cycle that
    never sends a FIN or RST — gets torn down by the kernel rather than
    lingering for the default ~15 minute retransmission timeout. The Linux
    tuning probes after 3s idle, every 2s, dropping after 3 failures (~9s).
    """
    try:
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        if hasattr(socket, "TCP_KEEPIDLE"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 3)
        if hasattr(socket, "TCP_KEEPINTVL"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 2)
        if hasattr(socket, "TCP_KEEPCNT"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
    except OSError:
        pass


def serve_client(conn: socket.socket, addr) -> None:
    """
    Hold the connection open and push a NEWLINE-TERMINATED CSV snapshot whenever
    the payload changes, plus a heartbeat every HEARTBEAT_INTERVAL_S.

    Both halves of that matter, and this server previously did neither.

    It used to accept, send one payload with NO trailing newline, and close. The
    sketch frames on '\\n' before it will call parseReceivedMessage(), so a
    payload without one is buffered and never parsed — the machine received
    nothing it could act on, ever, while the Pi log showed apparently healthy
    sends. And because the socket closed after each send, the ClearCore printed
    "Server disconnected. Attempting reconnect..." every two seconds forever.
    """
    conn.settimeout(5.0)
    # Disable Nagle so these sub-100-byte updates ship immediately rather than
    # waiting to coalesce with a next packet that may be seconds away.
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass

    print(f"tcp: [persistent] client connected {addr}")
    last_sent = None
    last_sent_at = 0.0
    try:
        while True:
            try:
                payload = build_payload()
            except Exception as e:
                # Transient JSON read race — log it and retry next tick rather
                # than dropping a working connection.
                print(f"tcp: payload error for {addr}: {e}")
                time.sleep(POLL_INTERVAL_S)
                continue

            now = time.monotonic()
            changed = (payload != last_sent)
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
        print(f"tcp: [persistent] serving on {HOST}:{PORT}")

        current_conn = None

        while True:
            conn, addr = s.accept()
            enable_keepalive(conn)

            # Single-client deployment: a new connection almost always means the
            # ClearCore rebooted and reconnected while we still held its stale
            # socket. Drop the previous one so the fresh connection wins
            # immediately instead of waiting for a timeout. The old
            # serve_client thread then errors on its next send and exits.
            if current_conn is not None:
                print(f"tcp: new client {addr}, dropping previous connection")
                try:
                    current_conn.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    current_conn.close()
                except OSError:
                    pass

            current_conn = conn
            threading.Thread(
                target=serve_client, args=(conn, addr), daemon=True
            ).start()


if __name__ == "__main__":
    serve()
