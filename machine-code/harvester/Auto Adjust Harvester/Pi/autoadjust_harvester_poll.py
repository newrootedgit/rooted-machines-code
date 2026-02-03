#!/usr/bin/env python3
import os
import sys
import time
import json
import fcntl
import tempfile
from typing import Dict

from te.interface.common import ScreenID, Status, VariableID, VariableData
from te.utils.discovery_tool import pprint_discover_tes

# ========================
# Configuration
# ========================
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
JSON_PIN_FILE_PATH = "/home/rooted/te-cli/PIN_Values.json"

LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"
LOCK_PIN_FILE_PATH = JSON_PIN_FILE_PATH + ".lock"

POLL_INTERVAL_SEC = 0.3  # matches original monitor loop cadence

# Recovery strategy:
#   "reconnect" (default) -> self-heal in-process by rediscovering the encoder
#   "restart"             -> exit(42); let systemd restart the process (fresh venv/python)
RECOVERY_MODE = os.getenv("TE_RECOVERY", "reconnect").lower().strip()
RECONNECT_BACKOFF_SEC = 1.0
DISCOVER_RETRY_SEC = 1.0

# Tolerate immediate post-enumeration hiccups
WARMUP_SEC = 1.5
READ_RETRIES = 5
READ_RETRY_SLEEP = 0.3

# Global TE handle used by get_variable / set_variable helpers
te = None

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

def ensure_json_exists(path: str):
    """
    Create a base JSON file if none exists.
    Schema:
      {
        "ready_to_run": false,
        "active_variety": null,
        "1": { ... variety 1 data ... },
        "2": { ... variety 2 data ... },
        ...
      }
    """
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with FileLock(LOCK_FILE_PATH, shared=False):
            if not os.path.exists(path):
                with open(path, "w") as f:
                    json.dump({
                        "ready_to_run": False,
                        "active_variety": None
                    }, f, indent=4)

def locked_read_json(path: str) -> Dict:
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except FileNotFoundError:
            return {}
        except json.JSONDecodeError:
            return {}

