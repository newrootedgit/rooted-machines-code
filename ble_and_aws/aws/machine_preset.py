import fcntl
import json
import os
import tempfile

PRESETS_FILE = '/home/rooted/te-cli/TE_Variable_Values.json'
LOCK_FILE_PATH = PRESETS_FILE + ".lock"


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


def load_presets() -> dict:
    """Load current presets from the TE_Variable_Values.json file."""
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(PRESETS_FILE, "r") as f:
                return json.load(f)
        except (FileNotFoundError, json.JSONDecodeError) as e:
            print(f"Error loading presets: {e}")
            return {}


def save_presets(presets: dict) -> bool:
    """Atomically write updated presets to the TE_Variable_Values.json file."""
    with FileLock(LOCK_FILE_PATH, shared=False):
        dir_name = os.path.dirname(PRESETS_FILE)
        os.makedirs(dir_name, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".tmp_", dir=dir_name)
        try:
            with os.fdopen(fd, "w") as tmpf:
                json.dump(presets, tmpf, indent=4)
                tmpf.flush()
                os.fsync(tmpf.fileno())
            os.replace(tmp_path, PRESETS_FILE)
            print(f"Saved presets to {PRESETS_FILE}")
            return True
        except Exception as e:
            print(f"Error saving presets: {e}")
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
            return False
