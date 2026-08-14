#!/usr/bin/env python3
import os
import sys
import time
import json
import fcntl
import tempfile
from typing import Dict

from te.interface.common import ScreenID, Status, VariableID, VariableData
from te.interface.hid.hid_reports import GuideKnobEventReport
from te.utils.discovery_tool import pprint_discover_tes

# ========================
# Configuration
# ========================
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
JSON_PIN_FILE_PATH = "/home/rooted/te-cli/PIN_Values.json"

LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"
LOCK_PIN_FILE_PATH = JSON_PIN_FILE_PATH + ".lock"

POLL_INTERVAL_SEC = 0.3  # matches original monitor loop cadence
NUM_VARIETIES = 20       # total variety slots (1-20)

# Variety name display on Touch Encoder
VARIETY_NAME_SCREEN = 10   # Operator variety selection screen
VARIETY_NAME_VAR = 6       # VariableID for the name string on screen 10
EDIT_VARIETY_SCREEN = 26   # Edit mode variety selection screen
EDIT_VARIETY_VAR = 7       # VariableID for the name string on screen 26
CONFIRM_VARIETY_SCREEN = 18  # Operator confirmation screen
CONFIRM_VARIETY_VAR = 1      # VariableID for the name string on screen 18

# Screen 18 doubles as the run screen: it shows which variety is loaded AND
# carries the Start/Stop button, so STATE_SCREEN and CONFIRM_VARIETY_SCREEN are
# the same screen with different variables on it.
#   var 1 -> variety name    var 2 -> variety index    var 6 -> press latch
#
# The button is configured in GUIDE with Action="Set value" writing 1 into var 6
# (default 0). Poll detects nonzero, flips ready_to_run in the JSON, then writes
# 0 back so the latch is re-armed for the next press. Var 6 MUST be numeric —
# safe_get_var calls .to_int() on the read.
STATE_SCREEN        = CONFIRM_VARIETY_SCREEN
STATE_BTN_PRESS_VAR = 6

# Shutdown screen. The GUIDE project has a button that navigates here, and
# ARRIVING ON THIS SCREEN IS THE REQUEST — there is no latch variable and no
# confirmation step. Whatever text screen 4 carries in GUIDE is what the
# operator sees; poll writes nothing to it.
#
# This machine is unplugged at the end of every working day. Without a clean
# halt first that is an unclean shutdown roughly 250 times a year, which is the
# abuse that corrupted the Freshleaf seeder's storage on 2026-08-11. The routine
# this screen supports: press the button, wait for the screen to go dark, then
# pull the plug.
SHUTDOWN_SCREEN = 4

# Where the halt is requested. /run is tmpfs, so the request never touches
# storage — and it is cleared at boot for free, which means a request that
# somehow survives cannot re-trigger a shutdown on the next start.
SHUTDOWN_REQUEST_PATH = "/run/rooted/shutdown-request"

# Valid min/max for each variety field. Seeded into the settings file — see
# ensure_variable_ranges() — and read back from there at runtime, so a machine
# whose limits were tuned on site is checked against ITS limits rather than
# these. Never validate against this constant directly.
#
# Ranges for THIS machine, not the harvester this code came from. Note that
# roller_speed and roller_start_delay effectively swapped bounds relative to the
# harvester: screen 6 was 0-3 for blade speed and screen 16 was 0-250 for blade
# height. Check the GUIDE project's widget limits agree with these.
#
# The two delays are offsets and legitimately go NEGATIVE. Any "nothing can be
# below zero" assumption would silently destroy half of every operator's range.
DEFAULT_VARIABLE_RANGES = {
    "roller_speed":       {"min": 0,    "max": 250},
    "belt_speed":         {"min": 0,    "max": 20},
    # +-20, not +-100. The sketch multiplies these by 100 to get milliseconds
    # (see user_roller_start_mod_value in tabletop_seeder_photoeye.ino), so one
    # unit is 100 ms and this range is +-2 seconds. The old +-100 gave the
    # operator +-10 seconds of authority over roller timing, which is far more
    # than the adjustment is for and makes the encoder fiddly to dial in.
    #
    # If you widen this, widen the screen 16 and 40 widget limits in GUIDE to
    # match — a value the encoder allows but this file rejects is refused on
    # save, and the only sign of it is a REFUSED line in the journal that
    # nobody at the machine is reading.
    "roller_start_delay": {"min": -20, "max": 20},
    "roller_stop_delay":  {"min": -20, "max": 20},
}