def locked_atomic_write_json(path: str, data: Dict):
    with FileLock(LOCK_FILE_PATH, shared=False):
        dir_name = os.path.dirname(path)
        os.makedirs(dir_name, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
        try:
            with os.fdopen(fd, "w") as tmpf:
                json.dump(data, tmpf, indent=4)
                tmpf.flush()
                os.fsync(tmpf.fileno())
            os.replace(tmp_path, path)
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass

# ========================
# Grayhill TE helpers
# ========================
def discover_te_once():
    devices, hid_manager = pprint_discover_tes()
    if not devices:
        return None
    return devices[0]

def discover_te_blocking():
    """Keep trying until a TE is found."""
    while True:
        te_dev = discover_te_once()
        if te_dev:
            print("poll: discovered Touch Encoder")
            time.sleep(WARMUP_SEC)
            return te_dev
        time.sleep(DISCOVER_RETRY_SEC)

def safe_get_var(screen_id: int, var_id: int) -> int:
    """
    Read a var with small retries. Uses global `te`.
    If we get Status.ERROR, explicitly set the target screen and retry.
    """
    global te
    last_err = None
    for attempt in range(READ_RETRIES):
        val = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
        if val != Status.ERROR:
            try:
                return val.to_int()
            except ValueError:
                # TE returned a non-integer value; treat as transient error and retry
                last_err = "ValueError in to_int()"
                print(f"DEBUG: safe_get_var ValueError on s{screen_id} v{var_id}, attempt {attempt + 1}")
                continue
        last_err = "Status.ERROR"
        try:
            te.guide.set_screen(ScreenID(screen_id))
        except Exception:
            pass
        time.sleep(READ_RETRY_SLEEP)
    raise RuntimeError(f"{last_err} reading screen {screen_id} var {var_id}")

def safe_get_screen() -> ScreenID:
    """Get current screen with small retries."""
    global te
    if te is None:
        raise RuntimeError("te is None in safe_get_screen")
    last_err = None
    for attempt in range(READ_RETRIES):
        try:
            scr = te.guide.get_screen()
            if isinstance(scr, ScreenID):
                return scr
            # Some APIs might return int; normalize
            try:
                return ScreenID(scr)
            except Exception:
                last_err = f"unexpected screen: {scr} (type: {type(scr)})"
        except Exception as e:
            last_err = str(e)
            if attempt == 0:  # Only print on first attempt to avoid spam
                print(f"DEBUG: safe_get_screen attempt {attempt + 1} failed: {e}")
        time.sleep(READ_RETRY_SLEEP)
    raise RuntimeError(f"Error reading current screen: {last_err}")

def set_variable(screen_id: int, var_id: int, value: int) -> bool:
    """Set a variable and confirm by reading it back."""
    global te
    if te is None:
        print(f"ERROR: te is None in set_variable for s{screen_id} v{var_id}")
        return False
    try:
        status = te.guide.set_var(ScreenID(screen_id), VariableID(var_id), VariableData(int(value)))
        time.sleep(0.2)
        # Some libraries use Status.SUCCESS instead of Status.OK
        expected_ok = getattr(Status, "SUCCESS", None) or getattr(Status, "OK", None)
        if expected_ok is not None and status != expected_ok:
            print(f"set_variable: non-success status {status} for s{screen_id} v{var_id}")
            return False
        got = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
        ok = False
        if got != Status.ERROR:
            try:
                ok = (got.to_int() == int(value))
            except ValueError:
                print(f"set_variable: ValueError converting got.to_int() for s{screen_id} v{var_id}")
        if not ok:
            print(f"set_variable: verify mismatch for s{screen_id} v{var_id}: "
                  f"got {got.to_int() if got != Status.ERROR else 'ERROR'}")
        return ok
    except Exception as e:
        print(f"ERROR: Exception setting variable s{screen_id} v{var_id}: {e}")
        import traceback
        traceback.print_exc()
        return False

# Original-style helpers that monitor_touch_encoder expects
def get_variable(screen_id: int, var_id: int) -> int:
    return safe_get_var(screen_id, var_id)

# ========================
# JSON-based state helpers
# ========================
def load_variety_data() -> Dict:
    """
    Return entire JSON object.
    Variety entries are stored under keys str(variety_index).
    """
    data = locked_read_json(JSON_FILE_PATH) or {}
    return data

def save_variety_data(
    variety_index: int,
    blade_speed: int,
    belt_speed: int,
    blade_height: int,
    airknife_mode: int,
):
    data = locked_read_json(JSON_FILE_PATH) or {}
    key = str(int(variety_index))
    data[key] = {
        "blade_speed": int(blade_speed),
        "belt_speed": int(belt_speed),
        "blade_height": int(blade_height),
        "airknife_mode": max(0, min(3, int(airknife_mode))),
    }
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"Saved variety {key} to JSON")

def save_active_variety(variety_index: int):
    data = locked_read_json(JSON_FILE_PATH) or {}
    data["active_variety"] = int(variety_index)
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"Set active_variety = {variety_index}")

def ready_to_run_toggle(flag: bool):
    """
    Track ready_to_run in JSON. The actual machine start logic lives on the
    ClearCore / main controller side that reads this JSON.
    """
    data = locked_read_json(JSON_FILE_PATH) or {}
    data["ready_to_run"] = bool(flag)
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"ready_to_run set to {bool(flag)}")

def restore_vars_if_reset():
    """
    After reconnect (or process restart), push the last active variety's values
    back to the encoder so the UI reflects what the JSON says.
    Restores: active_variety, belt_speed, blade_speed, and blade_height.
    """
    data = locked_read_json(JSON_FILE_PATH) or {}
    active_variety = data.get("active_variety", None)
    if active_variety is None:
        print("restore_vars_if_reset: no active_variety stored; nothing to restore")
        return

    key = str(active_variety)
    v = data.get(key)
    if not isinstance(v, dict):
        print(f"restore_vars_if_reset: no data stored for variety {key}")
        return

    print(f"restore_vars_if_reset: restoring values for variety {key}")
    
    # Restore active_variety (screen 10, var 1)
    set_variable(10, 1, active_variety)
    
    # Restore variety settings
    set_variable(6, 1, v.get("blade_speed", 0))        # Blade Speed
    set_variable(3, 1, v.get("belt_speed", 0))          # Belt Speed
    set_variable(16, 1, v.get("blade_height", 0))      # Blade Height
    set_variable(40, 1, v.get("airknife_mode", 0))     # Airknife Mode

