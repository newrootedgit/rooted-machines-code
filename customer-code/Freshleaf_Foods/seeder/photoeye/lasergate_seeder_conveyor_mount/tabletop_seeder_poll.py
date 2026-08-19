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

# Valid min/max for each variety field, written into a freshly created settings
# file. The live ranges are READ FROM THE SETTINGS FILE at runtime — see
# load_variable_ranges() — so this is only the seed for a machine that has no
# settings yet. Never validate against this constant directly, or a machine
# whose ranges were tuned on site would be checked against the wrong bounds.
#
# Note the delay/duration fields legitimately go NEGATIVE: they are offsets, not
# elapsed times. Any "nothing can be negative" rule would reject valid presets.
DEFAULT_VARIABLE_RANGES = {
    "roller_speed":        {"min": 0,    "max": 50},
    "belt_speed":          {"min": 0,    "max": 10},
    "irrigation_delay":    {"min": -100, "max": 100},
    "irrigation_duration": {"min": -100, "max": 100},
    "misting_delay":       {"min": -100, "max": 100},
    "misting_duration":    {"min": -100, "max": 100},
    "roller_delay":        {"min": -100, "max": 100},
    "roller_duration":     {"min": -100, "max": 100},
}

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

# Shutdown button, on screen 2. Configured in GUIDE with Action="Set value"
# writing 1 into var 5 (default 0), exactly like the run screen's toggle. Poll
# sees nonzero, parks the machine and asks the OS to halt.
#
# Var 5 here is NOT the same variable as STATE_BTN_PRESS_VAR — that one is var 5
# on screen 18. Same id, different screen, different widget.
#
# WHY: this machine is unplugged at the end of the day. Without a clean halt
# first that is an unclean shutdown a few hundred times a year, and one of those
# is what corrupted both of this Pi's Python scripts on 2026-08-11 — runs of
# 0xFF on clean 4 KiB boundaries, both services crash-looping. The routine this
# supports: press Shut Down, wait for the encoder screen to go dark, pull the
# plug.
SHUTDOWN_SCREEN        = 2
SHUTDOWN_BTN_PRESS_VAR = 5  # numeric latch, default 0, set to 1 on button press

# String var on screen 2 that poll writes progress into, so the operator can see
# the machine responding. The halt takes a few seconds and the encoder shows
# nothing on its own, which reads as a button that did not work — and an
# operator who believes that reaches for the plug, which is the exact unclean
# shutdown this feature exists to prevent.
#
# This is the STRING variable the screen 2 text field is bound to in GUIDE.
# Keep the field wide enough for the longest string below (16 characters);
# screen 19's field clips at about 11.
SHUTDOWN_STATUS_VAR = 2

# Progress strings. IDLE is written on arrival so a message left over from a
# previous visit does not greet the next operator. It must match the static
# label the GUIDE project puts on this field, or the text the operator sees
# vanishes a fraction of a second after they navigate to the screen.
SHUTDOWN_TEXT_IDLE   = "Shut Down"
SHUTDOWN_TEXT_BUSY   = "Shutting down"
SHUTDOWN_TEXT_FAILED = "HALT FAILED"

# Where the halt is requested. /run is tmpfs, so the request never touches
# storage — and it is cleared at boot for free, which means a request that
# somehow survived cannot re-trigger a shutdown on the next start.
SHUTDOWN_REQUEST_PATH = "/run/rooted/shutdown-request"

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

# Keep these SHORT — the screen 19 text field clips anything much longer than
# ~11 characters. "Calibrated" is the longest string here at 10.
#
# The screen walks the operator through the whole operation:
#
#   arrive          -> "Calibrate"   nothing has been asked for yet
#   press button    -> "Run Tray"    armed and waiting for a tray
#   tray in gate    -> "Measuring"   the controller registered the tray
#   measured        -> "Calibrated"  then bounce back to the variety screen
#
# "Calibrate" vs "Run Tray" cannot be told apart from cal_state — both are
# CAL_STATE_WAITING on an uncalibrated machine. The difference is whether THIS
# operator has pressed the button, which only poll knows, so the screen is
# driven by a local session (see cal_ui in the monitor loop) rather than by
# cal_state alone.
CAL_TEXT_IDLE      = "Calibrate"   # on arrival, before anything is requested
CAL_TEXT_UNKNOWN   = "No link"     # controller not reporting

