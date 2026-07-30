#!/usr/bin/env python3
import os
import json
import socket
import fcntl
import tempfile
import threading
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

# CSV field 4 (belt_speed) is pinned to this constant instead of being sourced
# from the JSON. The belt speed screen was removed from the Touch Encoder — the
# conveyor runs off a standalone VFD dial and the sketch autocalibrates against
# it — but the field must stay in the payload because field order is the parser
# contract with the ClearCore.
#
# It has to be a CONSTANT, not the stored value: the sketch treats any *change*
# in this field as "the operator re-dialed the VFD" and drops back to
# calibrating, burning the next tray as a no-dispense measurement pass. The
# per-variety belt_speed values still on disk are stale and inconsistent
# (variety 1 = 25, variety 2 = 10, the rest 0), so relaying them would trigger a
# spurious recalibration on every variety change.
#
# The value 1 matches the sketch's own boot default for user_vfd_speed, so the
# field is identical from the very first packet and the change detector can
# never fire. It only feeds the pre-calibration fallback speed table, which is
# telemetry-only — the sketch actuates nothing until calibration completes.
BELT_SPEED_CSV_VALUE = 1

# Control-plane return channel from the ClearCore: CAL_STATE datagrams carrying
# the sketch's calibration state, which we mirror into the shared JSON so the
# poll script can display it on the Touch Encoder.
#
# NOT port 9999 — that belongs to the AWS telemetry ingest, and two processes
# cannot bind the same UDP port. This listener lives in the TCP server process
# because that process already runs continuously and already owns the
# ClearCore-facing side of the JSON; a separate service would need its own unit
# file for three lines of parsing.
CAL_STATE_UDP_HOST = "0.0.0.0"
CAL_STATE_UDP_PORT = 9997
CAL_STATE_SCHEMA_VER = 1