# ========================
# PIN management helpers
# ========================
def ensure_pin_json_exists(path: str, default_pin: list = None):
    """
    Create a PIN JSON file if none exists.
    Schema:
      {
        "default_pin": [0, 0, 0, 0],  # never changes
        "user_pin": [0, 0, 0, 0]      # user can set
      }
    """
    if default_pin is None:
        default_pin = [0, 0, 0, 0]
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with FileLock(LOCK_PIN_FILE_PATH, shared=False):
            if not os.path.exists(path):
                # Validate default_pin format
                if not isinstance(default_pin, list) or len(default_pin) != 4:
                    # Fallback default PIN is 0000
                    default_pin = [0, 0, 0, 0]
                # Ensure all values are integers 0-9
                default_pin = [max(0, min(9, int(d))) for d in default_pin]
                with open(path, "w") as f:
                    json.dump({
                        "default_pin": default_pin,
                        "user_pin": [0, 0, 0, 0]
                    }, f, indent=4)

def locked_read_pin_json(path: str) -> Dict:
    """Read PIN JSON file with shared lock."""
    with FileLock(LOCK_PIN_FILE_PATH, shared=True):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except FileNotFoundError:
            return {}
        except json.JSONDecodeError:
            return {}

def locked_atomic_write_pin_json(path: str, data: Dict):
    """Write PIN JSON file atomically with exclusive lock."""
    with FileLock(LOCK_PIN_FILE_PATH, shared=False):
        dir_name = os.path.dirname(path)
        os.makedirs(dir_name, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
        try:
            with os.fdopen(fd, "w") as tmpf:
                json.dump(data, tmpf, indent=4)
                tmpf.flush()
                os.fsync(tmpf.fileno())
            os.replace(tmp_path, path)
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass

def verify_pin(entered_pin: list) -> bool:
    """
    Verify a user-entered PIN against both the default pin and user pin.
    Sets the touch encoder screen based on match result:
    - Screen 26 if PIN matches
    - Screen 37 if PIN does not match
    
    Args:
        entered_pin: List of 4 integers (0-9) representing the entered PIN
        
    Returns:
        True if the entered PIN matches either the default_pin or user_pin, False otherwise
    """
    global te
    
    # Validate input format
    if not isinstance(entered_pin, list) or len(entered_pin) != 4:
        # Invalid PIN format - set to error screen
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(37))
            except Exception as e:
                print(f"Error setting screen 37 (invalid PIN format): {e}")
        return False
    
    # Ensure all values are integers 0-9
    try:
        entered_pin = [max(0, min(9, int(d))) for d in entered_pin]
    except (ValueError, TypeError):
        # Invalid PIN format - set to error screen
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(37))
            except Exception as e:
                print(f"Error setting screen 37 (invalid PIN format): {e}")
        return False
    
    # Read PIN data with lock
    pin_data = locked_read_pin_json(JSON_PIN_FILE_PATH)
    
    # Get default and user pins, with validation
    default_pin = pin_data.get("default_pin", [0, 0, 0, 0])
    user_pin = pin_data.get("user_pin", [0, 0, 0, 0])
    
    # Ensure pins are lists of 4 integers
    if not isinstance(default_pin, list) or len(default_pin) != 4:
        default_pin = [0, 0, 0, 0]
    if not isinstance(user_pin, list) or len(user_pin) != 4:
        user_pin = [0, 0, 0, 0]
    
    # Normalize to integers 0-9
    default_pin = [max(0, min(9, int(d))) for d in default_pin]
    user_pin = [max(0, min(9, int(u))) for u in user_pin]
    
    # Compare entered PIN against both saved pins
    matches_default = entered_pin == default_pin
    matches_user = entered_pin == user_pin
    pin_matches = matches_default or matches_user
    
    # Set screen based on match result
    if te is not None:
        try:
            # Pause before advancing to result screen
            time.sleep(2)
            if pin_matches:
                te.guide.set_screen(ScreenID(26))
            else:
                te.guide.set_screen(ScreenID(37))
        except Exception as e:
            print(f"Error setting screen after PIN verification: {e}")

    return pin_matches

