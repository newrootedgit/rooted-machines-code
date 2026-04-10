# #!/usr/bin/env python3
# import os
# import sys
# import time
# import json
# import fcntl
# import tempfile
# from typing import Dict

# from te.interface.common import ScreenID, Status, VariableID, VariableData
# from te.utils.discovery_tool import pprint_discover_tes

# # ========================
# # Configuration
# # ========================
# JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
# LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"
# POLL_INTERVAL_SEC = 1.0

# # Recovery strategy:
# #   "reconnect" (default) -> self-heal in-process by rediscovering the encoder
# #   "restart"             -> exit(42); let systemd restart the process (fresh venv/python)
# RECOVERY_MODE = os.getenv("TE_RECOVERY", "reconnect").lower().strip()
# RECONNECT_BACKOFF_SEC = 1.0
# DISCOVER_RETRY_SEC = 1.0

# # Tolerate immediate post-enumeration hiccups
# WARMUP_SEC = 1.5
# READ_RETRIES = 5
# READ_RETRY_SLEEP = 0.3

# # ========================
# # Lock helpers (advisory)
# # ========================
# class FileLock:
#     def __init__(self, lock_path: str, shared: bool):
#         self.lock_path = lock_path
#         self.shared = shared
#         self._fh = None

#     def __enter__(self):
#         self._fh = open(self.lock_path, "a+")
#         fcntl.flock(self._fh, fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX)
#         return self

#     def __exit__(self, exc_type, exc, tb):
#         try:
#             fcntl.flock(self._fh, fcntl.LOCK_UN)
#         finally:
#             self._fh.close()
#             self._fh = None

# def ensure_json_exists(path: str):
#     if not os.path.exists(path):
#         os.makedirs(os.path.dirname(path), exist_ok=True)
#         with FileLock(LOCK_FILE_PATH, shared=False):
#             if not os.path.exists(path):
#                 with open(path, "w") as f:
#                     json.dump({"roller_speed": 0, "mode": 0}, f, indent=4)

# def locked_read_json(path: str) -> Dict:
#     with FileLock(LOCK_FILE_PATH, shared=True):
#         try:
#             with open(path, "r") as f:
#                 return json.load(f)
#         except FileNotFoundError:
#             return {}
#         except json.JSONDecodeError:
#             return {}

# def locked_atomic_write_json(path: str, data: Dict):
#     with FileLock(LOCK_FILE_PATH, shared=False):
#         dir_name = os.path.dirname(path)
#         os.makedirs(dir_name, exist_ok=True)
#         fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
#         try:
#             with os.fdopen(fd, "w") as tmpf:
#                 json.dump(data, tmpf, indent=4)
#                 tmpf.flush()
#                 os.fsync(tmpf.fileno())
#             os.replace(tmp_path, path)
#         finally:
#             if os.path.exists(tmp_path):
#                 try:
#                     os.remove(tmp_path)
#                 except OSError:
#                     pass

# # ========================
# # Grayhill TE helpers
# # ========================
# def discover_te_once():
#     devices, hid_manager = pprint_discover_tes()
#     if not devices:
#         return None
#     return devices[0]

# def discover_te_blocking():
#     """Keep trying until a TE is found."""
#     while True:
#         te = discover_te_once()
#         if te:
#             print("poll: discovered Touch Encoder")
#             time.sleep(WARMUP_SEC)
#             return te
#         time.sleep(DISCOVER_RETRY_SEC)

# def safe_get_var(te, screen_id: int, var_id: int) -> int:
#     """
#     Read a var with small retries. If we get Status.ERROR,
#     explicitly set the target screen and try again.
#     """
#     last_err = None
#     for attempt in range(READ_RETRIES):
#         val = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
#         if val != Status.ERROR:
#             return val.to_int()
#         last_err = "Status.ERROR"
#         try:
#             te.guide.set_screen(ScreenID(screen_id))
#         except Exception:
#             pass
#         time.sleep(READ_RETRY_SLEEP)
#     raise RuntimeError(f"{last_err} reading screen {screen_id} var {var_id}")