# In-session text, keyed by the controller's reported cal_state.
CAL_TEXT = {
    CAL_STATE_WAITING:   "Run Tray",
    CAL_STATE_MEASURING: "Measuring",
    CAL_STATE_DONE:      "Calibrated",
}

# How long "Calibrated" stays up before the screen returns to variety select.
# Long enough to read, short enough that nobody wonders if it's stuck.
CAL_DONE_DWELL_SEC = 2.5

# Give up on an unacknowledged calibration request after this long and drop the
# flag. The ack is the controller reporting cal_state != DONE; if it never comes
# (controller offline, link down, or running firmware that predates the
# handshake) the request would otherwise stay latched high forever — and because
# the sketch triggers on the RISING edge, a stuck-high flag means pressing
# Calibrate again does nothing. Clearing it restores a working button.
CAL_REQUEST_TIMEOUT_SEC = 15.0

# Pressing Calibrate arms the machine on the operator's behalf. The sketch only
# advances its calibration state machine while ready_to_run is true, so without
# this the operator has to select a variety and press Activate first — and the
# screen just sits on "Run a tray" while trays are ignored, which reads as a
# hang. See the ready_to_run_flag gate in autocalibrate_photoeye.ino.
#
# We only auto-arm if the machine was paused, and we put it back to paused once
# calibration completes. Nothing actuates while calibrating, so the machine is
# inert for the whole auto-armed window; handing control back at CAL_DONE means
# it never goes live off the back of a Calibrate press.
#
# Safety cap: if calibration never completes — no tray is ever run, the dwell
# keeps getting rejected — disarm anyway rather than leaving a machine armed
# because somebody pressed a button and walked away.
CAL_AUTOARM_TIMEOUT_SEC = 180.0

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
                        # Seeded here so a machine that regenerates its settings
                        # still knows what a valid value looks like. Without it
                        # the range checks below silently do nothing.
                        "variable_ranges": DEFAULT_VARIABLE_RANGES,
                        "variety_names": default_names,
                    }, f, indent=4)