def confirm_new_pin(pin_1: list, pin_2: list) -> bool:
    """
    Compare two PINs provided by the user. If they match, update the user-settable PIN.
    Sets the touch encoder screen based on match result:
    - Screen 39 if PINs match and update was successful
    - Screen 38 if PINs don't match (user_pin remains unchanged)
    
    Args:
        pin_1: First PIN entry (list of 4 integers 0-9)
        pin_2: Second PIN entry (list of 4 integers 0-9) for confirmation
        
    Returns:
        True if both PINs match and the update was successful, False otherwise
    """
    global te
    
    # Validate input format
    if not isinstance(pin_1, list) or len(pin_1) != 4:
        # Invalid PIN format - set to error screen
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(38))
            except Exception as e:
                print(f"Error setting screen 38 (invalid PIN format): {e}")
        return False
    if not isinstance(pin_2, list) or len(pin_2) != 4:
        # Invalid PIN format - set to error screen
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(38))
            except Exception as e:
                print(f"Error setting screen 38 (invalid PIN format): {e}")
        return False
    
    # Ensure all values are integers 0-9
    try:
        pin_1 = [max(0, min(9, int(d))) for d in pin_1]
        pin_2 = [max(0, min(9, int(d))) for d in pin_2]
    except (ValueError, TypeError):
        # Invalid PIN format - set to error screen
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(38))
            except Exception as e:
                print(f"Error setting screen 38 (invalid PIN format): {e}")
        return False
    
    # Check if the two PINs match
    if pin_1 != pin_2:
        # PINs don't match - leave user_pin unchanged and go to screen 38
        if te is not None:
            try:
                te.guide.set_screen(ScreenID(38))
            except Exception as e:
                print(f"Error setting screen 38 (PINs don't match): {e}")
        return False
    
    # PINs match - update user_pin
    # Read current PIN data with lock
    pin_data = locked_read_pin_json(JSON_PIN_FILE_PATH)
    
    # Ensure default_pin exists and is preserved (never changes)
    default_pin = pin_data.get("default_pin", [0, 0, 0, 0])
    if not isinstance(default_pin, list) or len(default_pin) != 4:
        default_pin = [0, 0, 0, 0]
    default_pin = [max(0, min(9, int(d))) for d in default_pin]
    
    # Update user_pin with the confirmed new PIN
    pin_data["default_pin"] = default_pin
    pin_data["user_pin"] = pin_1
    
    # Write back with exclusive lock
    locked_atomic_write_pin_json(JSON_PIN_FILE_PATH, pin_data)
    print(f"User PIN updated successfully")
    
    # PINs matched and update successful - go to screen 39
    if te is not None:
        try:
            te.guide.set_screen(ScreenID(39))
        except Exception as e:
            print(f"Error setting screen 39 (PIN update successful): {e}")
    
    return True

def handle_disconnect_and_recover():
    """Apply chosen recovery strategy on disconnect."""
    if RECOVERY_MODE == "restart":
        time.sleep(0.5)
        sys.exit(42)
    else:
        time.sleep(RECONNECT_BACKOFF_SEC)
        return None