# def set_variable(te, screen_id: int, var_id: int, value: int) -> bool:
#     """Set a variable and confirm by reading it back."""
#     try:
#         status = te.guide.set_var(ScreenID(screen_id), VariableID(var_id), VariableData(int(value)))
#         time.sleep(0.2)
#         if status != Status.OK:
#             print(f"set_variable: non-OK status {status} for s{screen_id} v{var_id}")
#             return False
#         got = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
#         ok = (got != Status.ERROR) and (got.to_int() == int(value))
#         if not ok:
#             print(f"set_variable: verify mismatch for s{screen_id} v{var_id}: got {got.to_int() if got!=Status.ERROR else 'ERROR'}")
#         return ok
#     except Exception as e:
#         print(f"Error setting variable s{screen_id} v{var_id}: {e}")
#         return False

# def restore_vars_if_reset(te):
#     """
#     If the device came back at defaults (e.g., after power-cycle),
#     push last known values from JSON.
#     """
#     last = locked_read_json(JSON_FILE_PATH) or {}
#     last_mode = int(last.get("mode", 0))
#     last_roll = int(last.get("roller_speed", 0))

#     try:
#         cur_mode = safe_get_var(te, 5, 1)
#     except Exception:
#         cur_mode = 0
#     try:
#         cur_roll = safe_get_var(te, 3, 1)
#     except Exception:
#         cur_roll = 0

#     should_restore = ((cur_mode == 0 and last_mode != 0) or
#                       (cur_roll == 0 and last_roll != 0))

#     if should_restore:
#         print(f"restore_vars_if_reset: restoring mode={last_mode}, roller={last_roll}")
#         _ok1 = set_variable(te, 5, 1, last_mode)   # mode
#         _ok2 = set_variable(te, 3, 1, last_roll)   # roller
#         print(f"restore_vars_if_reset: results mode={_ok1}, roller={_ok2}")

# def handle_disconnect_and_recover():
#     """Apply chosen recovery strategy on disconnect."""
#     if RECOVERY_MODE == "restart":
#         time.sleep(0.5)
#         sys.exit(42)
#     else:
#         time.sleep(RECONNECT_BACKOFF_SEC)
#         return None

# # ========================
# # Main
# # ========================
# def main():
#     ensure_json_exists(JSON_FILE_PATH)

#     # Initial discovery
#     te = discover_te_blocking()
#     first_cycle_after_reconnect = True  # treat first loop like a reconnect
#     try:
#         te.guide.set_screen(ScreenID(3))
#     except Exception:
#         pass
#     restore_vars_if_reset(te)

#     while True:
#         try:
#             # Read current device values
#             mode = safe_get_var(te, 5, 1)        # Screen 5, var 1
#             roller = safe_get_var(te, 3, 1)      # Screen 3, var 1

#             # Load last known for guard logic
#             last = locked_read_json(JSON_FILE_PATH) or {"roller_speed": 0, "mode": 0}
#             last_mode = int(last.get("mode", 0))
#             last_roller = int(last.get("roller_speed", 0))

#             # Guard: don't cement obvious reset defaults on the first cycle after reconnect
#             if first_cycle_after_reconnect and (
#                 (mode == 0 and last_mode != 0) or
#                 (roller == 0 and last_roller != 0)
#             ):
#                 print("poll: detected likely post-reconnect defaults; skipping JSON write this cycle")
#                 first_cycle_after_reconnect = False  # only skip once
#                 time.sleep(POLL_INTERVAL_SEC)
#                 continue

#             # Update and write JSON atomically
#             current = {
#                 "mode": int(mode),
#                 "roller_speed": int(roller),
#             }
#             locked_atomic_write_json(JSON_FILE_PATH, current)