def fsync_dir(dir_name: str):
    """
    Flush a directory entry so a rename is durable, not just the file contents.

    os.replace() is atomic — a reader never sees a half-written file — but on
    ext4 the rename itself only reaches the card when the journal commits,
    which defaults to every 5 seconds. Without this, pulling power seconds
    after saving a variety can silently roll that save back. The data is never
    corrupted either way; this closes the window where it's merely lost.

    Best effort: not every platform allows opening a directory (Windows does
    not), and where it fails the file-level fsync above has still happened.
    """
    try:
        fd = os.open(dir_name, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass

def backup_settings(path: str):
    """
    Keep a known-good copy of the settings file alongside it.

    Written after a successful variety save — the only time preset CONTENT
    changes — so the copy always holds a complete, parseable set. Costs one
    extra ~6 KB write per operator save, which is nothing next to the telemetry
    stream, and it is the difference between "presets are gone" and "presets are
    one cp away".

    Atomic for the same reason the main file is: a torn backup is worse than no
    backup, because it looks like a recovery option and isn't.
    """
    backup = path + ".bak"
    dir_name = os.path.dirname(path)
    try:
        with open(path, "rb") as src:
            blob = src.read()
        # Refuse to overwrite a good backup with something unparseable.
        json.loads(blob.decode("utf-8"))
        fd, tmp_path = tempfile.mkstemp(prefix=".tmpbak_", dir=dir_name)
        try:
            with os.fdopen(fd, "wb") as tmpf:
                tmpf.write(blob)
                tmpf.flush()
                os.fsync(tmpf.fileno())
            os.replace(tmp_path, backup)
            fsync_dir(dir_name)
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
    except (OSError, ValueError, UnicodeDecodeError) as e:
        print(f"WARNING: could not back up {path}: {e}")

def ensure_settings_backup(path: str):
    """
    Create the known-good copy at startup if one does not exist yet.

    backup_settings() otherwise only runs after an operator saves a variety, so
    a machine nobody has saved on has no .bak at all — and recover_settings_if_needed()
    below then has nothing to restore from. The recovery path exists but is
    disarmed, which is exactly the state the Freshleaf machine was found in after
    an SD corruption: settings intact by luck, no backup, and no way to tell.

    Only when the backup is MISSING. An existing .bak is the last state an
    operator explicitly saved; refreshing it every boot from whatever happens to
    be on disk would let a damaged-but-loadable file quietly replace a good copy.
    """
    backup = path + ".bak"
    if os.path.exists(backup) or not os.path.exists(path):
        return
    print(f"no backup at {backup}; creating one from the current settings")
    backup_settings(path)

def recover_settings_if_needed(path: str):
    """
    Restore presets from the backup if the live file is unreadable.

    Runs once at startup rather than from the read path: recovery needs an
    exclusive lock and a well-defined moment, and the machine is not yet doing
    anything. Without this an unattended machine silently comes up with zero
    presets and nobody finds out until an operator goes looking for them.
    """
    if not os.path.exists(path):
        return
    try:
        with open(path, "r") as f:
            json.load(f)
        return  # live file is fine
    except (OSError, json.JSONDecodeError):
        pass

    backup = path + ".bak"
    try:
        with open(backup, "r") as f:
            restored = json.load(f)
    except (OSError, json.JSONDecodeError):
        print(f"WARNING: {path} is unreadable and no usable backup exists at "
              f"{backup}. Variety presets will need to be re-entered.")
        quarantine_corrupt_json(path)
        return

    quarantine_corrupt_json(path)
    locked_atomic_write_json(path, restored)
    varieties = len([k for k in restored if str(k).isdigit()])
    print(f"RECOVERED {path} from {backup} ({varieties} variety presets restored)")

def quarantine_corrupt_json(path: str):
    """
    Preserve a copy of an unparseable settings file, once.

    Copies rather than moves: the machine has to keep running, and the callers
    all treat a missing file as "start fresh". Only the FIRST corruption is kept
    — a later good save followed by another corruption would otherwise overwrite
    the copy that still had the operator's presets in it.
    """
    backup = path + ".corrupt"
    try:
        if os.path.exists(backup):
            print(f"WARNING: {path} is unreadable; existing copy at {backup}")
            return
        with open(path, "rb") as src, open(backup, "wb") as dst:
            dst.write(src.read())
        print(f"WARNING: {path} is unreadable — variety presets may be lost. "
              f"A copy was kept at {backup} for recovery.")
    except OSError as e:
        print(f"WARNING: {path} is unreadable and could not be copied: {e}")

def locked_read_json(path: str) -> Dict:
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(path, "r") as f:
                return json.load(f)
        except FileNotFoundError:
            return {}
        except json.JSONDecodeError:
            # A damaged file reads as "no presets", and the next save would then
            # write a fresh file containing only that one variety — turning
            # recoverable damage into permanent loss. Keep a copy before anyone
            # overwrites it, and say so loudly rather than failing silently.
            quarantine_corrupt_json(path)
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
            fsync_dir(dir_name)
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

def load_variable_ranges(data: Dict) -> Dict:
    """
    Return the min/max declared for each variety field by the settings file.

    Read from the file rather than from DEFAULT_VARIABLE_RANGES so a machine
    whose limits were tuned on site is checked against ITS limits. Returns {}
    when the file predates the ranges block, which disables the checks below —
    the right failure direction, since inventing bounds risks rejecting presets
    an operator legitimately saved.
    """
    ranges = data.get("variable_ranges")
    return ranges if isinstance(ranges, dict) else {}

def preset_issues(values: Dict, ranges: Dict) -> list:
    """
    Return human-readable problems with one variety's stored values.

    Only fields the settings file declares a range for are checked, and only
    against that declared range. Note that most of these fields are offsets and
    legitimately go negative (-100..100), so the sign of a value says nothing on
    its own.
    """
    issues = []
    for field, bounds in ranges.items():
        if field not in values or not isinstance(bounds, dict):
            continue
        v = values[field]
        # bool is an int subclass; a True in a numeric slot is damage, not a value.
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            issues.append(f"{field}={v!r} is not a number")
            continue
        lo, hi = bounds.get("min"), bounds.get("max")
        if isinstance(lo, (int, float)) and v < lo:
            issues.append(f"{field}={v} below min {lo}")
        elif isinstance(hi, (int, float)) and v > hi:
            issues.append(f"{field}={v} above max {hi}")
    return issues

def audit_stored_presets(path: str):
    """
    Log any stored variety values that fall outside their declared range.

    Damaged storage does not always produce unparseable JSON — a corrupted block
    can land inside a number and leave a file that loads cleanly and carries a
    value nothing else would question. This only reports; it never edits. The
    operator re-saves the variety, which is the one action that can produce a
    correct value.
    """
    data = locked_read_json(path) or {}
    ranges = load_variable_ranges(data)
    if not ranges:
        print("audit_stored_presets: settings declare no variable_ranges; "
              "skipping the range check")
        return

    for key in sorted((k for k in data if str(k).isdigit()), key=int):
        v = data.get(key)
        if not isinstance(v, dict):
            continue
        issues = preset_issues(v, ranges)
        if issues:
            print(f"WARNING: variety {key} has out-of-range stored values — "
                  f"{'; '.join(issues)}. Re-enter and save this variety.")

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
    entry = {
        "roller_speed": int(roller_speed),
        "belt_speed": prior_belt_speed,
        "irrigation_delay": int(irrigation_delay),
        "irrigation_duration": int(irrigation_duration),
        "misting_delay": int(misting_delay),
        "misting_duration": int(misting_duration),
        "roller_delay": int(roller_delay),
        "roller_duration": int(roller_duration),
    }

    # A garbled encoder read must not become a stored preset. Checked against
    # the ranges the settings file itself declares, so this can only reject a
    # value the machine already considers invalid — it cannot second-guess a
    # legitimate save. The previous values stay on disk, which is the safer
    # outcome: keeping the last known-good preset beats overwriting it with
    # something the machine says is impossible.
    issues = preset_issues(entry, load_variable_ranges(data))
    if issues:
        print(f"REFUSING to save variety {key} — {'; '.join(issues)}. "
              f"Previous values left in place.")
        return

    data[key] = entry
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"Saved variety {key} to JSON")
    # Preset content just changed — refresh the known-good copy.
    backup_settings(JSON_FILE_PATH)