# ========================
# Core logic (adapted monitor_touch_encoder)
# ========================
def monitor_touch_encoder_loop():
    """
    This is your original monitor_touch_encoder logic,
    wrapped to run under the reconnect / JSON-locking architecture.
    """
    # When entering the loop, not safe to run
    ready_to_run_toggle(False)
    
    # Get initial screen
    initial_screen = safe_get_screen()
    initial_screen_int = initial_screen.value if hasattr(initial_screen, 'value') else int(initial_screen)
    print(f"DEBUG: Initial screen detected: {initial_screen} (int value: {initial_screen_int}, type: {type(initial_screen)})")

    # Return to variety selection screen
    try:
        global te
        target_screen = 10
        print(f"DEBUG: Attempting to set screen to {target_screen}")
        print(f"DEBUG: te object: {te}, te.guide: {te.guide if te else 'None'}")
        
        # Check what Status values are available (guarded so it can't crash)
        try:
            status_success = getattr(Status, "SUCCESS", None)
            status_error = getattr(Status, "ERROR", None)
            print(f"DEBUG: Status.SUCCESS = {status_success}, Status.ERROR = {status_error}")
        except Exception as status_e:
            print(f"DEBUG: Could not introspect Status enum: {status_e}")
        
        result = te.guide.set_screen(ScreenID(target_screen))
        print(f"DEBUG: set_screen returned: {result} (type: {type(result)}, value: {result.value if hasattr(result, 'value') else result})")
        
        # Wait a moment and verify the screen changed
        time.sleep(0.5)
        verify_screen = safe_get_screen()
        verify_screen_int = verify_screen.value if hasattr(verify_screen, 'value') else int(verify_screen)
        print(f"DEBUG: Screen after set_screen({target_screen}): {verify_screen} (int value: {verify_screen_int})")
        print(f"DEBUG: Device should show screen {verify_screen_int} on the physical display")
        print(f"DEBUG: NOTE: If device shows screen 1 but code reports screen 10, there may be a screen ID mapping issue")
        
        if verify_screen != ScreenID(target_screen):
            print(f"DEBUG: WARNING - Screen did not change to {target_screen}! Still on {verify_screen} (int: {verify_screen_int})")
        else:
            print(f"DEBUG: Successfully set screen to {target_screen}")
            
    except Exception as e:
        print(f"ERROR: Exception setting initial screen 10: {e}")
        import traceback
        traceback.print_exc()


    loop_count = 0
    while True:
        loop_count += 1
        # Get current screen (with retries)
        try:
            active_screen = safe_get_screen()
            active_screen_int = active_screen.value if hasattr(active_screen, 'value') else int(active_screen)
            if loop_count % 50 == 0:  # Print every 50 iterations to avoid spam
                print(f"DEBUG: Loop iteration {loop_count}, current screen: {active_screen} (int: {active_screen_int}, device shows: screen {active_screen_int})")
        except Exception as e:
            print(f"ERROR: Exception getting screen in loop: {e}")
            import traceback
            traceback.print_exc()
            time.sleep(POLL_INTERVAL_SEC)
            continue


        # ---------------------------------------------
        # Screen 17: Operator Mode - Variety Selection Confirmation
        # ---------------------------------------------
        if active_screen == ScreenID(17):
            variety_index = get_variable(10, 1)
            save_active_variety(variety_index)  # Save active variety to JSON

            saved_data = load_variety_data()
            # Display which variety is loaded on screen 18
            set_variable(18, 2, variety_index)

            key = str(variety_index)
            if key in saved_data and isinstance(saved_data[key], dict):
                saved_values = saved_data[key]

                # Push saved values into the encoder
                set_variable(6, 1, saved_values.get("blade_speed", 0))        # Blade Speed
                set_variable(3, 1, saved_values.get("belt_speed", 0))          # Belt Speed
                set_variable(16, 1, saved_values.get("blade_height", 0))    # Blade Height
                set_variable(40, 1, saved_values.get("airknife_mode", 0))    # Airknife Mode


                # Now it's safe to run
                ready_to_run_toggle(True)
            else:
                print(f"Variety {variety_index} not found. Waiting for user to define it.")

            time.sleep(2)
            # Variety selected confirmation screen
            te.guide.set_screen(ScreenID(18))


        # ---------------------------------------------
        # Screen 19: Edit Mode - Load Selected Variety Settings
        # ---------------------------------------------
        if active_screen == ScreenID(19):
            variety_index = get_variable(26, 1)
            save_active_variety(variety_index)  # Save active variety to JSON

            saved_data = load_variety_data()

            key = str(variety_index)
            if key in saved_data and isinstance(saved_data[key], dict):
                saved_values = saved_data[key]

                # Push saved values into the encoder
                set_variable(6, 1, saved_values.get("blade_speed", 0))        # Blade Speed
                set_variable(3, 1, saved_values.get("belt_speed", 0))          # Belt Speed
                set_variable(16, 1, saved_values.get("blade_height", 0))    # Blade Height
                set_variable(40, 1, saved_values.get("airknife_mode", 0))    # Airknife Mode


            else:
                print(f"Variety {variety_index} not found. Waiting for user to define it.")

            time.sleep(2)
            # Variety selected confirmation screen
            te.guide.set_screen(ScreenID(6))

        # -----------------------------
        # Screen 25: Check Pin to Enter Preset Editor
        # -----------------------------
        if active_screen == ScreenID(25):
            pin_1 = get_variable(21, 2)     # pin 1
            pin_2 = get_variable(22, 2)     # pin 2
            pin_3 = get_variable(23, 2)     # pin 3
            pin_4 = get_variable(24, 2)     # pin 4

            # Check if all pin values are valid before verifying
            if None not in (pin_1, pin_2, pin_3, pin_4):
                entered_pin = [pin_1, pin_2, pin_3, pin_4]
                verify_pin(entered_pin)
                # verify_pin will automatically set screen 26 (success) or 37 (failure)

        # -----------------------------
        # Screen 9: Save Preset
        # -----------------------------
        if active_screen == ScreenID(9):
            variety_index = get_variable(26, 1)     # current variety selection (Edit Mode screen)
            blade_speed = get_variable(6, 1)       # blade speed
            belt_speed = get_variable(3, 1)         # belt speed
            blade_height = get_variable(16, 1)     # blade height
            airknife_mode = get_variable(40, 1)    # airknife mode (0-3)

            if None not in (
                variety_index,
                blade_speed,
                belt_speed,
                blade_height,
                airknife_mode,
            ):
                # Persist the variety data into JSON with locking
                save_variety_data(
                    variety_index,
                    blade_speed,
                    belt_speed,
                    blade_height,
                    airknife_mode,
                )
                time.sleep(2)
                # Return to variety selection screen
                te.guide.set_screen(ScreenID(10))


        # -----------------------------
        # Screen 36: Set New PIN
        # -----------------------------
        elif active_screen == ScreenID(36):
            new_pin_1 = get_variable(28, 2)     # pin 1
            new_pin_2 = get_variable(29, 2)     # pin 2
            new_pin_3 = get_variable(30, 2)     # pin 3
            new_pin_4 = get_variable(31, 2)     # pin 4

            new_pin_1_confirm = get_variable(32, 2)     # pin 1 confirm
            new_pin_2_confirm = get_variable(33, 2)     # pin 2 confirm
            new_pin_3_confirm = get_variable(34, 2)     # pin 3 confirm
            new_pin_4_confirm = get_variable(35, 2)     # pin 4 confirm

            # Check if all pin values are valid before confirming
            if None not in (new_pin_1, new_pin_2, new_pin_3, new_pin_4,
                           new_pin_1_confirm, new_pin_2_confirm, new_pin_3_confirm, new_pin_4_confirm):
                new_pin = [new_pin_1, new_pin_2, new_pin_3, new_pin_4]
                new_pin_confirm = [new_pin_1_confirm, new_pin_2_confirm, new_pin_3_confirm, new_pin_4_confirm]
                confirm_new_pin(new_pin, new_pin_confirm)
                # confirm_new_pin will automatically set screen 39 (success) or 38 (failure)


        time.sleep(POLL_INTERVAL_SEC)

