#!/usr/bin/env python3
"""
Mock ClearCore — dev tool to simulate ClearCore telemetry without hardware.
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Connects to harvester_logger.py as if it were the real ClearCore firmware.

Downlink (received from Pi):  prints parsed screen_5_var_1 / screen_3_var_1.
Uplink   (sent to Pi):        fires  LOG|<UptimeS>|<DeltaSteps>  every 1 second.

Usage:
    python mock_clearcore.py                         # localhost:8888
    python mock_clearcore.py --host 192.168.10.1    # real Pi on network
    python mock_clearcore.py --belt-speed 300        # faster simulated belt
    python mock_clearcore.py --no-reconnect          # exit instead of retrying
"""

import socket
import time
import random
import argparse
import threading


# ── CLI ───────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description="Mock ClearCore (dev only)")
    p.add_argument("--host",        default="localhost",  help="Logger host")
    p.add_argument("--port",        type=int, default=8888)
    p.add_argument("--belt-speed",  type=int, default=200,
                   help="Simulated base belt pulses/sec")
    p.add_argument("--no-reconnect", action="store_true",
                   help="Exit on disconnect instead of retrying")
    return p.parse_args()


# ── Downlink receiver (background thread) ─────────────────────────────────────

def recv_loop(conn: socket.socket, stop_event: threading.Event):
    """
    Continuously read downlink commands from the Pi and print them.
    Sets stop_event when the connection drops so the main loop can react.
    """
    buf = ""
    while not stop_event.is_set():
        try:
            chunk = conn.recv(256).decode("ascii", errors="replace")
            if not chunk:
                print("[mock]   Server closed connection.")
                stop_event.set()
                break
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                # Parse the downlink format:
                # "screen_5_var_1: <mode>; screen_3_var_1: <belt_speed>;"
                mode       = _extract_int(line, "screen_5_var_1:")
                belt_speed = _extract_int(line, "screen_3_var_1:")
                print(f"[down]   ← mode={mode}  belt_speed={belt_speed}  (raw: {line!r})")
        except socket.timeout:
            continue
        except OSError:
            stop_event.set()
            break


def _extract_int(line: str, key: str) -> int:
    """Pull the integer value after 'key' in a semicolon-delimited string."""
    idx = line.find(key)
    if idx == -1:
        return -1
    after = line[idx + len(key):].strip().rstrip(";").strip()
    # Take the first token in case there are more fields
    token = after.split(";")[0].split()[0] if after else ""
    try:
        return int(token)
    except ValueError:
        return -1


# ── Main uplink loop ──────────────────────────────────────────────────────────

def run_session(conn: socket.socket, base_speed: int) -> None:
    """Send LOG heartbeats once per second until the connection drops."""
    conn.settimeout(2.0)

    stop_event = threading.Event()
    rx_thread  = threading.Thread(target=recv_loop, args=(conn, stop_event), daemon=True)
    rx_thread.start()

    boot_time = time.monotonic()

    try:
        while not stop_event.is_set():
            time.sleep(1.0)
            if stop_event.is_set():
                break

            uptime_s    = int(time.monotonic() - boot_time)
            delta_steps = base_speed + random.randint(-20, 20)

            msg = f"LOG|{uptime_s}|{delta_steps}\n"
            try:
                conn.sendall(msg.encode())
                print(f"[up]     → LOG|{uptime_s}|{delta_steps}")
            except OSError as e:
                print(f"[mock]   Send failed: {e}")
                stop_event.set()
                break
    except KeyboardInterrupt:
        stop_event.set()
        raise


def main():
    args = parse_args()

    print(f"[mock]   Belt base speed : {args.belt_speed} pulses/sec")
    print(f"[mock]   Target server   : {args.host}:{args.port}")
    print(f"[mock]   Press Ctrl-C to stop.\n")

    while True:
        try:
            print(f"[mock]   Connecting to {args.host}:{args.port}...")
            conn = socket.create_connection((args.host, args.port), timeout=5.0)
            print(f"[mock]   Connected.\n")
            run_session(conn, args.belt_speed)
        except KeyboardInterrupt:
            print("\n[mock]   Stopped by user.")
            break
        except (ConnectionRefusedError, socket.timeout) as e:
            print(f"[mock]   Connection failed: {e}")
        except Exception as e:
            print(f"[mock]   Session error: {e}")
        finally:
            try:
                conn.close()
            except Exception:
                pass

        if args.no_reconnect:
            print("[mock]   --no-reconnect set, exiting.")
            break

        print("[mock]   Reconnecting in 2s...\n")
        time.sleep(2.0)


if __name__ == "__main__":
    main()