# Irrigation and misting timing. The machine HAS both, but they are deliberately
# not on the Touch Encoder: operators only ever adjust belt speed, roller speed
# and the two roller offsets, and every extra screen makes the menu worse for
# the people who never touch the rest.
#
# They live here rather than compiled into the ClearCore sketch so they can be
# changed over Tailscale in seconds. A value baked into firmware needs somebody
# on site with a laptop and the Arduino toolchain, which for a machine in Dubai
# is a different order of problem entirely.
#
# These are MODIFIERS, not absolute times. The sketch assigns them to
# user_irrigation_start_mod_value and friends and adds them to its own computed
# timings, so 0 means "no offset" — the sketch's built-in behaviour, which is
# the right default for a machine nobody has tuned yet. They are NOT range
# checked, because they are not operator input.
DEFAULT_FIXED_TIMINGS = {
    "irrigation_delay": 0,
    "irrigation_duration": 0,
    "misting_delay": 0,
    "misting_duration": 0,
}

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
                default_names = {}
                for i in range(1, NUM_VARIETIES + 1):
                    default_names[str(i)] = str(i)
                default_names["1"] = "Broccoli"
                default_names["2"] = "Arugula"
                default_names["3"] = "Radish"
                with open(path, "w") as f:
                    json.dump({
                        "ready_to_run": False,
                        "active_variety": None,
                        "variable_ranges": dict(DEFAULT_VARIABLE_RANGES),
                        "fixed_timings": dict(DEFAULT_FIXED_TIMINGS),
                        "variety_names": default_names,
                    }, f, indent=4)

# ========================
# Surviving power loss
# ========================
# This machine's power can disappear mid-write — an e-stop that cuts the Pi, a
# breaker, someone pulling the plug. The helpers below are what stand between
# that and an operator's presets. They came from the Freshleaf seeder after an
# e-stop corrupted two files on its SD card; the reasoning is kept with them so
# it survives the next person who wonders why a simple save is this careful.

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

def quarantine_corrupt_json(path: str):
    """
    Preserve a copy of an unparseable file, once.

    Copies rather than moves: the machine has to keep running, and the callers
    all treat a missing file as "start fresh". Only the FIRST corruption is kept
    — a later good save followed by another corruption would otherwise overwrite
    the copy that still had the operator's data in it.
    """
    backup = path + ".corrupt"
    try:
        if os.path.exists(backup):
            print(f"WARNING: {path} is unreadable; existing copy at {backup}")
            return
        with open(path, "rb") as src, open(backup, "wb") as dst:
            dst.write(src.read())
        print(f"WARNING: {path} is unreadable — stored values may be lost. "
              f"A copy was kept at {backup} for recovery.")
    except OSError as e:
        print(f"WARNING: {path} is unreadable and could not be copied: {e}")

def backup_settings(path: str):
    """
    Keep a known-good copy of a settings file alongside it.

    Written after a successful save — the only time content changes — so the
    copy always holds a complete, parseable set. Costs one extra small write per
    operator save, which is nothing next to the telemetry stream, and it is the
    difference between "presets are gone" and "presets are one cp away".

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

    backup_settings() otherwise only runs after an operator saves, so a machine
    nobody has saved on has no .bak at all — and recover_settings_if_needed()
    below then has nothing to restore from. The recovery path exists but is
    disarmed, which is exactly the state the Freshleaf machine was found in
    after an SD corruption: settings intact by luck, no backup, no way to tell.

    Only when the backup is MISSING. An existing .bak is the last state an
    operator explicitly saved; refreshing it every boot from whatever happens to
    be on disk would let a damaged-but-loadable file quietly replace a good copy.
    """
    backup = path + ".bak"
    if os.path.exists(backup) or not os.path.exists(path):
        return
    print(f"no backup at {backup}; creating one from the current file")
    backup_settings(path)