# ========================
# Main
# ========================
def main():
    global te

    print("DEBUG: Starting main()")
    ensure_json_exists(JSON_FILE_PATH)
    ensure_pin_json_exists(JSON_PIN_FILE_PATH)
    print("DEBUG: JSON files ensured")

    # Initial discovery
    print("DEBUG: Starting TE discovery...")
    te = discover_te_blocking()
    print(f"DEBUG: TE discovered: {te}, type: {type(te)}")
    if te:
        print(f"DEBUG: te.guide: {te.guide}, type: {type(te.guide)}")
    # On startup or reconnect, restore whatever was last active
    # restore_vars_if_reset()

    while True:
        try:
            print("DEBUG: Entering monitor_touch_encoder_loop()")
            monitor_touch_encoder_loop()

        except KeyboardInterrupt:
            print("poll: received SIGINT, exiting...")
            sys.exit(0)

        except Exception as e:
            print(f"ERROR: poll: device error detected: {e}")
            import traceback
            traceback.print_exc()
            # Apply recovery strategy
            te = handle_disconnect_and_recover()
            if te is None and RECOVERY_MODE != "restart":
                te = discover_te_blocking()
                try:
                    print("DEBUG: Attempting to set screen 10 after reconnect")
                    te.guide.set_screen(ScreenID(10))
                    time.sleep(0.5)
                    verify = safe_get_screen()
                    print(f"DEBUG: Screen after reconnect set_screen(10): {verify}")
                except Exception as reconnect_err:
                    print(f"ERROR: Exception setting screen after reconnect: {reconnect_err}")
                    import traceback
                    traceback.print_exc()
                # After reconnect, try to restore the last active variety
                restore_vars_if_reset()


if __name__ == "__main__":
    main()

