













#!/usr/bin/env python3
import os
import sys
import time
import json
import fcntl
import tempfile
from typing import Dict
from te.interface.common import ScreenID, Status, VariableID
from te.utils.discovery_tool import pprint_discover_tes

# ========================
# Configuration
# ========================
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"
POLL_INTERVAL_SEC = 1.0

# Recovery strategy:
#   "reconnect" (default) -> self-heal in-process by rediscovering the encoder
#   "restart"             -> exit(42); let systemd restart the process (fresh venv/python)
RECOVERY_MODE = os.getenv("TE_RECOVERY", "reconnect").lower().strip()
RECONNECT_BACKOFF_SEC = 1.0
DISCOVER_RETRY_SEC = 1.0

# NEW: tolerate immediate post-enumeration hiccups
WARMUP_SEC = 1.5
READ_RETRIES = 5
READ_RETRY_SLEEP = 0.3

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
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with FileLock(LOCK_FILE_PATH, shared=False):
            if not os.path.exists(path):
                with open(path, "w") as f:
                    json.dump({"roller_speed": 0, "mode": 0}, f, indent=4)

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
        te = discover_te_once()
        if te:
            print("poll: discovered Touch Encoder")
            # NEW: give device a brief warm-up before first reads
            time.sleep(WARMUP_SEC)
            return te
        time.sleep(DISCOVER_RETRY_SEC)

def safe_get_var(te, screen_id: int, var_id: int) -> int:
    """
    Read a var with small retries. If we get Status.ERROR,
    explicitly set the target screen and try again.
    """
    last_err = None
    for attempt in range(READ_RETRIES):
        val = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
        if val != Status.ERROR:
            return val.to_int()
        last_err = "Status.ERROR"
        # try to ensure correct screen is active
        try:
            te.guide.set_screen(ScreenID(screen_id))
        except Exception:
            pass
        time.sleep(READ_RETRY_SLEEP)
    raise RuntimeError(f"{last_err} reading screen {screen_id} var {var_id}")


def handle_disconnect_and_recover():
    """Apply chosen recovery strategy on disconnect."""
    if RECOVERY_MODE == "restart":
        # give udev a moment to settle, then exit non-zero so systemd restarts us
        time.sleep(0.5)
        sys.exit(42)
    else:
        # default is "reconnect": drop handle by returning None to main loop
        time.sleep(RECONNECT_BACKOFF_SEC)
        return None

def main():
    ensure_json_exists(JSON_FILE_PATH)
    te = None

    # initial discovery
    te = discover_te_blocking()
    # try:
    #     te.guide.set_screen(ScreenID(3))
    # except Exception:
    #     pass
    try:
        te.guide.set_screen(ScreenID(3))
    except Exception:
        pass
    while True:
        try:
            mode = safe_get_var(te, 5, 1)        # Screen 5, var 1
            roller = safe_get_var(te, 3, 1)      # Screen 3, var 1

            current = locked_read_json(JSON_FILE_PATH) or {"roller_speed": 0, "mode": 0}
            current["mode"] = int(mode)
            current["roller_speed"] = int(roller)
            locked_atomic_write_json(JSON_FILE_PATH, current)

            time.sleep(POLL_INTERVAL_SEC)

        except KeyboardInterrupt:
            print("poll: received SIGINT, exiting...")
            sys.exit(0)

        except Exception as e:
            # Any HID/IO failure ends up here (USB brownout, device reset, etc.)
            print(f"poll: device error detected: {e}")
            # Choose recovery strategy
            te = handle_disconnect_and_recover()
            if te is None and RECOVERY_MODE != "restart":
                # Reconnect path: block until device is back
                te = discover_te_blocking()
                try:
                    te.guide.set_screen(ScreenID(3))
                except Exception:
                    pass

if __name__ == "__main__":
    main()






















# #!/usr/bin/env python3
# import os
# import sys
# import time
# import json
# import fcntl
# import tempfile
# from typing import Dict
# from te.interface.common import ScreenID, Status, VariableID
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
#             time.sleep(WARMUP_SEC)  # brief warm-up before first reads/writes
#             return te
#         time.sleep(DISCOVER_RETRY_SEC)

# def safe_get_var(te, screen_id: int, var_id: int) -> int:
#     """
#     Read a var with small retries. If we get Status.ERROR,
#     explicitly set the target screen and try again.
#     """
#     last_err = None
#     for _ in range(READ_RETRIES):
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

# def safe_set_var(te, screen_id: int, var_id: int, value: int) -> bool:
#     """Best-effort write with small retries."""
#     for _ in range(READ_RETRIES):
#         try:
#             te.guide.set_screen(ScreenID(screen_id))
#             te.guide.set_variable(screen_id, var_id, int(value))
#             # optional confirm
#             rv = te.guide.get_var(ScreenID(screen_id), VariableID(var_id))
#             if rv != Status.ERROR and rv.to_int() == int(value):
#                 print(f"restore: screen{screen_id}/var{var_id} -> {value}")
#             return True
#         except Exception:
#             time.sleep(READ_RETRY_SLEEP)
#     return False

# # --- One-time roller restore from JSON (screen 3, var 1) ---
# def restore_roller_from_json_once(te):
#     """
#     After discovery, read 'roller_speed' from JSON and write it
#     to screen 3, var 1 once. Leave mode (screen 5) alone.
#     """
#     try:
#         data = locked_read_json(JSON_FILE_PATH)
#         roller = int(data.get("roller_speed", 0))
#     except Exception:
#         return  # nothing to restore or JSON unreadable

#     if roller is None:
#         return
#     safe_set_var(te, 3, 1, roller)
# # -----------------------------------------------------------

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

#     # initial discovery
#     te = discover_te_blocking()
#     try:
#         te.guide.set_screen(ScreenID(3))  # keep UI steady on screen 3 initially
#     except Exception:
#         pass

#     # Restore ONLY roller_speed from JSON; do not set mode
#     restore_roller_from_json_once(te)

#     while True:
#         try:
#             # Normal operation: read BOTH screens every cycle
#             mode = safe_get_var(te, 5, 1)        # Screen 5 (read only)
#             roller = safe_get_var(te, 3, 1)      # Screen 3

#             current = locked_read_json(JSON_FILE_PATH) or {"roller_speed": 0, "mode": 0}
#             current["mode"] = int(mode)
#             current["roller_speed"] = int(roller)
#             locked_atomic_write_json(JSON_FILE_PATH, current)

#             time.sleep(POLL_INTERVAL_SEC)

#         except KeyboardInterrupt:
#             print("poll: received SIGINT, exiting...")
#             sys.exit(0)

#         except Exception as e:
#             print(f"poll: device error detected: {e}")
#             te = handle_disconnect_and_recover()
#             if te is None and RECOVERY_MODE != "restart":
#                 te = discover_te_blocking()
#                 try:
#                     te.guide.set_screen(ScreenID(3))
#                 except Exception:
#                     pass

# if __name__ == "__main__":
#     main()
