#!/usr/bin/env python3
"""
Telemetry Uplink

Periodically drains unsynced records from the local JSONL telemetry log
and publishes them to AWS IoT Core over MQTT.

Topic:   rooted/machines/<device_id>/telemetry
Payload: each JSONL record (heartbeat or other event type)
"""

import json
import os
import tempfile
import time

from awscrt.mqtt import QoS
from aws_iot_registration import connect_to_aws_iot, disconnect_from_aws_iot, get_device_config

LOG_JSONL_PATH  = "/home/rooted/telemetry_log.jsonl"
CURSOR_PATH     = "/home/rooted/telemetry_cursor.json"

POLL_INTERVAL_S = 300  # seconds between drain attempts (5 minutes)
BATCH_SIZE      = 50   # max records published per cycle


def read_cursor() -> int:
    """Return the number of lines already synced (0 if cursor is missing or corrupt)."""
    try:
        with open(CURSOR_PATH, "r") as f:
            data = json.load(f)
        return int(data["synced_lines"])
    except Exception:
        return 0


def write_cursor(n: int) -> None:
    """Atomically update the cursor file to n synced lines."""
    dir_ = os.path.dirname(CURSOR_PATH)
    with tempfile.NamedTemporaryFile("w", dir=dir_, delete=False, suffix=".tmp") as tmp:
        json.dump({"synced_lines": n}, tmp)
        tmp_path = tmp.name
    os.replace(tmp_path, CURSOR_PATH)


def fetch_unsynced_jsonl(cursor: int) -> list[dict]:
    """
    Read lines [cursor:] from the JSONL file, up to BATCH_SIZE.

    Concurrency guards:
    - Discards the final line if it has no trailing newline (mid-write in progress).
    - Skips lines that are not valid JSON.
    """
    try:
        with open(LOG_JSONL_PATH, "r") as f:
            content = f.read()
    except FileNotFoundError:
        return []

    # Drop last line if mid-write (no trailing newline)
    if content and not content.endswith("\n"):
        content = content[:content.rfind("\n") + 1]

    lines = content.splitlines()
    batch = []
    for line in lines[cursor: cursor + BATCH_SIZE]:
        try:
            batch.append(json.loads(line))
        except json.JSONDecodeError:
            print(f"[telemetry] Skipping malformed line: {line!r}")
    return batch


def publish_batch(
    mqtt_connection,
    topic: str,
    records: list[dict],
) -> int:
    """
    Publish each record and return the count of successfully published records.
    Stops at the first failure so the cursor only advances past confirmed sends.
    """
    synced_count = 0

    for record in records:
        payload = json.dumps(record)
        try:
            pub_future, _ = mqtt_connection.publish(
                topic=topic,
                payload=payload,
                qos=QoS.AT_LEAST_ONCE,
            )
            pub_future.result()  # blocks until puback
            synced_count += 1
            print(
                f"[telemetry] ↑ type={record.get('type', '?')}  "
                f"uptime={record.get('uptime_s', '?'):>6}  "
                f"delta={record.get('delta_steps', '?'):>6}  [sent]"
            )
        except Exception as e:
            print(f"[telemetry] Publish failed: {e}")
            # Stop this batch; remaining records will be retried next cycle.
            break

    return synced_count


def run_uplink() -> None:
    config    = get_device_config()
    device_id = config["device_id"]
    topic     = f"rooted/machines/{device_id}/telemetry"

    print(f"[telemetry] Device: {device_id}")
    print(f"[telemetry] Topic:  {topic}")
    print(f"[telemetry] Log:    {LOG_JSONL_PATH}")

    mqtt_connection = connect_to_aws_iot()

    print(f"[telemetry] Draining every {POLL_INTERVAL_S}s, batch={BATCH_SIZE} records.")

    try:
        while True:
            cursor  = read_cursor()
            records = fetch_unsynced_jsonl(cursor)

            if records:
                synced_count = publish_batch(mqtt_connection, topic, records)
                if synced_count:
                    write_cursor(cursor + synced_count)
                    print(f"[telemetry] Cursor advanced to {cursor + synced_count}.")
            else:
                print("[telemetry] No unsynced records.")

            time.sleep(POLL_INTERVAL_S)

    except KeyboardInterrupt:
        print("\n[telemetry] Shutting down...")
    finally:
        disconnect_from_aws_iot(mqtt_connection)


if __name__ == "__main__":
    run_uplink()