#             # After first successful write, clear the flag
#             if first_cycle_after_reconnect:
#                 first_cycle_after_reconnect = False

#             time.sleep(POLL_INTERVAL_SEC)

#         except KeyboardInterrupt:
#             print("poll: received SIGINT, exiting...")
#             sys.exit(0)

#         except Exception as e:
#             # Any HID/IO failure ends up here (USB brownout, device reset, etc.)
#             print(f"poll: device error detected: {e}")

#             # Choose recovery strategy
#             te = handle_disconnect_and_recover()
#             if te is None and RECOVERY_MODE != "restart":
#                 # Reconnect path: block until device is back
#                 te = discover_te_blocking()
#                 try:
#                     te.guide.set_screen(ScreenID(10))
#                 except Exception:
#                     pass
#                 # Try to restore and treat next loop as "first after reconnect"
#                 restore_vars_if_reset(te)
#                 first_cycle_after_reconnect = True

# if __name__ == "__main__":
#     main()





















#THIS IS A TEST COMMENT TO SEE IF UPDATING WORKS
#             _
#            /^\
#           /   \
#          /  |  \
#         /___|___\
#           / | \
#          /  |  \
#         /   |   \
#        /____|____\   .----.
#        | []   [] |  / .--. \
#        |   ___   | ( (    ) )
#        |  (___)  |  \ '--' /
#        '---------'   '----'   -- Blast off! 🚀
#





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
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"
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
            return val.to_int()
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
                last_err = f"unexpected screen: {scr}"
        except Exception as e:
            last_err = str(e)
        time.sleep(READ_RETRY_SLEEP)
    raise RuntimeError(f"Error reading current screen: {last_err}")

