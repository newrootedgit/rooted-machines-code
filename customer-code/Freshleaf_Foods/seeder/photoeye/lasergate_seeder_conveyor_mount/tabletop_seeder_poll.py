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
NUM_VARIETIES = 20       # total variety slots (1-20)

# Variety name display on Touch Encoder
VARIETY_NAME_SCREEN = 10   # Operator variety selection screen
VARIETY_NAME_VAR = 7       # VariableID for the name string on screen 10

# Screen 18 is the run screen: it shows which variety is loaded AND carries the
# Pause/Activate toggle, so RUNNING_VARIETY_SCREEN and STATE_SCREEN are the same
# screen with different variables on it.
#   var 1 -> variety name        var 2 -> "Active"/"Paused"    var 5 -> press latch
RUNNING_VARIETY_SCREEN = 18
RUNNING_VARIETY_VAR    = 1  # variety name string

# Machine state (status display + pause/activate button), on screen 18.
STATE_SCREEN = RUNNING_VARIETY_SCREEN
STATE_STATUS_VAR    = 2  # "Active" / "Paused" — mirrors ready_to_run
# The start/stop button in GUIDE is configured with Action="Set value" -> writes
# 1 into this Value ID (default 0). Poll detects nonzero, toggles ready_to_run
# in the JSON, then writes 0 back so the latch is re-armed for the next press.
# Must be a NUMERIC value in GUIDE — safe_get_var calls .to_int() on the read.
STATE_BTN_PRESS_VAR = 5

# Status strings pushed to STATE_STATUS_VAR. These are display only; the machine
# state itself lives in ready_to_run in the JSON, which the TCP server streams to
# the ClearCore. Never derive the text from the encoder's own state.
STATUS_TEXT_RUNNING = "Active"
STATUS_TEXT_STOPPED = "Paused"

# Calibration screen. The button writes 1 into CAL_TRIGGER_VAR the same way the
# run screen's toggle does; poll relays it to the ClearCore as calibrate_request
# in the JSON and renders the controller's reported cal_state back here.
CAL_SCREEN      = 19
CAL_TRIGGER_VAR = 4  # numeric latch, default 0, set to 1 on button press
CAL_STATUS_VAR  = 3  # string var showing calibration progress

# cal_state values as reported by the sketch, mirrored into the JSON by the TCP
# server from its UDP listener.
CAL_STATE_WAITING   = 0
CAL_STATE_MEASURING = 1
CAL_STATE_DONE      = 2

# Calibration is NOT a bounded operation: the sketch only measures when the
# machine is running AND a tray physically breaks the laser gate. So the waiting
# text has to tell the operator to run a tray, or the screen looks hung.
CAL_TEXT = {
    CAL_STATE_WAITING:   "Run a tray to calibrate",
    CAL_STATE_MEASURING: "Measuring...",
    CAL_STATE_DONE:      "Done calibrating",
}
CAL_TEXT_UNKNOWN = "Waiting for controller"

