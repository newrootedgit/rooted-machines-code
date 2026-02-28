#!/usr/bin/env python3
"""
AWS IoT Connection Module

Handles MQTT connection to AWS IoT Core using pre-provisioned certificates.
Certificates are created using the provision-iot-device.sh script.
"""

import json
import os
import subprocess
from datetime import datetime, timezone
from awsiot import mqtt_connection_builder
from awscrt.mqtt import QoS

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVICE_FILE = os.path.join(PROJECT_DIR, 'device_config.json')
CERTS_DIR = os.path.join(PROJECT_DIR, 'certs')


def get_device_config():
    """Load device configuration from config file."""
    with open(DEVICE_FILE, 'r') as f:
        return json.load(f)


def verify_certificates():
    """Verify all required certificate files exist."""
    required_files = [
        ('certificate.pem.crt', 'Device certificate'),
        ('private.pem.key', 'Private key'),
        ('AmazonRootCA1.pem', 'Amazon Root CA'),
    ]

    missing = []
    for filename, description in required_files:
        path = os.path.join(CERTS_DIR, filename)
        if not os.path.exists(path):
            missing.append(f"  - {description}: {path}")

    if missing:
        raise FileNotFoundError(
            "Missing certificate files. Run provision-iot-device.sh first.\n"
            + "\n".join(missing)
        )

    return True


def connect_to_aws_iot():
    """Establish MQTT connection to AWS IoT Core.

    Returns:
        mqtt.Connection: The active MQTT connection
    """
    # Verify certs exist
    verify_certificates()

    # Load config
    config = get_device_config()
    device_id = config['device_id']
    endpoint = config.get('aws_iot_endpoint')

    if not endpoint:
        raise ValueError(
            "aws_iot_endpoint not found in device_config.json. "
            "Run provision-iot-device.sh to set up the device."
        )

    cert_path = os.path.join(CERTS_DIR, 'certificate.pem.crt')
    key_path = os.path.join(CERTS_DIR, 'private.pem.key')
    ca_path = os.path.join(CERTS_DIR, 'AmazonRootCA1.pem')

    print(f"Connecting to AWS IoT as {device_id}...")
    print(f"  Endpoint: {endpoint}")

    # Build MQTT connection
    mqtt_connection = mqtt_connection_builder.mtls_from_path(
        endpoint=endpoint,
        cert_filepath=cert_path,
        pri_key_filepath=key_path,
        ca_filepath=ca_path,
        client_id=device_id,
        clean_session=False,
        keep_alive_secs=60,
    )

    # Connect
    connect_future = mqtt_connection.connect()
    connect_future.result()
    print("Connected to AWS IoT!")

    return mqtt_connection


def disconnect_from_aws_iot(mqtt_connection):
    """Gracefully disconnect from AWS IoT."""
    if mqtt_connection:
        print("Disconnecting from AWS IoT...")
        disconnect_future = mqtt_connection.disconnect()
        disconnect_future.result()
        print("Disconnected.")


if __name__ == "__main__":
    import time

    print("=== AWS IoT Connection Test ===")

    mqtt_conn = None
    try:
        mqtt_conn = connect_to_aws_iot()
        print("\nConnection successful! Press Ctrl+C to disconnect.")

        # Keep connection alive
        while True:
            time.sleep(1)

    except FileNotFoundError as e:
        print(f"\nSetup required: {e}")
    except KeyboardInterrupt:
        print("\nShutting down...")
    except Exception as e:
        print(f"\nConnection error: {e}")
    finally:
        if mqtt_conn:
            disconnect_from_aws_iot(mqtt_conn)