def save_active_variety(variety_index: int):
    data = locked_read_json(JSON_FILE_PATH) or {}
    data["active_variety"] = int(variety_index)
    locked_atomic_write_json(JSON_FILE_PATH, data)
    print(f"Set active_variety = {variety_index}")

def request_shutdown() -> bool:
    """
    Ask the system to halt, without giving this process the privilege to do it.

    poll runs as `rooted` under NoNewPrivileges=true, so it cannot call poweroff
    and should not be able to — a machine-control service does not need the
    right to switch the computer off. It writes a flag into /run instead; a
    systemd .path unit watches for that file and triggers a root oneshot that
    flushes the journal, syncs and halts. See install-shutdown-button.sh.

    Returning False matters: if the request cannot be written, the operator is
    about to unplug a machine that is still running, which is exactly the
    unclean shutdown this whole button exists to avoid. Say so in the journal.
    """
    try:
        os.makedirs(os.path.dirname(SHUTDOWN_REQUEST_PATH), exist_ok=True)
        with open(SHUTDOWN_REQUEST_PATH, "w") as f:
            f.write("requested from the Touch Encoder shutdown button\n")
            f.flush()
            os.fsync(f.fileno())
        print(f"shutdown: requested via {SHUTDOWN_REQUEST_PATH}")
        return True
    except OSError as e:
        print(f"ERROR: shutdown requested but {SHUTDOWN_REQUEST_PATH} could not "
              f"be written ({e}). THE MACHINE WILL NOT HALT — do not unplug it "
              f"until it has been shut down another way.")
        return False

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