# Give up on an unacknowledged calibration request after this long and drop the
# flag. The ack is the controller reporting cal_state != DONE; if it never comes
# (controller offline, link down, or running firmware that predates the
# handshake) the request would otherwise stay latched high forever — and because
# the sketch triggers on the RISING edge, a stuck-high flag means pressing
# Calibrate again does nothing. Clearing it restores a working button.
CAL_REQUEST_TIMEOUT_SEC = 15.0

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
        "variety_names": { "1": "...", "2": "...", ... },
        "1": { ... variety 1 data ... },
        "2": { ... variety 2 data ... },
        ...
      }
    """
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with FileLock(LOCK_FILE_PATH, shared=False):
            if not os.path.exists(path):
                default_names = {str(i): str(i) for i in range(1, NUM_VARIETIES + 1)}
                with open(path, "w") as f:
                    json.dump({
                        "ready_to_run": False,
                        "active_variety": None,
                        "variety_names": default_names,
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

def set_string_var(screen_id: int, var_id: int, value: str) -> bool:
    """Write a string variable to the encoder (no read-back verify)."""
    global te
    try:
        te.guide.set_var(ScreenID(screen_id), VariableID(var_id), VariableData(str(value)))
        return True
    except Exception as e:
        print(f"Error setting string var s{screen_id} v{var_id}: {e}")
        return False

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
    irrigation_delay: int,
    irrigation_duration: int,
    misting_delay: int,
    misting_duration: int,
    roller_delay: int,
    roller_duration: int,
):
    data = locked_read_json(JSON_FILE_PATH) or {}
    key = str(int(variety_index))
    # belt_speed no longer has a Touch Encoder screen — the conveyor runs off the
    # VFD dial and the sketch autocalibrates. The web app still reads this key,
    # so carry whatever is already on disk through untouched instead of dropping
    # it. Poll neither sources nor overwrites the value.
    existing = data.get(key)
    prior_belt_speed = existing.get("belt_speed", 0) if isinstance(existing, dict) else 0
    data[key] = {
        "roller_speed": int(roller_speed),
        "belt_speed": prior_belt_speed,
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

def set_calibrate_request(flag: bool):
    """
    Raise or clear the calibration request for the ClearCore.

    The TCP server relays this as a CSV field and the sketch edge-triggers on
    the 0->1 transition. We hold it high until the controller confirms it acted
    (cal_state leaves "done"), then clear it — that round trip IS the ack.
    """
    data = locked_read_json(JSON_FILE_PATH) or {}
    if bool(data.get("calibrate_request", False)) == bool(flag):
        return
    data["calibrate_request"] = bool(flag)
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"calibrate_request set to {bool(flag)}")

def expire_calibrate_request(raised_at):
    """
    Drop a calibration request the controller never acknowledged.

    Returns the timestamp to carry into the next cycle, or None if no request is
    outstanding. A `raised_at` of None while the flag is set on disk means we
    inherited it — poll restarted, or the flag predates this session — so we
    adopt it and start the clock rather than letting it sit latched forever.
    """
    data = locked_read_json(JSON_FILE_PATH) or {}
    if not bool(data.get("calibrate_request", False)):
        return None

    now = time.monotonic()
    if raised_at is None:
        return now
    if (now - raised_at) < CAL_REQUEST_TIMEOUT_SEC:
        return raised_at

    print("calibrate_request: no controller ack, clearing so the button works again")
    set_calibrate_request(False)
    return None

def get_variety_name(variety_index: int) -> str:
    """Look up the display name for a variety index from JSON."""
    data = locked_read_json(JSON_FILE_PATH) or {}
    names = data.get("variety_names", {})
    return names.get(str(int(variety_index)), str(variety_index))

def write_variety_to_screen(variety_index: int, screen_id: int = VARIETY_NAME_SCREEN, var_id: int = VARIETY_NAME_VAR):
    """Write the variety name string to a TE screen variable."""
    global te
    name = get_variety_name(variety_index)
    try:
        te.guide.set_var(
            ScreenID(screen_id),
            VariableID(var_id),
            VariableData(name),
        )
        print(f"Variety display s{screen_id}v{var_id}: [{variety_index}/{NUM_VARIETIES}] {name}")
    except Exception as e:
        print(f"ERROR: writing variety name to s{screen_id}v{var_id}: {e}")

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
    set_variable(VARIETY_NAME_SCREEN, 1, active_variety)  # restore selection index
    write_variety_to_screen(active_variety)               # restore name on screen 10
    # Repaint the running screen too. Without this it keeps whatever name was
    # there before the restart, which would claim the machine is running a
    # variety it isn't.
    write_variety_to_screen(
        active_variety, RUNNING_VARIETY_SCREEN, RUNNING_VARIETY_VAR
    )
    set_variable(6, 1, v.get("roller_speed", 0))        # Roller Speed
    # Screen 3 (Belt Speed) was removed from the TE menu — nothing to restore.
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
        # Write variety name before navigating to prevent flash of default string
        write_variety_to_screen(1)
        te.guide.set_screen(ScreenID(10))
    except Exception as e:
        print(f"Error setting initial screen 10: {e}")

    # Track the last index we wrote a name for, so we only push on change
    last_shown_index = None

    # Per-screen display caches — only push to encoder when the text changes,
    # and each doubles as the "just entered that screen" signal (None == not on
    # it last tick). Each screen's branch clears the OTHER cache, so navigating
    # 18 <-> 19 directly still re-runs the stale-press guard on arrival.
    last_state_status = None
    last_cal_text = None

    # When the outstanding calibration request was raised, for the ack timeout.
    cal_request_raised_at = None

    while True:
        # Get current screen (with retries)
        active_screen = safe_get_screen()

        # Checked every cycle, not just on the calibration screen: the request
        # flag is relayed to the ClearCore continuously regardless of what the
        # operator is looking at, so it has to be able to expire from anywhere.
        cal_request_raised_at = expire_calibrate_request(cal_request_raised_at)

        # On the selection screen, the encoder scrolls its own numeric variable
        # (screen 10 / var 1). Mirror that selection to the name string (var 7).
        if active_screen == ScreenID(VARIETY_NAME_SCREEN):
            sel = get_variable(VARIETY_NAME_SCREEN, 1)
            if sel is not None and sel != last_shown_index:
                write_variety_to_screen(sel)
                last_shown_index = sel
        else:
            last_shown_index = None

        # -----------------------------
        # Screen 9: Save Confirmation
        # -----------------------------
        if active_screen == ScreenID(9):
            variety_index = get_variable(10, 1)     # current variety selection
            roller_speed = get_variable(6, 1)       # roller speed
            irrigation_delay = get_variable(11, 1)  # irrigation delay
            irrigation_duration = get_variable(12, 1)
            misting_delay = get_variable(13, 1)
            misting_duration = get_variable(14, 1)
            roller_delay = get_variable(15, 1)
            roller_duration = get_variable(16, 1)

            if None not in (
                variety_index,
                roller_speed,
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

            # Show the user feedback FIRST: push the name onto the running screen
            # and navigate there immediately. The value loads below take ~8
            # set_var round-trips, so doing the visual confirmation up front
            # makes the selection feel instant.
            write_variety_to_screen(
                variety_index, RUNNING_VARIETY_SCREEN, RUNNING_VARIETY_VAR
            )
            try:
                te.guide.set_screen(ScreenID(RUNNING_VARIETY_SCREEN))
            except Exception as e:
                print(f"Error setting screen {RUNNING_VARIETY_SCREEN}: {e}")

            save_active_variety(variety_index)  # Save active variety to JSON
            saved_data = load_variety_data()

            key = str(variety_index)
            if key in saved_data and isinstance(saved_data[key], dict):
                saved_values = saved_data[key]

                # Push saved values into the encoder (done after navigation
                # so the user already sees the new screen).
                set_variable(6, 1, saved_values.get("roller_speed", 0))        # Roller Speed
                # Screen 3 (Belt Speed) was removed from the TE menu — not pushed.
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

        # ---------------------------------------------
        # Screen 18: Run screen (variety name + Active/Paused + toggle button)
        # ---------------------------------------------
        elif active_screen == ScreenID(STATE_SCREEN):
            last_cal_text = None  # arriving here means we left the cal screen

            # On first arrival, zero the press var so the stale "Set value"
            # default from GUIDE isn't misread as a real tap. last_state_status
            # being None is our "just entered the run screen" signal.
            first_entry = last_state_status is None
            if first_entry:
                set_variable(STATE_SCREEN, STATE_BTN_PRESS_VAR, 0)

            # ready_to_run in the JSON is the single source of truth — not the
            # encoder. Deriving the text from it (rather than flipping whatever
            # is currently displayed) is what makes the screen repaint correctly
            # after a poll restart, a TE reconnect, or a change made elsewhere.
            data = locked_read_json(JSON_FILE_PATH) or {}
            running = bool(data.get("ready_to_run", False))

            status_text = STATUS_TEXT_RUNNING if running else STATUS_TEXT_STOPPED
            if status_text != last_state_status:
                set_string_var(STATE_SCREEN, STATE_STATUS_VAR, status_text)
                last_state_status = status_text

            # The button ("Set value" action in GUIDE) writes 1 into the press
            # var on tap. Treat any nonzero as a press: repaint the encoder
            # first so the UI feels immediate, THEN flip ready_to_run.
            # Skip on first_entry — we just zeroed the var ourselves.
            try:
                pressed = 0 if first_entry else safe_get_var(STATE_SCREEN, STATE_BTN_PRESS_VAR)
            except Exception:
                pressed = 0
            if pressed:
                new_running = not running
                new_status_text = (
                    STATUS_TEXT_RUNNING if new_running else STATUS_TEXT_STOPPED
                )
                set_string_var(STATE_SCREEN, STATE_STATUS_VAR, new_status_text)
                last_state_status = new_status_text
                ready_to_run_toggle(new_running)
                # Consume the press so the latch is re-armed. Without this the
                # var stays 1 and the machine would toggle every poll cycle.
                set_variable(STATE_SCREEN, STATE_BTN_PRESS_VAR, 0)

        # ---------------------------------------------
        # Screen 19: Calibration (status text + calibrate button)
        # ---------------------------------------------
        elif active_screen == ScreenID(CAL_SCREEN):
            last_state_status = None  # arriving here means we left the run screen

            # Same stale-press guard as the run screen: zero the latch on
            # arrival and skip one read, so a press left unconsumed from a
            # previous visit can't fire the moment we navigate back.
            cal_first_entry = last_cal_text is None
            if cal_first_entry:
                set_variable(CAL_SCREEN, CAL_TRIGGER_VAR, 0)

            data = locked_read_json(JSON_FILE_PATH) or {}
            cal_state = data.get("cal_state", None)
            requested = bool(data.get("calibrate_request", False))

            cal_text = CAL_TEXT.get(cal_state, CAL_TEXT_UNKNOWN)
            if cal_text != last_cal_text:
                set_string_var(CAL_SCREEN, CAL_STATUS_VAR, cal_text)
                last_cal_text = cal_text

            # The controller has left CAL_DONE, so it has seen our request and
            # started over. That transition is the ack — drop the flag so the
            # next press produces a fresh rising edge on the ClearCore.
            if requested and cal_state is not None and cal_state != CAL_STATE_DONE:
                set_calibrate_request(False)
                cal_request_raised_at = None

            try:
                pressed = (
                    0 if cal_first_entry
                    else safe_get_var(CAL_SCREEN, CAL_TRIGGER_VAR)
                )
            except Exception:
                pressed = 0
            if pressed:
                # Repaint first so the button feels responsive, then raise the
                # request. Showing the waiting text immediately also tells the
                # operator what has to happen next: run a tray.
                set_string_var(CAL_SCREEN, CAL_STATUS_VAR,
                               CAL_TEXT[CAL_STATE_WAITING])
                last_cal_text = CAL_TEXT[CAL_STATE_WAITING]
                set_calibrate_request(True)
                cal_request_raised_at = time.monotonic()
                set_variable(CAL_SCREEN, CAL_TRIGGER_VAR, 0)

        else:
            # Leaving the state screen — invalidate the caches so re-entry
            # repaints, and so the next arrival re-runs the first_entry guard.
            last_state_status = None
            last_cal_text = None

        time.sleep(POLL_INTERVAL_SEC)

# ========================
# Main
# ========================
def main():
    global te

    ensure_json_exists(JSON_FILE_PATH)

    # Fail-safe: clear any stale ready_to_run persisted from a previous session
    # BEFORE discovery/restore. The TCP server is a separate process that streams
    # this JSON to the ClearCore continuously; if we leave ready_to_run=true on
    # disk during the ~2s discover+restore window, the belt starts on its own
    # until the monitor loop finally forces it false. Stop first, then bring up.
    ready_to_run_toggle(False)

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
                # Same fail-safe as startup: clear stale ready_to_run before the
                # restore window so the belt can't self-start during reconnect.
                ready_to_run_toggle(False)
                try:
                    # Write variety name before navigating to prevent flash of default string
                    write_variety_to_screen(1)
                    te.guide.set_screen(ScreenID(10))
                except Exception:
                    pass
                # After reconnect, try to restore the last active variety
                restore_vars_if_reset()


if __name__ == "__main__":
    main()