# If no CAL_STATE datagram arrives for this long, treat the calibration state as
# unknown rather than showing a stale "Done calibrating" from a controller that
# may have since rebooted or lost its link.
CAL_STATE_STALE_AFTER_S = 10.0

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

    belt_speed is the one exception: the key is still present in the JSON for
    the web app's benefit, but it is ignored here and the CSV carries the
    pinned BELT_SPEED_CSV_VALUE instead. See that constant for why.

    Expected JSON schema:
      {
        "ready_to_run": bool,
        "active_variety": int or null,
        "1": {
          "roller_speed": int,
          "belt_speed": int,          # present but ignored (see above)
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
    # Written by the poll script when the operator presses Calibrate. Held high
    # until poll sees cal_state leave "done"; the sketch edge-triggers on it.
    calibrate_request = int(bool(data.get("calibrate_request", False)))

    # Default values if nothing is set yet. belt_speed is deliberately absent —
    # it is never read from the JSON, only emitted as BELT_SPEED_CSV_VALUE.
    variety_values = {
        "roller_speed": 0,
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
        "belt_speed": BELT_SPEED_CSV_VALUE,
        "irrigation_delay": variety_values["irrigation_delay"],
        "irrigation_duration": variety_values["irrigation_duration"],
        "misting_delay": variety_values["misting_delay"],
        "misting_duration": variety_values["misting_duration"],
        "roller_delay": variety_values["roller_delay"],
        "roller_duration": variety_values["roller_duration"],
        "calibrate_request": calibrate_request,
        "variety_name": variety_name,
    }

def force_ready_to_run_false() -> None:
    """
    Root fail-safe: clear ready_to_run on disk before we serve any client.

    This server is the ONLY bridge to the ClearCore, so forcing the flag false
    here guarantees the first payload we can ever send is ready=0 — no matter the
    boot order relative to the poll script. Without this, a stale ready_to_run=true
    persisted from a previous session is streamed to the motor the instant the
    ClearCore connects, before the poll process clears it (the "brief startup
    twitch"). A control-bridge (re)start must always fail safe to stopped.
    """
    try:
        with FileLock(LOCK_FILE_PATH, shared=False):
            try:
                with open(JSON_FILE_PATH, "r") as f:
                    data = json.load(f)
            except (FileNotFoundError, json.JSONDecodeError):
                data = {}
            if data.get("ready_to_run", False):
                data["ready_to_run"] = False
                with open(JSON_FILE_PATH, "w") as f:
                    json.dump(data, f, indent=4)
                print("tcp: startup fail-safe - cleared stale ready_to_run")
    except OSError as e:
        # Never let a startup write hiccup keep the server from coming up.
        print(f"tcp: could not clear ready_to_run at startup: {e}")


def build_payload() -> str:
    """Render the current shared state as a single CSV line (no trailing \\n)."""
    state = load_state()
    # Field order is the contract with the ClearCore parser. variety_name is
    # last so any future overflow truncation chops the name, not the structured
    # numeric tail — which is why calibrate_request slots in ahead of it rather
    # than being appended.
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
        state["calibrate_request"],
        state["variety_name"],
    ]
    return ",".join(str(x) for x in payload_fields)


def _locked_merge_json(updates: Dict) -> None:
    """
    Read-modify-write the shared JSON under an exclusive lock.

    Only the given keys are touched; everything else on disk is preserved. The
    poll script owns most of this file, so we must never write a wholesale
    snapshot from here or we'd clobber a variety save racing us.
    """
    with FileLock(LOCK_FILE_PATH, shared=False):
        try:
            with open(JSON_FILE_PATH, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            data = {}
        if all(data.get(k) == v for k, v in updates.items()):
            return  # no change; skip the write entirely
        data.update(updates)
        dir_name = os.path.dirname(JSON_FILE_PATH)
        os.makedirs(dir_name, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
        try:
            with os.fdopen(fd, "w") as tmpf:
                json.dump(data, tmpf, indent=4)
                tmpf.flush()
                os.fsync(tmpf.fileno())
            os.replace(tmp_path, JSON_FILE_PATH)
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass


def parse_cal_state(raw: str) -> int | None:
    """
    Pull cal_state out of a CAL_STATE datagram.

    Format: CAL_STATE,schema_ver,boot_id,seq,cal_state,belt_speed_x100
    Returns None for anything malformed or off-schema — a bad datagram must
    never be able to move the displayed state.
    """
    parts = raw.strip().split(",")
    if len(parts) < 5 or parts[0].strip() != "CAL_STATE":
        return None
    try:
        if int(parts[1]) != CAL_STATE_SCHEMA_VER:
            return None
        cal_state = int(parts[4])
    except ValueError:
        return None
    return cal_state if cal_state in (0, 1, 2) else None


def cal_state_listener() -> None:
    """
    Receive CAL_STATE datagrams from the ClearCore and mirror them into the
    shared JSON, so poll can render calibration progress on the Touch Encoder.

    Runs as a daemon thread inside the TCP server process. Never raises — a
    failure here must not take down the control channel.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((CAL_STATE_UDP_HOST, CAL_STATE_UDP_PORT))
    except OSError as e:
        print(f"tcp: cal_state listener could not bind "
              f"{CAL_STATE_UDP_HOST}:{CAL_STATE_UDP_PORT}: {e}")
        return
    # Wake up regularly even with no traffic so the staleness check can run.
    sock.settimeout(1.0)
    print(f"tcp: cal_state listener on {CAL_STATE_UDP_HOST}:{CAL_STATE_UDP_PORT}")

    last_rx = 0.0
    last_written = None
    while True:
        try:
            raw, _addr = sock.recvfrom(256)
        except socket.timeout:
            # No datagram this second. If the controller has gone quiet, drop
            # the state to unknown so the TE stops claiming a stale result.
            if last_written is not None and (time.monotonic() - last_rx) > CAL_STATE_STALE_AFTER_S:
                try:
                    _locked_merge_json({"cal_state": None})
                    print("tcp: cal_state stale — controller silent, set to unknown")
                except OSError as e:
                    print(f"tcp: could not clear stale cal_state: {e}")
                last_written = None
            continue
        except OSError as e:
            print(f"tcp: cal_state listener recv error: {e}")
            time.sleep(0.5)
            continue

        cal_state = parse_cal_state(raw.decode("utf-8", errors="replace"))
        if cal_state is None:
            continue

        last_rx = time.monotonic()
        if cal_state != last_written:
            try:
                _locked_merge_json({"cal_state": cal_state})
                print(f"tcp: cal_state -> {cal_state}")
                last_written = cal_state
            except OSError as e:
                print(f"tcp: could not write cal_state: {e}")


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
    # Clear any stale ready_to_run BEFORE binding/accepting, so the first
    # snapshot a connecting ClearCore receives is always stopped.
    force_ready_to_run_false()

    # Same reasoning for the calibration handshake: a request left latched high
    # from a previous session would fire a recalibration on the ClearCore's
    # first heartbeat after reconnect. Start from a known-clear state, and mark
    # cal_state unknown until the controller actually reports in.
    try:
        _locked_merge_json({"calibrate_request": 0, "cal_state": None})
    except OSError as e:
        print(f"tcp: could not clear calibration state at startup: {e}")

    threading.Thread(target=cal_state_listener, daemon=True).start()

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