def new_cal_ui() -> Dict:
    """
    Fresh calibration-screen session state.

      started_at  when the operator pressed Calibrate, or None if idle
      acked       True once the controller confirmed it started over, after
                  which cal_state describes THIS calibration rather than the
                  previous one
      done_at     when "Calibrated" first went up, for the dwell before we
                  return the operator to the variety screen

    Deliberately survives navigating away from screen 19 — the operator usually
    has to walk to the machine to run the tray, and should come back to live
    progress rather than a reset screen.
    """
    return {"started_at": None, "acked": False, "done_at": None}

def resolve_calibration_autoarm(armed_at):
    """
    Release an auto-arm once calibration finishes, or after the safety cap.

    `armed_at` is the monotonic time we armed the machine for calibration, or
    None if we didn't. Returns the value to carry into the next cycle. Called
    every cycle from any screen, because the operator will usually navigate away
    to run the tray that completes the calibration.
    """
    if armed_at is None:
        return None

    data = locked_read_json(JSON_FILE_PATH) or {}

    # Operator took explicit control while we were armed — their choice wins,
    # so stop tracking rather than overriding them when calibration lands.
    if not bool(data.get("ready_to_run", False)):
        return None

    if data.get("cal_state", None) == CAL_STATE_DONE:
        print("calibration complete; releasing auto-arm")
        ready_to_run_toggle(False)
        return None

    if (time.monotonic() - armed_at) >= CAL_AUTOARM_TIMEOUT_SEC:
        print("calibration did not complete in time; releasing auto-arm")
        ready_to_run_toggle(False)
        return None

    return armed_at

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

    # When we armed the machine on the operator's behalf for a calibration, so
    # we know to hand it back to paused when the calibration lands.
    cal_autoarm_at = None

    # Calibration screen session — drives the Calibrate/Run Tray/Measuring/
    # Calibrated walkthrough. See new_cal_ui().
    cal_ui = new_cal_ui()

    # Shutdown screen state. None means "was not on screen 2 last tick", which
    # doubles as the just-arrived signal for the stale-press guard below.
    last_shutdown_seen = None

    # Latches once the operator has asked to shut down. The loop runs every
    # 300 ms and the machine takes a few seconds to halt, so without this the
    # request would be re-issued a dozen times on the way down.
    shutdown_requested = False

    # Drives the animated dots on the shutdown screen while the machine halts.
    shutdown_tick = 0

    while True:
        # Get current screen (with retries)
        active_screen = safe_get_screen()

        # ---------------------------------------------
        # Screen 2: Shut Down button
        # ---------------------------------------------
        # Handled before anything else so the halt is not delayed behind
        # encoder round-trips, and `continue` because nothing else is worth
        # doing on the way down.
        if active_screen == ScreenID(SHUTDOWN_SCREEN):
            # Stale-press guard, same as the run and calibration screens: zero
            # the latch on arrival and skip one read. The encoder is USB powered
            # and dies with the Pi, so a press left unconsumed as the machine
            # halted could otherwise fire the instant someone opens screen 2 on
            # the next boot — a machine that shuts itself down when you look at
            # the wrong screen.
            shutdown_first_entry = last_shutdown_seen is None
            if shutdown_first_entry:
                set_variable(SHUTDOWN_SCREEN, SHUTDOWN_BTN_PRESS_VAR, 0)
                # Clear anything left from a previous visit or a failed halt,
                # so the operator is never met by a stale "Shutting down".
                set_string_var(SHUTDOWN_SCREEN, SHUTDOWN_STATUS_VAR,
                               SHUTDOWN_TEXT_IDLE)
            last_shutdown_seen = True

            # Already on the way down. Keep the dots moving rather than holding
            # one static string: a frozen message is indistinguishable from a
            # hung machine, and the whole point is to show it is still working.
            # Nothing else runs here — the halt is the only thing left to do.
            if shutdown_requested:
                shutdown_tick += 1
                set_string_var(
                    SHUTDOWN_SCREEN, SHUTDOWN_STATUS_VAR,
                    SHUTDOWN_TEXT_BUSY + ("." * (shutdown_tick % 4)))
                time.sleep(POLL_INTERVAL_SEC)
                continue

            try:
                pressed = 0 if shutdown_first_entry else safe_get_var(
                    SHUTDOWN_SCREEN, SHUTDOWN_BTN_PRESS_VAR)
            except Exception:
                pressed = 0

            if pressed:
                print("shutdown: operator pressed the shut down button")
                # Repaint BEFORE doing any work. set_string_var is a single USB
                # write with no read-back, so the operator sees the machine
                # respond within one 300 ms tick instead of waiting on a JSON
                # write and an fsync first.
                set_string_var(SHUTDOWN_SCREEN, SHUTDOWN_STATUS_VAR,
                               SHUTDOWN_TEXT_BUSY)
                shutdown_requested = True
                shutdown_tick = 0
                # Park the machine first. The TCP server keeps streaming this
                # JSON to the ClearCore until the moment the OS goes away, so
                # the last thing the controller reads should say "not ready to
                # run" rather than leaving it armed as the link disappears.
                ready_to_run_toggle(False)
                if not request_shutdown():
                    # The halt will not happen. Say so on the screen — silently
                    # returning to idle would look like the press was ignored,
                    # and the operator would unplug a running machine.
                    set_string_var(SHUTDOWN_SCREEN, SHUTDOWN_STATUS_VAR,
                                   SHUTDOWN_TEXT_FAILED)
                    # Re-arm so the operator can try again rather than being
                    # left tapping a dead button.
                    shutdown_requested = False
                # Consume the press either way, so a halt that fails does not
                # leave the latch stuck high for the retry.
                set_variable(SHUTDOWN_SCREEN, SHUTDOWN_BTN_PRESS_VAR, 0)

            time.sleep(POLL_INTERVAL_SEC)
            continue
        else:
            last_shutdown_seen = None

        # Checked every cycle, not just on the calibration screen: the request
        # flag is relayed to the ClearCore continuously regardless of what the
        # operator is looking at, so it has to be able to expire from anywhere.
        cal_request_raised_at = expire_calibrate_request(cal_request_raised_at)

        # Likewise from any screen: the operator has to walk away from screen 19
        # to run the tray that completes the calibration.
        cal_autoarm_at = resolve_calibration_autoarm(cal_autoarm_at)

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
                # The operator has taken explicit control of the run state, so
                # drop any calibration auto-arm rather than overriding them when
                # the calibration lands.
                cal_autoarm_at = None
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

            # The controller has left CAL_DONE, so it has seen our request and
            # started over. That transition is the ack — drop the flag so the
            # next press produces a fresh rising edge on the ClearCore, and only
            # from here on does cal_state describe THIS calibration.
            if requested and cal_state is not None and cal_state != CAL_STATE_DONE:
                set_calibrate_request(False)
                cal_request_raised_at = None
                cal_ui["acked"] = True

            # Give up on a session the controller never acknowledged, so the
            # screen falls back to "Calibrate" and the operator can retry
            # instead of staring at "Run Tray" forever.
            if (cal_ui["started_at"] is not None and not cal_ui["acked"]
                    and (time.monotonic() - cal_ui["started_at"]) >= CAL_REQUEST_TIMEOUT_SEC):
                print("calibration request went unacknowledged; resetting the screen")
                cal_ui = new_cal_ui()

            # ---- choose the text ----
            if cal_state is None:
                cal_text = CAL_TEXT_UNKNOWN
            elif cal_ui["started_at"] is None:
                # Nothing asked for yet. Show the invitation, not the machine's
                # standing state — arriving on an already-calibrated machine
                # should still offer "Calibrate".
                cal_text = CAL_TEXT_IDLE
            elif not cal_ui["acked"]:
                # We've asked but the controller hasn't reset yet. cal_state is
                # still the PREVIOUS result, so honouring it here would flash
                # "Calibrated" and bounce the operator off the screen instantly.
                cal_text = CAL_TEXT[CAL_STATE_WAITING]
            else:
                cal_text = CAL_TEXT.get(cal_state, CAL_TEXT_UNKNOWN)

            if cal_text != last_cal_text:
                set_string_var(CAL_SCREEN, CAL_STATUS_VAR, cal_text)
                last_cal_text = cal_text

            # ---- finished: hold "Calibrated", then return to variety select ----
            if cal_ui["acked"] and cal_state == CAL_STATE_DONE:
                if cal_ui["done_at"] is None:
                    cal_ui["done_at"] = time.monotonic()
                elif (time.monotonic() - cal_ui["done_at"]) >= CAL_DONE_DWELL_SEC:
                    print("calibration finished; returning to variety select")
                    cal_ui = new_cal_ui()
                    last_cal_text = None
                    try:
                        te.guide.set_screen(ScreenID(VARIETY_NAME_SCREEN))
                    except Exception as e:
                        print(f"Error returning to screen {VARIETY_NAME_SCREEN}: {e}")

            try:
                pressed = (
                    0 if cal_first_entry
                    else safe_get_var(CAL_SCREEN, CAL_TRIGGER_VAR)
                )
            except Exception:
                pressed = 0
            if pressed:
                # Repaint first so the button feels responsive, then raise the
                # request. Showing "Run Tray" immediately also tells the operator
                # what has to happen next.
                set_string_var(CAL_SCREEN, CAL_STATUS_VAR,
                               CAL_TEXT[CAL_STATE_WAITING])
                last_cal_text = CAL_TEXT[CAL_STATE_WAITING]
                cal_ui = new_cal_ui()
                cal_ui["started_at"] = time.monotonic()
                set_calibrate_request(True)
                cal_request_raised_at = time.monotonic()

                # Arm the machine if it is paused, so the sketch will actually
                # advance its calibration state machine. Calibrating from a
                # cold, unselected, paused machine has to work — that is the
                # whole point of the button. resolve_calibration_autoarm() puts
                # it back to paused once the calibration lands.
                if not bool(data.get("ready_to_run", False)):
                    print("auto-arming for calibration")
                    ready_to_run_toggle(True)
                    cal_autoarm_at = time.monotonic()

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

    # Before anything reads the settings: if the live file was damaged (power
    # cut mid-write), restore it from the backup. Must run before the first
    # ready_to_run_toggle below, because that write would otherwise bake an
    # empty settings file over the damaged one.
    recover_settings_if_needed(JSON_FILE_PATH)

    # Arm the recovery path above for next time, then report anything that
    # loaded cleanly but cannot be a real value. Both run before discovery so
    # they land in the journal ahead of any device chatter.
    ensure_settings_backup(JSON_FILE_PATH)
    audit_stored_presets(JSON_FILE_PATH)

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