def recover_settings_if_needed(path: str, writer):
    """
    Restore from the backup if the live file is unreadable.

    Runs once at startup rather than from the read path: recovery needs an
    exclusive lock and a well-defined moment, and the machine is not yet doing
    anything. Without this an unattended machine silently comes up with nothing
    and nobody finds out until an operator goes looking.

    `writer` is the locked atomic write for THIS file — the settings and the PIN
    store have separate locks, and restoring one while holding the other's lock
    would be a real (if rare) way to corrupt the thing we are repairing.
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
              f"{backup}. Its contents will need to be re-entered.")
        quarantine_corrupt_json(path)
        return

    quarantine_corrupt_json(path)
    writer(path, restored)
    entries = len([k for k in restored if str(k).isdigit()])
    print(f"RECOVERED {path} from {backup} ({entries} stored entries restored)")

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
            # Without this the rename can still be lost to a power cut seconds
            # after the operator saw "Saved".
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

def load_variable_ranges(data: Dict) -> Dict:
    """
    Return the min/max declared for each variety field by the settings file.

    Read from the file rather than from a constant so a machine whose limits
    were tuned on site is checked against ITS limits. Returns {} when the file
    predates the ranges block, which disables the checks below — the right
    failure direction, since inventing bounds risks rejecting presets an
    operator legitimately saved.
    """
    ranges = data.get("variable_ranges")
    return ranges if isinstance(ranges, dict) else {}

def ensure_variable_ranges(path: str):
    """
    Seed the ranges block into a settings file that has none.

    ensure_json_exists() only writes variable_ranges when it CREATES the file,
    so any machine whose settings predate that block never gets one — and every
    range check silently does nothing, because load_variable_ranges() returns {}
    and preset_issues() then has nothing to check against. The refuse-on-save
    guard and the startup audit both look present and are both inert.

    That is exactly the state the refarm seeder was found in after switching to
    this code: its settings file came from the tabletop era, so a save of any
    value at all would have been accepted without complaint.

    Only when the block is MISSING. An existing one is this machine's tuned
    configuration and outranks the defaults compiled in here — the whole point
    of reading ranges from the file is that a machine can disagree with the
    constant. Merging per-field would quietly re-impose factory limits on a
    field somebody had deliberately widened.
    """
    data = locked_read_json(path) or {}
    if not data:
        return  # nothing to add ranges to; ensure_json_exists() handles creation
    if isinstance(data.get("variable_ranges"), dict):
        return  # already configured, leave it alone

    data["variable_ranges"] = dict(DEFAULT_VARIABLE_RANGES)
    locked_atomic_write_json(path, data)
    print(f"seeded variable_ranges into {path} — range checks are now active "
          f"({', '.join(sorted(DEFAULT_VARIABLE_RANGES))})")

def ensure_fixed_timings(path: str):
    """
    Seed the irrigation/misting block into a settings file that has none.

    Same reasoning as ensure_variable_ranges(): the block is only written when
    the file is CREATED, so any machine whose settings predate it would have
    the TCP server fall back to zeros for four fields the ClearCore genuinely
    reads and acts on.

    Only when the block is MISSING. Values tuned on site outrank the defaults
    compiled in here — that is the whole point of keeping them in the file
    rather than in the sketch.
    """
    data = locked_read_json(path) or {}
    if not data:
        return  # nothing to add to; ensure_json_exists() handles creation
    if isinstance(data.get("fixed_timings"), dict):
        return  # already configured, leave it alone

    data["fixed_timings"] = dict(DEFAULT_FIXED_TIMINGS)
    locked_atomic_write_json(path, data)
    print(f"seeded fixed_timings into {path} — "
          f"{', '.join(f'{k}={v}' for k, v in sorted(DEFAULT_FIXED_TIMINGS.items()))}. "
          f"These are not adjustable from the encoder; edit the file to change them.")

def preset_issues(values: Dict, ranges: Dict) -> list:
    """
    Return human-readable problems with one variety's values.

    Only fields the settings file declares a range for are checked, and only
    against that declared range. Note the two delays are offsets and
    legitimately go negative (-20..20), so the sign of a value says nothing
    on its own — a "reject anything below zero" rule would be wrong here.
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

    Damaged storage does not always produce unparseable JSON — a corrupted
    block can land inside a number and leave a file that loads cleanly and
    carries a value nothing else would question. This only reports; it never
    edits. The operator re-saves the variety, which is the one action that can
    produce a correct value.
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
    belt_speed: int,
    roller_start_delay: int,
    roller_stop_delay: int,
):
    data = locked_read_json(JSON_FILE_PATH) or {}
    key = str(int(variety_index))
    # No clamp on roller_stop_delay. The harvester pinned this field to 0-3
    # because it was an airknife MODE — an enum, where anything else was
    # meaningless. Here it is a signed offset in -20..+20, and the old clamp
    # would have turned every negative value into 0 and capped the rest at 3,
    # on save, with nothing in the log to say why the machine ignored what the
    # operator entered.
    values = {
        "roller_speed": int(roller_speed),
        "belt_speed": int(belt_speed),
        "roller_start_delay": int(roller_start_delay),
        "roller_stop_delay": int(roller_stop_delay),
    }

    # Refuse rather than clamp. A clamp writes a value the operator did not
    # choose and says nothing; refusing leaves the last good preset in place and
    # puts the reason in the journal. An out-of-range read here means a bad
    # encoder read or a GUIDE widget whose limits disagree with this file —
    # both worth seeing, neither worth silently persisting.
    issues = preset_issues(values, load_variable_ranges(data))
    if issues:
        print(f"REFUSED to save variety {key} — {'; '.join(issues)}. "
              f"Previous values kept.")
        return

    data[key] = values
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
    unclean shutdown this whole screen exists to avoid. Say so in the journal.
    """
    try:
        os.makedirs(os.path.dirname(SHUTDOWN_REQUEST_PATH), exist_ok=True)
        with open(SHUTDOWN_REQUEST_PATH, "w") as f:
            f.write("requested from the Touch Encoder shutdown screen\n")
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
    Restores: active_variety, belt_speed, roller_speed, and roller_start_delay.
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

    # Restore active_variety (screen 10, var 1) and write name to display
    set_variable(10, 1, active_variety)
    write_variety_to_screen(active_variety)
    
    # Restore variety settings
    set_variable(6, 1, v.get("roller_speed", 0))        # Roller Speed
    set_variable(3, 1, v.get("belt_speed", 0))          # Belt Speed
    set_variable(16, 1, v.get("roller_start_delay", 0))      # Roller Start Delay
    set_variable(40, 1, v.get("roller_stop_delay", 0))     # Roller Stop Delay

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
            # Same reasoning as the settings file, and it matters MORE here: a
            # damaged PIN store reads as {} and ensure_pin_json_exists() would
            # then reset the machine to the factory PIN. Keep the evidence.
            quarantine_corrupt_json(path)
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
            # A PIN change that survives the operator walking away but not the
            # next power cut is worse than one that never appeared to save.
            fsync_dir(dir_name)
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
                # Write variety name before navigating to screen 26
                # to prevent flash of default string
                write_variety_to_screen(1, EDIT_VARIETY_SCREEN, EDIT_VARIETY_VAR)
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
        
        # Write variety name before navigating to prevent flash of default string
        write_variety_to_screen(1)

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


    # Variety scrolling state
    current_variety_index = 1       # 1-based index into variety slots
    last_screen = None              # Track screen transitions

    # Latches once the operator has asked to shut down. The loop runs every
    # 300 ms and the machine takes a few seconds to halt, so without this the
    # request would be re-issued a dozen times on the way down.
    shutdown_requested = False

    # Run-state cache for screen 18. None means "was not on the run screen last
    # tick", which doubles as the just-arrived signal for the stale-press guard.
    last_state_running = None

    # Write initial variety name to screen 10
    write_variety_to_screen(current_variety_index)

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
        # Screen 4: Shut Down
        # ---------------------------------------------
        # Arriving here IS the request — the GUIDE button navigates to this
        # screen and that is the whole gesture. Checked before any other screen
        # handling so the halt is not delayed behind encoder round-trips, and
        # `continue` because nothing else is worth doing on the way down.
        if active_screen == ScreenID(SHUTDOWN_SCREEN):
            if not shutdown_requested:
                shutdown_requested = True
                print("shutdown: operator opened the shutdown screen")
                # Park the machine first. The TCP server keeps streaming this
                # JSON to the ClearCore until the moment the OS goes away, so
                # the last thing the controller reads should say "not ready to
                # run" rather than leaving it armed as the link disappears.
                ready_to_run_toggle(False)
                request_shutdown()
            time.sleep(POLL_INTERVAL_SEC)
            continue

        # Detect screen transitions - write variety name when arriving at screen 10 or 26
        if active_screen == ScreenID(VARIETY_NAME_SCREEN) and last_screen != ScreenID(VARIETY_NAME_SCREEN):
            current_variety_index = 1
            write_variety_to_screen(current_variety_index)
            set_variable(VARIETY_NAME_SCREEN, 1, current_variety_index)
        if active_screen == ScreenID(EDIT_VARIETY_SCREEN) and last_screen != ScreenID(EDIT_VARIETY_SCREEN):
            current_variety_index = 1
            write_variety_to_screen(current_variety_index, EDIT_VARIETY_SCREEN, EDIT_VARIETY_VAR)
            set_variable(EDIT_VARIETY_SCREEN, 1, current_variety_index)
        last_screen = active_screen


        # ---------------------------------------------
        # Screen 17: Operator Mode - Variety Selection Confirmation
        # ---------------------------------------------
        if active_screen == ScreenID(17):
            variety_index = get_variable(10, 1)
            save_active_variety(variety_index)  # Save active variety to JSON

            saved_data = load_variety_data()

            # Display which variety is loaded on screen 18, by NAME on var 1.
            #
            # The harvester also wrote the numeric index to var 2 here. On this
            # machine var 2 carries the Start/Stop button's static label, so
            # that write stamped a number over it — the button read "0" instead
            # of its text. Var 1 already shows the variety, so nothing is lost.
            #
            # Screen 18's variables on this machine:
            #   var 1 -> variety name (written here)
            #   var 2 -> button label, GUIDE-owned — POLL MUST NOT WRITE IT
            #   var 6 -> press latch (written by the run-screen branch)
            write_variety_to_screen(variety_index, CONFIRM_VARIETY_SCREEN, CONFIRM_VARIETY_VAR)

            key = str(variety_index)
            if key in saved_data and isinstance(saved_data[key], dict):
                saved_values = saved_data[key]

                # Push saved values into the encoder
                set_variable(6, 1, saved_values.get("roller_speed", 0))        # Roller Speed
                set_variable(3, 1, saved_values.get("belt_speed", 0))          # Belt Speed
                set_variable(16, 1, saved_values.get("roller_start_delay", 0))    # Roller Start Delay
                set_variable(40, 1, saved_values.get("roller_stop_delay", 0))    # Roller Stop Delay

                # Deliberately does NOT arm the machine. Selecting a variety is
                # a setup action; the Start/Stop button on screen 18 (var 6) is
                # the only thing that sets ready_to_run true. Arming here would
                # mean the belt goes live as a side effect of menu navigation,
                # and the operator's first press of Start would actually stop
                # it — which reads as a broken button.
            else:
                print(f"Variety {variety_index} not found. Waiting for user to define it.")

            time.sleep(2)
            # Variety selected confirmation screen
            te.guide.set_screen(ScreenID(18))

        # ---------------------------------------------
        # Screen 18: Run screen — Start/Stop belt
        # ---------------------------------------------
        if active_screen == ScreenID(STATE_SCREEN):
            # Zero the latch on arrival and skip one read. A press left
            # unconsumed from an earlier visit would otherwise fire the instant
            # the operator navigates back here — starting the belt on its own,
            # which is the one thing this screen must never do by surprise.
            first_entry = last_state_running is None
            if first_entry:
                set_variable(STATE_SCREEN, STATE_BTN_PRESS_VAR, 0)

            # ready_to_run in the JSON is the single source of truth, not the
            # encoder. Reading it fresh each cycle rather than tracking a local
            # flag is what makes the button behave correctly after a poll
            # restart, a TE reconnect, or a change made anywhere else.
            data = locked_read_json(JSON_FILE_PATH) or {}
            running = bool(data.get("ready_to_run", False))
            last_state_running = running

            try:
                pressed = (0 if first_entry
                           else safe_get_var(STATE_SCREEN, STATE_BTN_PRESS_VAR))
            except Exception:
                pressed = 0

            if pressed:
                # Consume the press FIRST, always. If the write below throws,
                # the latch must not stay high — it would re-toggle every
                # 300 ms and the belt would stutter on and off.
                set_variable(STATE_SCREEN, STATE_BTN_PRESS_VAR, 0)
                ready_to_run_toggle(not running)
                last_state_running = not running
        else:
            # Left the run screen — drop the cache so re-entry runs the
            # stale-press guard again.
            last_state_running = None

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
                set_variable(6, 1, saved_values.get("roller_speed", 0))        # Roller Speed
                set_variable(3, 1, saved_values.get("belt_speed", 0))          # Belt Speed
                set_variable(16, 1, saved_values.get("roller_start_delay", 0))    # Roller Start Delay
                set_variable(40, 1, saved_values.get("roller_stop_delay", 0))    # Roller Stop Delay


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
            roller_speed = get_variable(6, 1)       # roller speed
            belt_speed = get_variable(3, 1)         # belt speed
            roller_start_delay = get_variable(16, 1)     # roller start delay
            roller_stop_delay = get_variable(40, 1)    # roller stop delay (-20..+20, one unit = 100 ms)

            if None not in (
                variety_index,
                roller_speed,
                belt_speed,
                roller_start_delay,
                roller_stop_delay,
            ):
                # Persist the variety data into JSON with locking
                save_variety_data(
                    variety_index,
                    roller_speed,
                    belt_speed,
                    roller_start_delay,
                    roller_stop_delay,
                )
                time.sleep(2)
                # Write variety name before navigating to prevent flash of default string
                write_variety_to_screen(1)
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


        # Wait for knob events (replaces time.sleep - acts as sleep when no event)
        event = te.await_res(expected_res=[GuideKnobEventReport], timeout=POLL_INTERVAL_SEC)
        if event is not None:
            # Knob turned - scroll varieties if on screen 10 or 26
            if event.relative_value > 0:
                current_variety_index = (current_variety_index % NUM_VARIETIES) + 1
            elif event.relative_value < 0:
                current_variety_index = ((current_variety_index - 2) % NUM_VARIETIES) + 1

            if active_screen == ScreenID(VARIETY_NAME_SCREEN):
                write_variety_to_screen(current_variety_index)
                set_variable(VARIETY_NAME_SCREEN, 1, current_variety_index)
            elif active_screen == ScreenID(EDIT_VARIETY_SCREEN):
                write_variety_to_screen(current_variety_index, EDIT_VARIETY_SCREEN, EDIT_VARIETY_VAR)
                set_variable(EDIT_VARIETY_SCREEN, 1, current_variety_index)

# ========================
# Main
# ========================
def main():
    global te

    print("DEBUG: Starting main()")
    ensure_json_exists(JSON_FILE_PATH)
    ensure_pin_json_exists(JSON_PIN_FILE_PATH)
    print("DEBUG: JSON files ensured")

    # Repair before anything reads or writes. Order matters: recovery has to run
    # before the first write to either file, or that write bakes an empty
    # structure over damage the backup could still have undone.
    #
    # The PIN store is recovered too, and it is the one that fails worst — a
    # damaged PIN file reads as {} and ensure_pin_json_exists() resets the
    # machine to the factory PIN, silently unlocking a machine that was locked.
    recover_settings_if_needed(JSON_FILE_PATH, locked_atomic_write_json)
    recover_settings_if_needed(JSON_PIN_FILE_PATH, locked_atomic_write_pin_json)

    # Arm the recovery path for next time. Without this a machine nobody has
    # saved on carries no .bak at all, and the code above has nothing to work
    # with — recovery that exists but is disarmed.
    ensure_settings_backup(JSON_FILE_PATH)
    ensure_settings_backup(JSON_PIN_FILE_PATH)

    # Must run BEFORE the audit: without a ranges block there is nothing to
    # check against, and both the audit and the refuse-on-save guard quietly do
    # nothing at all.
    ensure_variable_ranges(JSON_FILE_PATH)

    # Irrigation/misting timings the encoder has no screen for. The TCP server
    # streams these to the ClearCore every cycle, so a missing block means four
    # fields silently fall back to zero.
    ensure_fixed_timings(JSON_FILE_PATH)

    # Report presets that load fine but hold impossible values — the damage
    # that parses. Reports only; the operator re-saving is the fix.
    audit_stored_presets(JSON_FILE_PATH)

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
                    # Write variety name before navigating to prevent flash of default string
                    write_variety_to_screen(1)
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

