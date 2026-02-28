#!/usr/bin/env python3
"""
MQTT Command Handler

Subscribes to the commands topic and handles get_presets / update_presets actions.
Publishes responses back on the pong topic.
Runs telemetry uplink in a background thread sharing the same MQTT connection.
"""

import json
import time
from awscrt.mqtt import QoS
from aws_iot_registration import connect_to_aws_iot, disconnect_from_aws_iot, get_device_config
from machine_preset import load_presets, save_presets
from telemetry import start_telemetry_thread


def start_command_handler():
    config    = get_device_config()
    device_id = config['device_id']

    commands_topic  = f'rooted/machines/{device_id}/commands'
    pong_topic      = f'rooted/machines/{device_id}/pong'
    telemetry_topic = f'rooted/machines/{device_id}/telemetry'

    mqtt_connection = connect_to_aws_iot()

    def on_message(topic, payload, **kwargs):
        try:
            message    = json.loads(payload)
            action     = message.get('action')
            request_id = message.get('requestId')

            print(f"Received command: action={action}, requestId={request_id}")

            if not request_id:
                print("Missing requestId, ignoring")
                return

            if action == 'get_presets':
                try:
                    presets  = load_presets()
                    response = {
                        'requestId': request_id,
                        'action':    'presets_response',
                        'config':    presets,
                    }
                except Exception as e:
                    response = {
                        'requestId': request_id,
                        'action':    'presets_response',
                        'error':     str(e),
                    }

            elif action == 'update_presets':
                try:
                    current = load_presets()

                    new_presets = message.get('presets')
                    print(f"Updating presets with: {new_presets}")
                    if new_presets:
                        for key, values in new_presets.items():
                            if key in current and isinstance(current[key], dict):
                                current[key].update(values)
                            else:
                                current[key] = values

                    new_variety_names = message.get('variety_names')
                    print(f"Updating variety names with: {new_variety_names}")
                    if new_variety_names:
                        if 'variety_names' not in current:
                            current['variety_names'] = {}
                        current['variety_names'].update(new_variety_names)

                    save_presets(current)
                    response = {
                        'requestId': request_id,
                        'action':    'presets_updated',
                        'success':   True,
                    }
                except Exception as e:
                    response = {
                        'requestId': request_id,
                        'action':    'presets_updated',
                        'success':   False,
                        'error':     str(e),
                    }

            else:
                print(f"Unknown action: {action}")
                return

            mqtt_connection.publish(
                topic=pong_topic,
                payload=json.dumps(response),
                qos=QoS.AT_LEAST_ONCE,
            )
            print(f"Published response to {pong_topic}: {response['action']}")

        except Exception as e:
            print(f"Error handling command: {e}")

    subscribe_future, _ = mqtt_connection.subscribe(
        topic=commands_topic,
        qos=QoS.AT_LEAST_ONCE,
        callback=on_message,
    )
    subscribe_future.result()
    print(f"Subscribed to {commands_topic}")

    stop_event = start_telemetry_thread(mqtt_connection, telemetry_topic)

    return mqtt_connection, stop_event


if __name__ == '__main__':
    print("=== Rooted Command Handler ===")
    mqtt_conn  = None
    stop_event = None
    try:
        mqtt_conn, stop_event = start_command_handler()
        print("Listening for commands... Press Ctrl+C to stop.")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        if stop_event:
            stop_event.set()
        if mqtt_conn:
            disconnect_from_aws_iot(mqtt_conn)
