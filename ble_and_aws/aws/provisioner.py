#!/opt/rooted-ble/.venv/bin/python3 -u
import subprocess
import os
import sys
import threading
from bluezero import peripheral
from bluezero import adapter
import json
from json import JSONDecodeError
import dbus

SERVICE_UUID = "322486ee-3b18-476d-86ae-2481eafaea9a"
SSID_UUID    = "322486ee-3b18-476d-86ae-2481eafaea9b"
PASS_UUID    = "322486ee-3b18-476d-86ae-2481eafaea9c"
STATUS_UUID  = "322486ee-3b18-476d-86ae-2481eafaea9d"

ONBOARD_UUID     = "322486ee-3b18-476d-86ae-2481eafaea9e"
USER_INFO_UUID   = "322486ee-3b18-476d-86ae-2481eafaea9f"
DEVICE_ID_UUID   = "322486ee-3b18-476d-86ae-2481eafeaea0"

ONBOARD_CODE = "RootedRobotics123"

ONBOARD_STATUS = 0x04

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STATE_FILE = os.path.join(SCRIPT_DIR, 'onboard_state.json')
DEVICE_FILE = os.path.join(SCRIPT_DIR, 'device_config.json')


def get_device_config():
    """Load device config, auto-generating device_id if missing."""
    config = {}

    if os.path.exists(DEVICE_FILE):
        try:
            with open(DEVICE_FILE, 'r') as f:
                config = json.load(f)
        except (JSONDecodeError, IOError) as e:
            print(f"Error loading device config: {e}")

    return config


class MachineBLE:
    def __init__(self):
        self.ssid = ""
        self.password = ""
        self.status_chr = None  # Set when client subscribes to notifications
        self.is_onboarded = False
        self.user_info = None
        

    def _safe_notify(self, value):
        """Safely send notification, handling client disconnection gracefully."""
        if not self.status_chr:
            return False
        try:
            self.status_chr.set_value(value)
            return True
        except dbus.exceptions.DBusException as e:
            # Client disconnected - this is expected when phone moves away or app closes
            print(f"Client disconnected (notification failed): {e.get_dbus_name()}")
            self.status_chr = None
            return False
        except Exception as e:
            print(f"Unexpected error sending notification: {e}")
            self.status_chr = None
            return False

    def on_status_notify(self, notifying, characteristic):
        """Called when client subscribes/unsubscribes to status notifications"""
        if notifying:
            self.status_chr = characteristic
            print("Client subscribed to status notifications")
        else:
            print("Client unsubscribed from status notifications")

    def on_ssid_write(self, value, options):
        self.ssid = bytes(value).decode('utf-8')
        print(f"SSID received: {self.ssid}")

    def on_onboard_write(self, value, options):
        code = bytes(value).decode('utf-8')
        if code == ONBOARD_CODE:
            self.is_onboarded = True
            # Send back onboarding success status (0x04)
            self._safe_notify([ONBOARD_STATUS])
            print("Device successfully onboarded.")
        else:
            print("Invalid onboarding code received.")

    def on_user_info_write(self, value, options):
        try:
            user_info = bytes(value).decode('utf-8')
            self.user_info = json.loads(user_info)
            with open(STATE_FILE, 'w') as f:
                json.dump(self.user_info, f)
            print(f"User info received: {self.user_info}")
        except JSONDecodeError:
            print("Invalid JSON received for user info.")
        except dbus.exceptions.DBusException as e:
            print(f"Client disconnected during user_info write: {e.get_dbus_name()}")
        except Exception as e:
            print(f"Error in user_info write: {e}")

    def on_pass_write(self, value, options):
        if not self.is_onboarded:
            print("Device not onboarded. Ignoring password write.")
            return
        self.password = bytes(value).decode('utf-8')
        print("Password received. Attempting Wi-Fi connection...")
        # Run WiFi connection in background thread so BLE write can complete
        thread = threading.Thread(target=self.connect_to_wifi)
        thread.start()

    def connect_to_wifi(self):
        if not self.is_onboarded:
            print("Device not onboarded. Cannot connect to Wi-Fi.")
            return

        if not self.ssid:
            print("SSID not set.")
            return

        self._safe_notify([0x01])  # Connecting

        cmd = ["sudo", "nmcli", "device", "wifi", "connect", self.ssid, "password", self.password]
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            if result.returncode == 0:
                print("Success!")
                self._safe_notify([0x02])  # Success
            else:
                print(f"Failed: {result.stderr}")
                self._safe_notify([0x03])  # Failure
        except Exception as e:
            print(f"Error: {e}")
            self._safe_notify([0x03])


def start_ble():
    machine = MachineBLE()
    config = get_device_config()

    if not config.get('device_name') or not config.get('device_id'):
        print("Device configuration incomplete. Please ensure device_name and device_id are set.")
        return

    device_name = config['device_name']
    device_id = config['device_id']

    adapters = list(adapter.Adapter.available())
    if not adapters:
        print("No Bluetooth adapters found!")
        return

    adapter_address = adapters[0].address
    print(f"Using adapter: {adapter_address}")
    print(f"Device ID: {device_id}")

    app = peripheral.Peripheral(adapter_address, local_name=device_name)
    app.add_service(srv_id=1, uuid=SERVICE_UUID, primary=True)

    app.add_characteristic(srv_id=1, chr_id=1, uuid=SSID_UUID,
                           value=[], notifying=False, flags=['write'],
                           write_callback=machine.on_ssid_write)

    app.add_characteristic(srv_id=1, chr_id=2, uuid=PASS_UUID,
                           value=[], notifying=False, flags=['write'],
                           write_callback=machine.on_pass_write)

    app.add_characteristic(srv_id=1, chr_id=4, uuid=ONBOARD_UUID,
                           value=[], notifying=False, flags=['write'],
                           write_callback=machine.on_onboard_write)

    app.add_characteristic(srv_id=1, chr_id=5, uuid=USER_INFO_UUID,
                           value=[], notifying=False, flags=['write'],
                           write_callback=machine.on_user_info_write)

    # Device ID characteristic - readable by frontend
    app.add_characteristic(srv_id=1, chr_id=6, uuid=DEVICE_ID_UUID,
                           value=list(device_id.encode('utf-8')),
                           notifying=False, flags=['read'])

    app.add_characteristic(srv_id=1, chr_id=3, uuid=STATUS_UUID,
                           value=[0x00], notifying=False,
                           flags=['notify'],
                           notify_callback=machine.on_status_notify)

    print(f"GATT Server running. Advertising as: {device_name}")

    try:
        app.publish()
    except dbus.exceptions.DBusException as e:
        # Handle client disconnection gracefully - this is normal behavior
        error_name = e.get_dbus_name()
        if "ServiceUnknown" in error_name or "NoReply" in error_name:
            print(f"BLE client disconnected: {error_name}")
        else:
            print(f"DBus error: {e}")
            raise
    except KeyboardInterrupt:
        print("Shutting down BLE server...")


if __name__ == '__main__':
    print("=== Rooted BLE Provisioner Starting ===")
    print(f"Python version: {sys.version}")
    start_ble()