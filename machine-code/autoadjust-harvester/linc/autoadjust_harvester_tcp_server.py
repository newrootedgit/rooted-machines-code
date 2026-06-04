#!/usr/bin/env python3
import os
import json
import socket
import fcntl
import threading
import time
from typing import Dict

# ========================
# Configuration
# ========================
HOST = "192.168.10.1"
PORT = 8888
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# Match the tabletop seeder TCP behavior: keep the ClearCore connection open
# and push a fresh line when state changes, plus a periodic heartbeat.
POLL_INTERVAL_S = 0.5
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
          "blade_speed": int,
          "belt_speed": int,
          "blade_height": int
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
        "blade_speed": 0,
        "belt_speed": 0,
        "blade_height": 0,
    }

    if active_variety is not None:
        key = str(active_variety)
        v = data.get(key, {})
        if isinstance(v, dict):
            for k in variety_values.keys():
                try:
                    variety_values[k] = int(v.get(k, 0))
                except (TypeError, ValueError):
                    variety_values[k] = 0
    else:
        active_variety = -1  # sentinel for "no active variety"

    return {
        "ready_to_run": ready_to_run,
        "active_variety": int(active_variety),
        "blade_speed": variety_values["blade_speed"],
        "belt_speed": variety_values["belt_speed"],
        "blade_height": variety_values["blade_height"],
    }

def build_payload() -> str:
    state = load_state()
    # Field order is the contract with the ClearCore parser.
    # Format: ready_to_run,active_variety,blade_speed,belt_speed,blade_height
    payload_fields = [
        state["ready_to_run"],
        state["active_variety"],
        state["blade_speed"],
        state["belt_speed"],
        state["blade_height"],
    ]
    return ",".join(str(x) for x in payload_fields)

def enable_keepalive(conn: socket.socket) -> None:
    """
    Turn on TCP keepalive so a silently-dead peer (e.g. a ClearCore power cycle
    that never sends a FIN/RST) gets torn down by the kernel instead of
    lingering for the default ~15min retransmission timeout. Linux-specific
    tuning probes after 3s idle, every 2s, dropping after 3 fails (~9s).
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
    Hold the connection open and push newline-terminated CSV snapshots when
    values change, plus heartbeat refreshes.
    """
    conn.settimeout(5.0)
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass

    print(f"tcp: [persistent-v2] client connected {addr}")
    last_sent = None
    last_sent_at = 0.0

    try:
        while True:
            try:
                payload = build_payload()
            except Exception as e:
                print(f"tcp: payload error for {addr}: {e}")
                time.sleep(POLL_INTERVAL_S)
                continue

            now = time.monotonic()
            changed = payload != last_sent
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
        print(f"tcp: [persistent-v3] serving on {HOST}:{PORT}")

        current_conn = None

        while True:
            conn, addr = s.accept()
            enable_keepalive(conn)

            # Single-client deployment: a new connection almost always means the
            # ClearCore rebooted and reconnected while we were still holding its
            # stale socket. Drop the previous connection so the fresh one wins
            # immediately instead of waiting for the old one to time out. The
            # old serve_client thread then errors out on its next send and exits.
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