def set_variable(screen_id: int, var_id: int, value: int) -> bool:
    """Set a variable and confirm by reading it back."""
    global te
    try:
        status = te.guide.set_var(ScreenID(screen_id), VariableID(var_id), VariableData(int(value)))
        time.sleep(0.2)
        if status != Status.OK:
            print(f"set_variable: non-OK status {status} for s{screen_id} v{var_id}")
            return False
        got = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
        ok = (got != Status.ERROR) and (got.to_int() == int(value))
        if not ok:
            print(f"set_variable: verify mismatch for s{screen_id} v{var_id}: "
                  f"got {got.to_int() if got != Status.ERROR else 'ERROR'}")
        return ok
    except Exception as e:
        print(f"Error setting variable s{screen_id} v{var_id}: {e}")
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
    roller_speed: int,
    belt_speed: int,
    irrigation_delay: int,
    irrigation_duration: int,
    misting_delay: int,
    misting_duration: int,
    roller_delay: int,
    roller_duration: int,
):
    data = locked_read_json(JSON_FILE_PATH) or {}
    key = str(int(variety_index))
    data[key] = {
        "roller_speed": int(roller_speed),
        "belt_speed": int(belt_speed),
        "irrigation_delay": int(irrigation_delay),
        "irrigation_duration": int(irrigation_duration),
        "misting_delay": int(misting_delay),
        "misting_duration": int(misting_duration),
        "roller_delay": int(roller_delay),
        "roller_duration": int(roller_duration),
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
    set_variable(6, 1, v.get("roller_speed", 0))        # Roller Speed
    set_variable(3, 1, v.get("belt_speed", 0))          # Belt Speed
    set_variable(11, 1, v.get("irrigation_delay", 0))   # Irrigation Delay
    set_variable(12, 1, v.get("irrigation_duration", 0))# Irrigation Duration
    set_variable(13, 1, v.get("misting_delay", 0))      # Misting Delay
    set_variable(14, 1, v.get("misting_duration", 0))   # Misting Duration
    set_variable(15, 1, v.get("roller_delay", 0))       # Roller Delay
    set_variable(16, 1, v.get("roller_duration", 0))    # Roller Duration

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

    # Return to variety selection screen
    try:
        global te
        te.guide.set_screen(ScreenID(10))
    except Exception as e:
        print(f"Error setting initial screen 10: {e}")

    while True:
        # Get current screen (with retries)
        active_screen = safe_get_screen()

        # -----------------------------
        # Screen 9: Save Confirmation
        # -----------------------------
        if active_screen == ScreenID(9):
            variety_index = get_variable(10, 1)     # current variety selection
            roller_speed = get_variable(6, 1)       # roller speed
            belt_speed = get_variable(3, 1)         # belt speed
            irrigation_delay = get_variable(11, 1)  # irrigation delay
            irrigation_duration = get_variable(12, 1)
            misting_delay = get_variable(13, 1)
            misting_duration = get_variable(14, 1)
            roller_delay = get_variable(15, 1)
            roller_duration = get_variable(16, 1)

            if None not in (
                variety_index,
                roller_speed,
                belt_speed,
                irrigation_delay,
                irrigation_duration,
                misting_delay,
                misting_duration,
                roller_delay,
                roller_duration,
            ):
                # Persist the variety data into JSON with locking
                save_variety_data(
                    variety_index,
                    roller_speed,
                    belt_speed,
                    irrigation_delay,
                    irrigation_duration,
                    misting_delay,
                    misting_duration,
                    roller_delay,
                    roller_duration,
                )
                time.sleep(2)
                # Return to variety selection screen
                te.guide.set_screen(ScreenID(10))

        # ---------------------------------------------
        # Screen 17: Variety Selection Confirmation
        # ---------------------------------------------
        elif active_screen == ScreenID(17):
            variety_index = get_variable(10, 1)
            save_active_variety(variety_index)  # Save active variety to JSON

            saved_data = load_variety_data()
            # Display which variety is loaded on screen 18
            set_variable(18, 2, variety_index)

            key = str(variety_index)
            if key in saved_data and isinstance(saved_data[key], dict):
                saved_values = saved_data[key]

                # Push saved values into the encoder
                set_variable(6, 1, saved_values.get("roller_speed", 0))        # Roller Speed
                set_variable(3, 1, saved_values.get("belt_speed", 0))          # Belt Speed
                set_variable(11, 1, saved_values.get("irrigation_delay", 0))   # Irrigation Delay
                set_variable(12, 1, saved_values.get("irrigation_duration", 0))# Irrigation Duration
                set_variable(13, 1, saved_values.get("misting_delay", 0))      # Misting Delay
                set_variable(14, 1, saved_values.get("misting_duration", 0))   # Misting Duration
                set_variable(15, 1, saved_values.get("roller_delay", 0))       # Roller Delay
                set_variable(16, 1, saved_values.get("roller_duration", 0))    # Roller Duration

                # Now it's safe to run
                ready_to_run_toggle(True)
            else:
                print(f"Variety {variety_index} not found. Waiting for user to define it.")

            time.sleep(2)
            # Variety selected confirmation screen
            te.guide.set_screen(ScreenID(18))

        time.sleep(POLL_INTERVAL_SEC)

# ========================
# Main
# ========================
def main():
    global te

    ensure_json_exists(JSON_FILE_PATH)

    # Initial discovery
    te = discover_te_blocking()
    # On startup or reconnect, restore whatever was last active
    restore_vars_if_reset()

    while True:
        try:
            monitor_touch_encoder_loop()

        except KeyboardInterrupt:
            print("poll: received SIGINT, exiting...")
            sys.exit(0)

        except Exception as e:
            print(f"poll: device error detected: {e}")
            # Apply recovery strategy
            te = handle_disconnect_and_recover()
            if te is None and RECOVERY_MODE != "restart":
                te = discover_te_blocking()
                try:
                    te.guide.set_screen(ScreenID(10))
                except Exception:
                    pass
                # After reconnect, try to restore the last active variety
                restore_vars_if_reset()


if __name__ == "__main__":
    main()

